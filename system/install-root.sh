#!/bin/bash
# Root half of the git installer (install.sh), through sudo or pkexec:
#   install-root.sh <repo> <build-dir> <models-dir>
# Puts the files in /usr/local; `omaface setup` then wires them in. The
# package installs the same files in /usr through the same install-files.sh.

set -euo pipefail

REPO=$1
BUILD=$2
MODELS=$3

PREFIX=/usr/local
LIB=$PREFIX/lib/omaface
SRC=$PREFIX/src/omaface

(( EUID == 0 )) || { echo "install-root.sh must run as root" >&2; exit 1; }

if [[ -f /usr/lib/omaface/omaface-engine ]]; then
  echo "the omaface package is installed; remove it before installing from git" >&2
  exit 1
fi

# An earlier install under the old name (faceid): its own uninstaller takes
# its system parts out (PAM lines, service, theme), keeping enrolled faces and
# settings, which then move to the new paths.
if [[ -x /usr/local/lib/faceid/uninstall ]]; then
  echo "  replacing the earlier install (faceid); faces and settings are kept"
  /usr/local/lib/faceid/uninstall --keep-data >/dev/null
  if [[ -d /var/lib/faceid && ! -e /var/lib/omaface ]]; then mv /var/lib/faceid /var/lib/omaface; fi
  if [[ -d /etc/faceid && ! -e /etc/omaface ]]; then mv /etc/faceid /etc/omaface; fi
fi

bash "$REPO/system/install-files.sh" "$REPO" "$BUILD" "$MODELS" "" "$PREFIX" /etc/systemd/system /etc/tmpfiles.d
install -m0755 "$REPO/system/uninstall-root.sh" "$LIB/uninstall"
# From before setup kept this hash in /var/lib/omaface.
rm -f "$PREFIX/share/omaface/sddm-upstream.sha256"

# A package would be rebuilt by its manager when OpenCV changes; a git
# install rebuilds itself from these sources through a pacman hook.
echo "  OpenCV rebuild hook"
rm -rf "$SRC/engine"
install -d -m0755 "$SRC/engine" /etc/pacman.d/hooks
install -m0644 "$REPO/engine/omaface-engine.cpp" "$REPO/engine/omaface-pam.c" "$REPO/engine/Makefile" \
  "$REPO"/engine/*.h "$SRC/engine/"
sed "s|@PREFIX@|$PREFIX|g" "$REPO/system/rebuild" >"$LIB/rebuild"
chmod 0755 "$LIB/rebuild"
sed "s|@PREFIX@|$PREFIX|g" "$REPO/system/omaface.hook" >/etc/pacman.d/hooks/omaface.hook
chmod 0644 /etc/pacman.d/hooks/omaface.hook

# opencv only arrived as a dependency of frei0r-plugins; an orphan cleanup
# would take it, and the omaFace engine with it.
if ! pacman -Qi opencv | grep -q 'Install Reason *: Explicitly'; then
  echo "  marking opencv as explicitly installed (keeps it through orphan cleanups)"
  pacman -D --asexplicit opencv >/dev/null
fi
echo "  done"
