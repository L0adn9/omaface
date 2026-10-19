#!/bin/bash
# Root half of `faceid uninstall`: takes every system file back out and puts
# PAM and the login screen back the way Omarchy had them.
#   uninstall [--keep-data]   keep enrolled faces and /etc/faceid/config

set -euo pipefail
(( EUID == 0 )) || { echo "run through: faceid uninstall" >&2; exit 1; }
keep=0
[[ ${1:-} == --keep-data ]] && keep=1

echo "Removing Face ID from PAM"
for f in sudo polkit-1 sddm; do
  [[ -f /etc/pam.d/$f ]] || continue
  if grep -qE 'faceid-pam|^# faceid:' "/etc/pam.d/$f"; then
    tmp=$(mktemp "/etc/pam.d/$f.faceid.XXXXXX")
    grep -vE 'faceid-pam|^# faceid:' "/etc/pam.d/$f" >"$tmp"
    chmod --reference="/etc/pam.d/$f" "$tmp"
    chown --reference="/etc/pam.d/$f" "$tmp"
    mv "$tmp" "/etc/pam.d/$f"
  fi
done
rm -f /etc/pam.d/omarchy-lock-face
# An /etc copy of polkit's stack that we made: drop it if nothing else
# (fingerprint setup since) has changed it, so polkit uses its own again.
if [[ -f /var/lib/faceid/pam-backup/polkit-1.created && -f /usr/lib/pam.d/polkit-1 ]] &&
  cmp -s /etc/pam.d/polkit-1 /usr/lib/pam.d/polkit-1; then
  rm -f /etc/pam.d/polkit-1
fi

echo "Restoring Omarchy's login screen"
rm -f /etc/sddm.conf.d/20-faceid.conf
rm -rf /usr/share/sddm/themes/omarchy-faceid

echo "Stopping the service"
systemctl disable --now faceid.socket >/dev/null 2>&1 || true
rm -f /etc/systemd/system/faceid.socket /etc/systemd/system/faceid@.service /etc/tmpfiles.d/faceid.conf
systemctl daemon-reload
rm -rf /run/faceid

echo "Removing files"
rm -f /usr/share/polkit-1/actions/local.faceid.policy /etc/pacman.d/hooks/faceid.hook /usr/local/bin/faceid
rm -rf /usr/local/share/faceid /usr/local/src/faceid
if (( keep )); then
  echo "Kept enrolled faces in /var/lib/faceid and /etc/faceid/config"
else
  rm -rf /var/lib/faceid /etc/faceid
  echo "Deleted enrolled faces and configuration"
fi
# Last, since this script lives here.
rm -rf /usr/local/lib/faceid
echo "Face ID removed. opencv stays installed (marked explicit); remove it with pacman if you want."
