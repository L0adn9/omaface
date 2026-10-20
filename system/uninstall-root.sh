#!/bin/bash
# Root half of `omaface uninstall` for the git install: everything teardown
# does, then every file install-root.sh put in /usr/local and /etc.
#   uninstall [--keep-data]   keep enrolled faces and /etc/omaface/config

set -euo pipefail
(( EUID == 0 )) || { echo "run through: omaface uninstall" >&2; exit 1; }
LIB=/usr/local/lib/omaface

"$LIB/teardown" "$@"

echo "Removing files"
rm -f /etc/systemd/system/omaface.socket /etc/systemd/system/omaface@.service /etc/tmpfiles.d/omaface.conf
systemctl daemon-reload
rm -f /usr/share/polkit-1/actions/local.omaface.policy /etc/pacman.d/hooks/omaface.hook /usr/local/bin/omaface \
  /usr/local/share/applications/omaface.desktop /usr/local/share/icons/hicolor/scalable/apps/omaface.svg
rm -rf /usr/local/share/omaface /usr/local/src/omaface /usr/share/sddm/themes/omaface
# Last, since this script lives here.
rm -rf "$LIB"
echo "omaFace removed. opencv stays installed (marked explicit); remove it with pacman if you want."
