#!/bin/bash
# Root half of `omaface setup`: settings, the service, and one omaFace line
# above the password in each PAM stack. Run through the omaface command:
#   setup-root <login: yes|no> <webcam: yes|no>
# Safe to re-run: every step is idempotent and PAM edits are made once.
# `omaface uninstall` (teardown) reverses it.

set -euo pipefail

LOGIN=${1:-no}
WEBCAM=${2:-no}

LIB=@PREFIX@/lib/omaface
SHARE=@PREFIX@/share/omaface
CONF=/etc/omaface/config
PAM_BACKUP=/var/lib/omaface/pam-backup
PAM_LINE="pam_exec.so quiet $LIB/omaface-pam"
MARK="# omaface: omaFace first, then the usual password (omaface uninstall removes this)"

(( EUID == 0 )) || { echo "run through: omaface setup" >&2; exit 1; }
source "$LIB/pam.sh"

echo "  settings (kept if they already exist)"
install -d -m0755 /etc/omaface /var/lib/omaface
[[ -f $CONF ]] || install -m0644 "$SHARE/config" "$CONF"
set_conf() {
  if grep -qE "^[[:space:]]*$1[[:space:]]*=" "$CONF"; then
    sed -i "s/^[[:space:]]*$1[[:space:]]*=.*/$1=$2/" "$CONF"
  else
    echo "$1=$2" >>"$CONF"
  fi
}
if [[ $LOGIN != yes ]]; then
  # No SDDM greeter on this machine: nothing to show a face to at login.
  set_conf login off
fi
if [[ $WEBCAM == yes ]]; then
  # No IR camera: the regular webcam, everywhere, with its liveness checks.
  set_conf rgb_fallback on
fi

echo "  service"
systemd-tmpfiles --create omaface.conf
systemctl daemon-reload
systemctl enable --now omaface.socket >/dev/null

echo "  PAM"
install -d -m0700 "$PAM_BACKUP"

# polkit keeps its stack in /usr/lib/pam.d unless something (Omarchy's
# fingerprint setup) made an /etc copy. Make one from the vendor file, and
# remember that we did, so teardown can take it away again.
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
  insert_before /etc/pam.d/sddm "pam_exec.so quiet expose_authtok $LIB/omaface-pam" \
    "${PAM_ANCHORS_SDDM[@]}"
fi

cat >/etc/pam.d/omaface-lock <<EOF
#%PAM-1.0
# omaFace for the Omarchy lock screen (io.github.l0adn9.omaface). Managed by omaface.
auth       required                    $PAM_LINE
account    include                     system-local-login
EOF
chmod 0644 /etc/pam.d/omaface-lock

if [[ $LOGIN == yes ]]; then
  echo "  login screen"
  # The omaFace theme is Omarchy's with the mark added; doctor says when
  # Omarchy's has changed since.
  if [[ -f /usr/share/sddm/themes/omarchy/Main.qml ]]; then
    sha256sum /usr/share/sddm/themes/omarchy/Main.qml | cut -d' ' -f1 >/var/lib/omaface/sddm-upstream.sha256
  fi
  # Selected once a face exists (omaface enroll does it otherwise).
  owner=${SUDO_USER:-$(getent passwd "${PKEXEC_UID:-65534}" | cut -d: -f1)}
  if grep -qE '^login=on' "$CONF" && [[ -s /var/lib/omaface/${owner:-none}/faces.yml ]]; then
    write_login_conf /etc/sddm.conf.d/20-omaface.conf
  fi
fi

echo "  done"
