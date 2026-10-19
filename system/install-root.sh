#!/bin/bash
# Root half of the omaFace installer. Called by install.sh through sudo:
#   install-root.sh <repo> <build-dir> <models-dir> <login: yes|no> <webcam: yes|no>
# Safe to re-run: every step is idempotent and PAM edits are made once.

set -euo pipefail

REPO=$1
BUILD=$2
MODELS=$3
LOGIN=${4:-yes}
WEBCAM=${5:-no}

LIB=/usr/local/lib/omaface
SHARE=/usr/local/share/omaface
SRC=/usr/local/src/omaface
PAM_BACKUP=/var/lib/omaface/pam-backup
PAM_LINE="pam_exec.so quiet /usr/local/lib/omaface/omaface-pam"
MARK="# omaface: omaFace first, then the usual password (omaface uninstall removes this)"

(( EUID == 0 )) || { echo "install-root.sh must run as root" >&2; exit 1; }
source "$REPO/system/pam.sh"

# An earlier install under the old name (faceid): its own uninstaller takes
# its system parts out (PAM lines, service, theme), keeping enrolled faces and
# settings, which then move to the new paths.
if [[ -x /usr/local/lib/faceid/uninstall ]]; then
  echo "  replacing the earlier install (faceid); faces and settings are kept"
  /usr/local/lib/faceid/uninstall --keep-data >/dev/null
  if [[ -d /var/lib/faceid && ! -e /var/lib/omaface ]]; then mv /var/lib/faceid /var/lib/omaface; fi
  if [[ -d /etc/faceid && ! -e /etc/omaface ]]; then mv /etc/faceid /etc/omaface; fi
fi

echo "  binaries, models, command"
install -d -m0755 "$LIB" "$SHARE" "$SHARE/models" "$SRC" /etc/omaface
install -m0755 "$BUILD/engine/omaface-engine" "$LIB/omaface-engine"
install -m0755 "$BUILD/engine/omaface-pam" "$LIB/omaface-pam"
install -m0755 "$REPO/system/rebuild" "$LIB/rebuild"
install -m0755 "$REPO/system/uninstall-root.sh" "$LIB/uninstall"
install -m0644 "$REPO/system/pam.sh" "$LIB/pam.sh"
install -m0755 "$REPO/bin/omaface" /usr/local/bin/omaface
install -m0644 "$MODELS/face_detection_yunet_2023mar.onnx" "$MODELS/face_recognition_sface_2021dec.onnx" \
  "$REPO/models/SHA256SUMS" "$SHARE/models/"
(cd "$SHARE/models" && sha256sum -c --quiet SHA256SUMS)

# Sources for the OpenCV rebuild hook and for `omaface sync`.
rm -rf "$SRC/engine" "$SHARE/plugin" "$SHARE/tools"
install -d -m0755 "$SRC/engine" "$SHARE/plugin" "$SHARE/tools"
install -m0644 "$REPO/engine/omaface-engine.cpp" "$REPO/engine/omaface-pam.c" "$REPO/engine/Makefile" "$SRC/engine/"
install -m0644 "$REPO"/plugin/*.qml "$REPO/plugin/manifest.json" "$SHARE/plugin/"
install -m0644 "$REPO/tools/patches.py" "$SHARE/tools/"
install -m0755 "$REPO/tools/build-plugin.py" "$SHARE/tools/"
if [[ $LOGIN == yes ]]; then
  echo "  login screen theme"
  if [[ -f /usr/share/sddm/themes/omarchy/Main.qml ]]; then
    sha256sum /usr/share/sddm/themes/omarchy/Main.qml | cut -d' ' -f1 >"$SHARE/sddm-upstream.sha256"
  fi
  rm -rf /usr/share/sddm/themes/omaface
  install -d -m0755 /usr/share/sddm/themes/omaface
  install -m0644 "$REPO"/sddm/* /usr/share/sddm/themes/omaface/
fi

echo "  configuration (kept if it already exists)"
[[ -f /etc/omaface/config ]] || install -m0644 "$REPO/system/config" /etc/omaface/config
set_conf() {
  if grep -qE "^[[:space:]]*$1[[:space:]]*=" /etc/omaface/config; then
    sed -i "s/^[[:space:]]*$1[[:space:]]*=.*/$1=$2/" /etc/omaface/config
  else
    echo "$1=$2" >>/etc/omaface/config
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
install -d -m0755 /var/lib/omaface

echo "  service, polkit action, pacman hook"
install -m0644 "$REPO/system/omaface.socket" "$REPO/system/omaface@.service" /etc/systemd/system/
install -m0644 "$REPO/system/omaface.tmpfiles" /etc/tmpfiles.d/omaface.conf
install -m0644 "$REPO/system/omaface.policy" /usr/share/polkit-1/actions/local.omaface.policy
install -d -m0755 /etc/pacman.d/hooks
install -m0644 "$REPO/system/omaface.hook" /etc/pacman.d/hooks/omaface.hook
systemd-tmpfiles --create /etc/tmpfiles.d/omaface.conf
systemctl daemon-reload
systemctl enable --now omaface.socket >/dev/null

# opencv only arrived as a dependency of frei0r-plugins; an orphan cleanup
# would take it, and the omaFace engine with it.
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
# One omaFace line right above the password, so success skips the password
# prompt and anything else falls straight through to it.
insert_before() {
  local file=$1 args=$2
  shift 2
  grep -q omaface-pam "$file" && return 0
  local anchor
  anchor=$(pam_anchor "$file" "$@") || { echo "unexpected layout in $file; not changed" >&2; return 1; }
  local tmp
  tmp=$(mktemp "$file.omaface.XXXXXX")
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
  # At the login screen pam_exec passes on what was typed; omaface-pam only
  # looks at the camera when the field was empty.
  insert_before /etc/pam.d/sddm "pam_exec.so quiet expose_authtok /usr/local/lib/omaface/omaface-pam" \
    "${PAM_ANCHORS_SDDM[@]}"
fi

cat >/etc/pam.d/omaface-lock <<'EOF'
#%PAM-1.0
# omaFace for the Omarchy lock screen (io.github.l0adn9.omaface). Managed by omaface.
auth       required                    pam_exec.so quiet /usr/local/lib/omaface/omaface-pam
account    include                     system-local-login
EOF
chmod 0644 /etc/pam.d/omaface-lock

# The login theme is selected once a face exists (omaface enroll does it).
owner=${SUDO_USER:-$(getent passwd "${PKEXEC_UID:-65534}" | cut -d: -f1)}
if [[ $LOGIN == yes ]] && grep -qE '^login=on' /etc/omaface/config && [[ -s /var/lib/omaface/${owner:-none}/faces.yml ]]; then
  printf '# omaFace login theme (omaface disable login reverts to Omarchy'"'"'s theme)\n[Theme]\nCurrent=omaface\n' \
    >/etc/sddm.conf.d/20-omaface.conf
fi

echo "  done"
