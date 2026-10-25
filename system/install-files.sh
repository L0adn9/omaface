#!/bin/bash
# Puts every omaFace file in place for one prefix. Shared by the git installer
# (install-root.sh: prefix /usr/local, units in /etc) and the package
# (PKGBUILD: prefix /usr, units in /usr/lib), so both install the same set.
# Nothing here touches PAM, the login screen or a user's config: that is
# `omaface setup`.
#
#   install-files.sh <repo> <engine-build-dir> <models-dir> <destdir> <prefix> <unit-dir> <tmpfiles-dir>

set -euo pipefail

REPO=$1
BUILD=$2
MODELS=$3
DEST=$4
PREFIX=$5
UNITS=$6
TMPFILES=$7

LIB=$DEST$PREFIX/lib/omaface
SHARE=$DEST$PREFIX/share/omaface

# fill <mode> <source> <target>: copy with @PREFIX@ filled in.
fill() {
  install -Dm"$1" /dev/null "$3"
  sed "s|@PREFIX@|$PREFIX|g" "$2" >"$3"
}

echo "  binaries and command"
install -Dm0755 "$BUILD/omaface-engine" "$LIB/omaface-engine"
install -Dm0755 "$BUILD/omaface-pam" "$LIB/omaface-pam"
fill 0755 "$REPO/bin/omaface" "$DEST$PREFIX/bin/omaface"
fill 0755 "$REPO/system/setup-root.sh" "$LIB/setup-root"
fill 0755 "$REPO/system/teardown-root.sh" "$LIB/teardown"
install -Dm0644 "$REPO/system/pam.sh" "$LIB/pam.sh"

echo "  models"
install -d -m0755 "$SHARE/models"
# Every model the checksum list names, checked again where it lands.
while read -r _ name; do install -m0644 "$MODELS/$name" "$SHARE/models/"; done <"$REPO/models/SHA256SUMS"
install -m0644 "$REPO/models/SHA256SUMS" "$SHARE/models/"
(cd "$SHARE/models" && sha256sum -c --quiet SHA256SUMS)

echo "  plugin sources, default settings, login theme"
# The shell plugin is built per user from these by `omaface setup` and
# `omaface sync`, against whatever lock screen Omarchy has at the time.
rm -rf "$SHARE/plugin" "$SHARE/tools"
install -d -m0755 "$SHARE/plugin" "$SHARE/tools" "$SHARE/hooks"
install -m0644 "$REPO"/plugin/*.qml "$REPO/plugin/manifest.json" "$SHARE/plugin/"
install -m0644 "$REPO/tools/patches.py" "$SHARE/tools/"
install -m0755 "$REPO/tools/build-plugin.py" "$SHARE/tools/"
install -m0755 "$REPO/tools/test-client.py" "$SHARE/tools/"
install -m0755 "$REPO/hooks/post-update" "$SHARE/hooks/post-update"
install -m0644 "$REPO/system/config" "$SHARE/config"
# SDDM only looks in /usr/share. Selecting the theme is up to `omaface setup`.
rm -rf "$DEST/usr/share/sddm/themes/omaface"
install -d -m0755 "$DEST/usr/share/sddm/themes/omaface"
install -m0644 "$REPO"/sddm/* "$DEST/usr/share/sddm/themes/omaface/"

echo "  service, polkit action, launcher entry"
fill 0644 "$REPO/system/omaface.socket" "$DEST$UNITS/omaface.socket"
fill 0644 "$REPO/system/omaface@.service" "$DEST$UNITS/omaface@.service"
install -Dm0644 "$REPO/system/omaface.tmpfiles" "$DEST$TMPFILES/omaface.conf"
fill 0644 "$REPO/system/omaface.policy" "$DEST/usr/share/polkit-1/actions/local.omaface.policy"
install -Dm0644 "$REPO/desktop/omaface.desktop" "$DEST$PREFIX/share/applications/omaface.desktop"
install -Dm0644 "$REPO/desktop/omaface.svg" "$DEST$PREFIX/share/icons/hicolor/scalable/apps/omaface.svg"
