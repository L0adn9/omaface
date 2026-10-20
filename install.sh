#!/bin/bash
# omaFace, face unlock for Omarchy: installer from git. Run it as your user
# from this folder:
#   ./install.sh
# It builds everything first and only then asks for root, to put the files
# in /usr/local; `omaface setup` then wires them in. Re-running it upgrades
# in place. `omaface uninstall` reverses all of it. (The AUR package installs
# the same files in /usr and leaves `omaface setup` to you.)

set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
REPO=$PWD
MODEL_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/omaface/models"

step() { printf '\n\e[1m==> %s\e[0m\n' "$*"; }
fail() { printf '\e[31minstall: %s\e[0m\n' "$*" >&2; exit 1; }

(( EUID != 0 )) || fail "run as your user; it asks for root itself"

step "Checking this machine"
source system/pam.sh
# The same checks `omaface setup` makes, here before anything is built.
omaface_preflight || exit 1

# Build tools and OpenCV. pacman lists everything it would add and asks first.
missing=()
for c in g++ make pkg-config; do command -v "$c" >/dev/null || { missing+=(base-devel); break; }; done
pkg-config --exists opencv5 || pkg-config --exists opencv4 || missing+=(opencv)
if (( ${#missing[@]} )); then
  echo "  omaFace is built here and needs: ${missing[*]}"
  [[ " ${missing[*]} " == *" opencv "* ]] && echo "  (opencv is about 113 MB; most of what it uses is already on Omarchy)"
  [[ -t 0 ]] || fail "install them first: sudo pacman -S --needed ${missing[*]}"
  sudo pacman -S --needed "${missing[@]}" || fail "packages not installed; nothing was changed"
fi
for c in jq curl python3 sha256sum; do command -v "$c" >/dev/null || fail "missing command: $c"; done
echo "  ok"

build=$(mktemp -d "${TMPDIR:-/tmp}/omaface-build.XXXXXX")
chmod 0755 "$build"
trap 'rm -rf "$build"' EXIT

step "Building the engine"
cp -a engine "$build/engine"
make -C "$build/engine" -s clean all PREFIX=/usr/local
# Some camera, IR or not, before anything is installed. Which one and what
# that means is for setup to say.
if ! "$build/engine/omaface-engine" probe >/dev/null 2>&1 && ! "$build/engine/omaface-engine" probe webcam >/dev/null 2>&1; then
  echo "  Cameras on this machine:" >&2
  "$build/engine/omaface-engine" cameras 2>/dev/null | sed 's/^/    /' >&2 || true
  fail "no camera found; nothing was changed"
fi
echo "  ok"

step "Fetching face models (checksum-verified)"
mkdir -p "$MODEL_CACHE"
cp models/SHA256SUMS "$MODEL_CACHE/"
# Each model from where models/URLS says, kept only if its hash matches.
while read -r sum name; do
  [[ -f $MODEL_CACHE/$name ]] && [[ $(sha256sum "$MODEL_CACHE/$name" | cut -d' ' -f1) == "$sum" ]] && continue
  url=$(awk -v n="$name" '$1 == n { print $2 }' models/URLS)
  [[ -n $url ]] || fail "no download address for $name in models/URLS"
  echo "  downloading $name"
  curl -fsSL "$url" -o "$MODEL_CACHE/$name.part"
  mv "$MODEL_CACHE/$name.part" "$MODEL_CACHE/$name"
done <models/SHA256SUMS
(cd "$MODEL_CACHE" && sha256sum -c --quiet SHA256SUMS) || fail "model checksum mismatch; delete $MODEL_CACHE and retry"
echo "  ok"

step "Installing files (root)"
if [[ -t 0 ]]; then
  sudo bash "$REPO/system/install-root.sh" "$REPO" "$build/engine" "$MODEL_CACHE"
else
  pkexec /usr/bin/bash "$REPO/system/install-root.sh" "$REPO" "$build/engine" "$MODEL_CACHE"
fi

exec /usr/local/bin/omaface setup
