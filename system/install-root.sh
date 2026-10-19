#!/bin/bash
# Root half of the Face ID installer. Called by install.sh through sudo:
#   install-root.sh <repo> <build-dir> <models-dir> <login: yes|no> <webcam: yes|no>
# Safe to re-run: every step is idempotent and PAM edits are made once.

set -euo pipefail

REPO=$1
BUILD=$2
MODELS=$3
LOGIN=${4:-yes}
WEBCAM=${5:-no}

LIB=/usr/local/lib/faceid
SHARE=/usr/local/share/faceid
SRC=/usr/local/src/faceid
PAM_BACKUP=/var/lib/faceid/pam-backup
PAM_LINE="pam_exec.so quiet /usr/local/lib/faceid/faceid-pam"
MARK="# faceid: Face ID first, then the usual password (faceid uninstall removes this)"

(( EUID == 0 )) || { echo "install-root.sh must run as root" >&2; exit 1; }
source "$REPO/system/pam.sh"

echo "  binaries, models, command"
install -d -m0755 "$LIB" "$SHARE" "$SHARE/models" "$SRC" /etc/faceid
install -m0755 "$BUILD/engine/faceid-engine" "$LIB/faceid-engine"
install -m0755 "$BUILD/engine/faceid-pam" "$LIB/faceid-pam"
install -m0755 "$REPO/system/rebuild" "$LIB/rebuild"
install -m0755 "$REPO/system/uninstall-root.sh" "$LIB/uninstall"
install -m0644 "$REPO/system/pam.sh" "$LIB/pam.sh"
install -m0755 "$REPO/bin/faceid" /usr/local/bin/faceid
install -m0644 "$MODELS/face_detection_yunet_2023mar.onnx" "$MODELS/face_recognition_sface_2021dec.onnx" \
  "$REPO/models/SHA256SUMS" "$SHARE/models/"
(cd "$SHARE/models" && sha256sum -c --quiet SHA256SUMS)

# Sources for the OpenCV rebuild hook and for `faceid sync`.
rm -rf "$SRC/engine" "$SHARE/plugin" "$SHARE/tools"
install -d -m0755 "$SRC/engine" "$SHARE/plugin" "$SHARE/tools"
install -m0644 "$REPO/engine/faceid-engine.cpp" "$REPO/engine/faceid-pam.c" "$REPO/engine/Makefile" "$SRC/engine/"
install -m0644 "$REPO"/plugin/*.qml "$REPO/plugin/manifest.json" "$SHARE/plugin/"
install -m0644 "$REPO/tools/patches.py" "$SHARE/tools/"
install -m0755 "$REPO/tools/build-plugin.py" "$SHARE/tools/"
if [[ $LOGIN == yes ]]; then
  echo "  login screen theme"
  if [[ -f /usr/share/sddm/themes/omarchy/Main.qml ]]; then
    sha256sum /usr/share/sddm/themes/omarchy/Main.qml | cut -d' ' -f1 >"$SHARE/sddm-upstream.sha256"
  fi
  rm -rf /usr/share/sddm/themes/omarchy-faceid
  install -d -m0755 /usr/share/sddm/themes/omarchy-faceid
  install -m0644 "$REPO"/sddm/* /usr/share/sddm/themes/omarchy-faceid/
fi

echo "  configuration (kept if it already exists)"
[[ -f /etc/faceid/config ]] || install -m0644 "$REPO/system/config" /etc/faceid/config
set_conf() {
  if grep -qE "^[[:space:]]*$1[[:space:]]*=" /etc/faceid/config; then
    sed -i "s/^[[:space:]]*$1[[:space:]]*=.*/$1=$2/" /etc/faceid/config
  else
    echo "$1=$2" >>/etc/faceid/config
  fi
}
if [[ $LOGIN != yes ]]; then
  # No SDDM greeter on this machine: nothing to show a face to at login.
  set_conf login off
fi
if [[ $WEBCAM == yes ]]; then
  # No IR camera: the regular webcam, for the lock screen only (the engine
  # enforces that too).
  set_conf rgb_fallback on
  set_conf sudo off
  set_conf polkit off
  set_conf login off
fi
install -d -m0755 /var/lib/faceid

echo "  service, polkit action, pacman hook"
install -m0644 "$REPO/system/faceid.socket" "$REPO/system/faceid@.service" /etc/systemd/system/
install -m0644 "$REPO/system/faceid.tmpfiles" /etc/tmpfiles.d/faceid.conf
install -m0644 "$REPO/system/faceid.policy" /usr/share/polkit-1/actions/local.faceid.policy
install -d -m0755 /etc/pacman.d/hooks
install -m0644 "$REPO/system/faceid.hook" /etc/pacman.d/hooks/faceid.hook
systemd-tmpfiles --create /etc/tmpfiles.d/faceid.conf
systemctl daemon-reload
systemctl enable --now faceid.socket >/dev/null

# opencv only arrived as a dependency of frei0r-plugins; an orphan cleanup
# would take it, and the Face ID engine with it.
if ! pacman -Qi opencv | grep -q 'Install Reason *: Explicitly'; then
  echo "  marking opencv as explicitly installed (keeps it through orphan cleanups)"
  pacman -D --asexplicit opencv >/dev/null
fi

echo "  PAM"
install -d -m0700 "$PAM_BACKUP"

# polkit keeps its stack in /usr/lib/pam.d unless something (Omarchy's
# fingerprint setup) made an /etc copy. Make one from the vendor file, and
# remember that we did, so uninstall can take it away again.
if [[ ! -f /etc/pam.d/polkit-1 ]]; then
  src=$(polkit_pam_file) || { echo "polkit has no PAM configuration" >&2; exit 1; }
  install -m0644 "$src" /etc/pam.d/polkit-1
  touch "$PAM_BACKUP/polkit-1.created"
fi

for f in sudo polkit-1 sddm; do
  [[ -f /etc/pam.d/$f ]] || continue
  [[ -f $PAM_BACKUP/$f ]] || cp -a "/etc/pam.d/$f" "$PAM_BACKUP/$f"
done

# insert_before <file> <module args> <anchor regex...>
# One Face ID line right above the password, so success skips the password
# prompt and anything else falls straight through to it.
insert_before() {
  local file=$1 args=$2
  shift 2
  grep -q faceid-pam "$file" && return 0
  local anchor
  anchor=$(pam_anchor "$file" "$@") || { echo "unexpected layout in $file; not changed" >&2; return 1; }
  local tmp
  tmp=$(mktemp "$file.faceid.XXXXXX")
  awk -v anchor="$anchor" -v mark="$MARK" -v line="auth      sufficient  $args" '
    $0 ~ anchor && !done { print mark; print line; done = 1 }
    { print }
  ' "$file" >"$tmp"
  chmod --reference="$file" "$tmp"
  chown --reference="$file" "$tmp"
  mv "$tmp" "$file"
}

insert_before /etc/pam.d/sudo "$PAM_LINE" "${PAM_ANCHORS_SUDO[@]}"
insert_before /etc/pam.d/polkit-1 "$PAM_LINE" "${PAM_ANCHORS_POLKIT[@]}"
if [[ $LOGIN == yes ]]; then
  # At the login screen pam_exec passes on what was typed; faceid-pam only
  # looks at the camera when the field was empty.
  insert_before /etc/pam.d/sddm "pam_exec.so quiet expose_authtok /usr/local/lib/faceid/faceid-pam" \
    "${PAM_ANCHORS_SDDM[@]}"
fi

cat >/etc/pam.d/omarchy-lock-face <<'EOF'
#%PAM-1.0
# Face ID for the Omarchy lock screen (l0adn9.faceid). Managed by faceid.
auth       required                    pam_exec.so quiet /usr/local/lib/faceid/faceid-pam
account    include                     system-local-login
EOF
chmod 0644 /etc/pam.d/omarchy-lock-face

# The login theme is selected once a face exists (faceid enroll does it).
owner=${SUDO_USER:-$(getent passwd "${PKEXEC_UID:-65534}" | cut -d: -f1)}
if [[ $LOGIN == yes ]] && grep -qE '^login=on' /etc/faceid/config && [[ -s /var/lib/faceid/${owner:-none}/faces.yml ]]; then
  printf '# Face ID login theme (faceid disable login reverts to Omarchy'"'"'s theme)\n[Theme]\nCurrent=omarchy-faceid\n' \
    >/etc/sddm.conf.d/20-faceid.conf
fi

echo "  done"
