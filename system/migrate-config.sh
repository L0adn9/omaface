#!/bin/bash
# Moves an existing /etc/omaface/config past defaults that changed, as root,
# on every upgrade (install-root.sh, the package's post_upgrade) and setup.
# Only a value still at an old default moves; one the user picked is kept.
#
# 1.2.2: matching became stricter (a second face in view stops it, and a face
# is compared with its five closest samples, not the single closest), and
# the thresholds rose with it: IR 0.50 -> 0.55, webcam 0.55 -> 0.60.

set -euo pipefail

CONF=/etc/omaface/config
[[ -f $CONF ]] || exit 0

# bump <key> <old default> <new default>
bump() {
  local old=${2//./\\.}
  if grep -qE "^[[:space:]]*$1[[:space:]]*=[[:space:]]*$old[[:space:]]*$" "$CONF"; then
    sed -i -E "s/^[[:space:]]*$1[[:space:]]*=.*/$1=$3/" "$CONF"
    echo "  $1: $2 -> $3 (the new default; omaface set $1 $2 goes back)"
  fi
}
bump threshold 0.50 0.55
bump threshold 0.5 0.55
bump threshold_rgb 0.55 0.60
