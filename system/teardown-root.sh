#!/bin/bash
# Root half of `omaface uninstall`: takes omaFace out of PAM and the login
# screen and stops the service, the way Omarchy had them. The files stay
# (the git install's uninstall removes them next; the package before removal
# runs this with --keep-data).
#   teardown [--keep-data]   keep enrolled faces and /etc/omaface/config

set -euo pipefail
(( EUID == 0 )) || { echo "run through: omaface uninstall" >&2; exit 1; }
keep=0
[[ ${1:-} == --keep-data ]] && keep=1

echo "Removing omaFace from PAM"
for f in sudo polkit-1 sddm; do
  [[ -f /etc/pam.d/$f ]] || continue
  if grep -qE 'omaface-pam|^# omaface:' "/etc/pam.d/$f"; then
    tmp=$(mktemp "/etc/pam.d/$f.omaface.XXXXXX")
    grep -vE 'omaface-pam|^# omaface:' "/etc/pam.d/$f" >"$tmp"
    chmod --reference="/etc/pam.d/$f" "$tmp"
    chown --reference="/etc/pam.d/$f" "$tmp"
    mv "$tmp" "/etc/pam.d/$f"
  fi
done
rm -f /etc/pam.d/omaface-lock
# An /etc copy of polkit's stack that we made: drop it if nothing else
# (fingerprint setup since) has changed it, so polkit uses its own again.
if [[ -f /var/lib/omaface/pam-backup/polkit-1.created && -f /usr/lib/pam.d/polkit-1 ]] &&
  cmp -s /etc/pam.d/polkit-1 /usr/lib/pam.d/polkit-1; then
  rm -f /etc/pam.d/polkit-1
fi
rm -f /var/lib/omaface/pam-backup/polkit-1.created

echo "Restoring Omarchy's login screen"
rm -f /etc/sddm.conf.d/20-omaface.conf

echo "Stopping the service"
systemctl disable --now omaface.socket >/dev/null 2>&1 || true
rm -rf /run/omaface

if (( keep )); then
  echo "Kept enrolled faces in /var/lib/omaface and /etc/omaface/config"
else
  rm -rf /var/lib/omaface /etc/omaface
  echo "Deleted enrolled faces and configuration"
fi
