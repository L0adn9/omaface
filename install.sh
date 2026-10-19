#!/bin/bash
# omaFace, face unlock for Omarchy: installer. Run it as your user from this folder:
#   ./install.sh
# It builds everything first and only then asks for root once. Re-running it
# upgrades in place. `omaface uninstall` reverses all of it.

set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
REPO=$PWD
PLUGIN_ID=io.github.l0adn9.omaface
PLUGINS_DIR="$HOME/.config/omarchy/plugins"
MODEL_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/omaface/models"
ZOO=https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models

step() { printf '\n\e[1m==> %s\e[0m\n' "$*"; }
fail() { printf '\e[31minstall: %s\e[0m\n' "$*" >&2; exit 1; }

(( EUID != 0 )) || fail "run as your user; it asks for root itself"

step "Checking this machine"
source system/pam.sh

command -v omarchy >/dev/null && command -v omarchy-shell >/dev/null ||
  fail "this is for Omarchy 4 (omarchy and omarchy-shell not found)"
[[ -f /usr/share/omarchy/shell/plugins/lock/Service.qml ]] ||
  fail "Omarchy's shell lock screen was not found; omaFace needs Omarchy 4 or newer (omarchy update)"
[[ -f /usr/lib/security/pam_exec.so ]] || fail "pam_exec.so not found (pam package)"

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

# Every PAM stack gets one line above its password line; check they all have
# one before anything is touched.
pam_anchor /etc/pam.d/sudo "${PAM_ANCHORS_SUDO[@]}" >/dev/null ||
  fail "/etc/pam.d/sudo has a layout omaFace does not recognize; nothing was changed"
polkit_file=$(polkit_pam_file) || fail "polkit has no PAM configuration"
pam_anchor "$polkit_file" "${PAM_ANCHORS_POLKIT[@]}" >/dev/null ||
  fail "$polkit_file has a layout omaFace does not recognize; nothing was changed"

# The login screen needs SDDM as the display manager. Without it the other
# three places still work and login stays off.
login=no
if sddm_in_use && pam_anchor /etc/pam.d/sddm "${PAM_ANCHORS_SDDM[@]}" >/dev/null; then
  login=yes
  if [[ -n $(sddm_autologin_user) ]]; then
    echo "  note: SDDM signs you in automatically, so the login screen never shows."
    echo "        omaFace is set up for it anyway and works if you turn autologin off."
  fi
else
  echo "  note: SDDM is not your login manager; omaFace will not be offered at login"
fi
echo "  ok"

build=$(mktemp -d "${TMPDIR:-/tmp}/omaface-build.XXXXXX")
chmod 0755 "$build"
trap 'rm -rf "$build"' EXIT

step "Building the engine"
cp -a engine "$build/engine"
make -C "$build/engine" -s clean all
webcam=no
if ! cam=$("$build/engine/omaface-engine" probe); then
  if ! cam=$("$build/engine/omaface-engine" probe webcam); then
    echo "  Cameras on this machine:" >&2
    "$build/engine/omaface-engine" cameras 2>/dev/null | sed 's/^/    /' >&2 || true
    fail "no camera found. If yours is listed above but not picked, set device=/dev/videoN in /etc/omaface/config after installing."
  fi
  # Found automatically: no IR camera, so the regular webcam, with the limits
  # that keep it safe. Nothing to decide here; Settings shows it too.
  cat <<'EOF'
  This laptop has no IR (Windows Hello) camera, so omaFace uses the regular
  webcam, with limits that keep it safe:
    - lock screen only: sudo, system prompts and the login screen keep
      using your password
    - every unlock asks for a few random head turns, so a photo or a video
      of you is not enough
    - three failed tries and it waits for your password
EOF
  webcam=yes
  login=no
fi
if [[ $webcam == yes ]]; then
  echo "  Webcam: $(cut -f2 <<<"$cam") ($(cut -f1 <<<"$cam"), $(cut -f4 <<<"$cam") $(cut -f3 <<<"$cam")), lock screen only"
else
  echo "  IR camera: $(cut -f2 <<<"$cam") ($(cut -f1 <<<"$cam"), $(cut -f4 <<<"$cam") $(cut -f3 <<<"$cam"))"
fi

# Many laptops keep the IR light off until it is switched on once. omaFace
# installs either way; this only says what to do about it.
emitter=$([[ $webcam == yes ]] && echo webcam || "$build/engine/omaface-engine" emitter 2>/dev/null || true)
case $emitter in
  webcam) ;;
  *'"pulsing"'* | *'"steady"'*) echo "  IR light: on" ;;
  *'"dark"'*)
    cat <<'EOF'
  IR light: did not come on. Many Lenovo, Dell and HP laptops need it switched
  on once. After installing, run:
      omarchy pkg aur add linux-enable-ir-emitter
      sudo linux-enable-ir-emitter configure
  then check with: omaface emitter
EOF
    ;;
  *) echo "  IR light: could not check (camera busy?); check later with: omaface emitter" ;;
esac

step "Fetching face models (OpenCV Zoo, checksum-verified)"
mkdir -p "$MODEL_CACHE"
cp models/SHA256SUMS "$MODEL_CACHE/"
while read -r sum name; do
  [[ -f $MODEL_CACHE/$name ]] && [[ $(sha256sum "$MODEL_CACHE/$name" | cut -d' ' -f1) == "$sum" ]] && continue
  case "$name" in
    face_detection_*) dir=face_detection_yunet ;;
    face_recognition_*) dir=face_recognition_sface ;;
  esac
  echo "  downloading $name"
  curl -fsSL "$ZOO/$dir/$name" -o "$MODEL_CACHE/$name.part"
  mv "$MODEL_CACHE/$name.part" "$MODEL_CACHE/$name"
done <models/SHA256SUMS
(cd "$MODEL_CACHE" && sha256sum -c --quiet SHA256SUMS) || fail "model checksum mismatch; delete $MODEL_CACHE and retry"
echo "  ok"

step "Building the shell plugin from the current Omarchy lock screen"
python3 -I tools/build-plugin.py plugin "$build/plugin"
omarchy plugin validate "$build/plugin" >/dev/null || fail "plugin failed omarchy's manifest validation"

step "Installing system parts (root)"
echo "  PAM: sudo, polkit$([[ $login == yes ]] && echo " and login") get one omaFace line above the password;"
echo "       originals are backed up to /var/lib/omaface/pam-backup"
if [[ -t 0 ]]; then
  sudo bash "$REPO/system/install-root.sh" "$REPO" "$build" "$MODEL_CACHE" "$login" "$webcam"
else
  pkexec /usr/bin/bash "$REPO/system/install-root.sh" "$REPO" "$build" "$MODEL_CACHE" "$login" "$webcam"
fi

step "Installing the shell plugin"
mkdir -p "$PLUGINS_DIR"

# An earlier omaFace build under another plugin id (a lock screen clone that
# ships FaceGlyph.qml): switch it off, which hands the lock back to Omarchy for
# a moment, and keep a copy, so two lock screens never compete.
for manifest in "$PLUGINS_DIR"/*/manifest.json; do
  [[ -f $manifest ]] || continue
  dir=$(dirname "$manifest")
  old=$(jq -r '.id // ""' "$manifest" 2>/dev/null)
  [[ -n $old && $old != "$PLUGIN_ID" && -f $dir/FaceGlyph.qml ]] || continue
  jq -e '.omarchy.clonedFrom == "omarchy.lock"' "$manifest" >/dev/null 2>&1 || continue
  echo "  replacing the earlier omaFace plugin $old"
  omarchy plugin disable omarchy.polkit >/dev/null 2>&1 || true
  omarchy plugin disable "$old" >/dev/null 2>&1 || true
  backup="$HOME/.local/share/omarchy-plugin-backups/$old-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$(dirname "$backup")"
  mv "$dir" "$backup"
  echo "  kept in $backup"
done
if [[ -d $PLUGINS_DIR/$PLUGIN_ID ]]; then
  backup="$HOME/.local/share/omarchy-plugin-backups/$PLUGIN_ID-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$(dirname "$backup")"
  cp -a "$PLUGINS_DIR/$PLUGIN_ID" "$backup"
  rm -rf "$PLUGINS_DIR/$PLUGIN_ID"
  echo "  previous copy kept in $backup"
  # Keep the three newest backups.
  ls -1d "$(dirname "$backup")/$PLUGIN_ID"-* 2>/dev/null | sort | head -n -3 | xargs -r rm -rf
fi
cp -a "$build/plugin" "$PLUGINS_DIR/$PLUGIN_ID"
omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
for _ in $(seq 1 40); do
  omarchy plugin list --json 2>/dev/null | jq -e --arg id "$PLUGIN_ID" 'any(.[]; .id == $id)' >/dev/null && break
  sleep 0.1
done

step "Handing the lock screen and polkit dialog to omaFace"
cp -a "$HOME/.config/omarchy/shell.json" "$HOME/.config/omarchy/shell.json.bak.omaface.$(date +%s)"
# Polkit first: two agents cannot be registered at once.
omarchy plugin disable omarchy.polkit >/dev/null
omarchy plugin enable "$PLUGIN_ID" >/dev/null
omarchy restart shell >/dev/null 2>&1 || true

live=0
for _ in $(seq 1 40); do
  out=$(timeout 3 omarchy-shell omaface status 2>/dev/null || true)
  if [[ $out == *'"polkitRegistered":true'* ]]; then live=1; break; fi
  sleep 0.5
done
if (( ! live )); then
  echo "  the plugin did not come up; restoring Omarchy's own lock screen and polkit dialog" >&2
  omarchy plugin disable "$PLUGIN_ID" >/dev/null 2>&1 || true
  omarchy plugin enable omarchy.lock >/dev/null 2>&1 || true
  omarchy plugin enable omarchy.polkit >/dev/null 2>&1 || true
  omarchy restart shell >/dev/null 2>&1 || true
  fail "plugin failed to load; see: journalctl --user -b | grep -i omaface"
fi
timeout 3 omarchy-shell lock status >/dev/null || fail "lock screen IPC not answering"
echo "  lock screen and polkit dialog now run from $PLUGIN_ID"

step "Launcher entry, update hook, health check"
# Leftovers from an install under the old name.
rm -f "$HOME/.local/share/applications/faceid.desktop" \
  "$HOME/.local/share/icons/hicolor/scalable/apps/faceid.svg" \
  "$HOME/.config/omarchy/hooks/post-update.d/faceid"
install -Dm0644 desktop/omaface.desktop "$HOME/.local/share/applications/omaface.desktop"
install -Dm0644 desktop/omaface.svg "$HOME/.local/share/icons/hicolor/scalable/apps/omaface.svg"
install -Dm0755 hooks/post-update "$HOME/.config/omarchy/hooks/post-update.d/omaface"
hc="$HOME/.local/bin/omarchy-health-check"
if [[ -f $hc ]]; then
  python3 -I - "$hc" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path).read()
# A section from an earlier install (also under the old name "Face ID") goes
# first, so the check never calls a command that no longer exists.
text = re.sub(r'head_ "(?:Face ID|omaFace)[^"]*"\n.*?\nfi\n\n', '', text, flags=re.S)
section = '''head_ "omaFace"
# IR face unlock: engine, PAM lines, plugin live in the shell, upstream drift.
if command -v omaface >/dev/null 2>&1; then
  issues=$(omaface doctor --quiet 2>&1 | sed 's/\\x1b\\[[0-9;]*m//g; s/^ *//')
  if [ -z "$issues" ]; then ok "omaface doctor: all checks pass"
  else while IFS= read -r l; do [ -n "$l" ] && bad "${l#* }" "omaface doctor"; done <<<"$issues"; fi
else
  bad "omaface command missing" "re-run install.sh from the omaFace folder"
fi

'''
anchor = "printf '\\n\\033[1m%d passed, %d failed\\033[0m\\n'"
if anchor in text:
    text = text.replace(anchor, section + anchor, 1)
    open(path, "w").write(text)
    print("  omaFace section in omarchy-health-check")
PY
fi

step "Done"
omaface doctor --quiet || true
if [[ $(omaface status --json 2>/dev/null) != *'"name"'* ]]; then
  cat <<'EOF'

  Next: enroll your face (sit in front of the laptop, lid open):
      omaface enroll              in a terminal
      omaface settings            or the omaFace window (also in the app launcher)
EOF
fi
cat <<'EOF'

  Your password keeps working everywhere. Undo it all with: omaface uninstall
EOF
