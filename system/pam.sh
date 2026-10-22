# Where omaFace goes in each PAM stack, shared by install.sh and omaface setup
# (checks before anything changes), setup-root (the edit) and omaface doctor.
#
# The omaFace line sits directly above the line that asks for the password,
# so whatever comes before it (Omarchy's fingerprint reader and its lid gate)
# keeps its place, and the password stays the fallback underneath.
# Stock Arch, Omarchy, and Omarchy with fingerprint set up all match one of
# these; anything else is refused rather than guessed at.

PAM_ANCHORS_SUDO=(
  '^auth[[:space:]]+(include|substack)[[:space:]]+system-auth'
  '^auth[[:space:]]+required[[:space:]]+pam_unix[.]so'
)
PAM_ANCHORS_POLKIT=(
  '^auth[[:space:]]+required[[:space:]]+pam_unix[.]so'
  '^auth[[:space:]]+(include|substack)[[:space:]]+system-auth'
)
PAM_ANCHORS_SDDM=(
  '^auth[[:space:]]+(include|substack)[[:space:]]+system-login'
)

# pam_anchor <file> <anchor...>: the first anchor that occurs exactly once.
pam_anchor() {
  local file=$1 anchor
  shift
  [[ -r $file ]] || return 1
  for anchor in "$@"; do
    if [[ $(grep -cE "$anchor" "$file") == 1 ]]; then
      printf '%s\n' "$anchor"
      return 0
    fi
  done
  return 1
}

# polkit ships its stack in /usr/lib/pam.d; a copy in /etc/pam.d (made by
# Omarchy's fingerprint setup, or by us) takes precedence.
polkit_pam_file() {
  if [[ -f /etc/pam.d/polkit-1 ]]; then
    echo /etc/pam.d/polkit-1
  elif [[ -f /usr/lib/pam.d/polkit-1 ]]; then
    echo /usr/lib/pam.d/polkit-1
  else
    return 1
  fi
}

# The login screen only applies when SDDM is the display manager.
sddm_in_use() {
  [[ $(readlink -f /etc/systemd/system/display-manager.service 2>/dev/null) == */sddm.service ]]
}

# The login screen's drop-in: the omaFace theme, and permission for it to read
# the scanner's live status (the webcam's "turn your head" prompts) through
# QML's file access, which Qt 6 only allows with this variable. Any greeter
# environment set elsewhere is kept, since a later file replaces the key.
write_login_conf() {
  local env="QML_XHR_ALLOW_FILE_READ=1" other="" f
  local files=()
  for f in /etc/sddm.conf.d/*.conf /etc/sddm.conf; do
    [[ -f $f && $f != */20-omaface.conf ]] && files+=("$f")
  done
  if (( ${#files[@]} )); then
    other=$(awk '/^[[:space:]]*GreeterEnvironment[[:space:]]*=/ { sub(/^[^=]*=[[:space:]]*/, ""); v = $0 } END { print v }' "${files[@]}")
  fi
  if [[ -n $other && $other != *QML_XHR_ALLOW_FILE_READ* ]]; then env="$other,$env"; fi
  printf '# omaFace login theme (omaface disable login reverts to Omarchy'"'"'s theme)\n[General]\nGreeterEnvironment=%s\n\n[Theme]\nCurrent=omaface\n' "$env" >"$1"
  chmod 0644 "$1"
}

# SDDM autologin (the Omarchy ISO turns it on for encrypted disks) means the
# greeter never shows, so there is no login screen to scan at. SDDM reads
# sddm.conf.d in order and sddm.conf last; a later empty User= turns it off.
sddm_autologin_user() {
  awk '
    /^[[:space:]]*\[/ { section = $0; gsub(/[[:space:]\[\]]/, "", section); next }
    section == "Autologin" && /^[[:space:]]*User[[:space:]]*=/ {
      sub(/^[^=]*=[[:space:]]*/, ""); sub(/[[:space:]]+$/, ""); user = $0
    }
    END { if (user != "") print user }
  ' /etc/sddm.conf.d/*.conf /etc/sddm.conf 2>/dev/null
}

# omaface_preflight: can omaFace be set up here? Says why not on stderr and
# fails, before anything is changed. Sets OMAFACE_LOGIN to yes when the login
# screen can have it too (SDDM is the display manager), else no.
omaface_preflight() {
  if ! command -v omarchy >/dev/null || ! command -v omarchy-shell >/dev/null; then
    echo "this is for Omarchy 4.5 (omarchy and omarchy-shell not found)" >&2
    return 1
  fi
  if [[ ! -f /usr/share/omarchy/shell/plugins/lock/Service.qml ]]; then
    echo "Omarchy's shell lock screen was not found; omaFace needs Omarchy 4.5 or newer (omarchy update)" >&2
    return 1
  fi
  # 4.5 replaced IpcHandler with ShellIpc for shell targets; the plugin's
  # build patches the new files and cannot be used with the old ones.
  if [[ ! -f /usr/share/omarchy/shell/Commons/ShellIpc.qml ]]; then
    echo "omaFace needs Omarchy 4.5 or newer for its shell IPC (omarchy update)" >&2
    return 1
  fi
  [[ -f /usr/lib/security/pam_exec.so ]] || { echo "pam_exec.so not found (pam package)" >&2; return 1; }
  # Every PAM stack gets one line above its password line; check they all
  # have one before anything is touched.
  if ! pam_anchor /etc/pam.d/sudo "${PAM_ANCHORS_SUDO[@]}" >/dev/null; then
    echo "/etc/pam.d/sudo has a layout omaFace does not recognize; nothing was changed" >&2
    return 1
  fi
  local polkit_file
  polkit_file=$(polkit_pam_file) || { echo "polkit has no PAM configuration" >&2; return 1; }
  if ! pam_anchor "$polkit_file" "${PAM_ANCHORS_POLKIT[@]}" >/dev/null; then
    echo "$polkit_file has a layout omaFace does not recognize; nothing was changed" >&2
    return 1
  fi
  # The login screen needs SDDM as the display manager. Without it the other
  # three places still work and login stays off.
  OMAFACE_LOGIN=no
  if sddm_in_use && pam_anchor /etc/pam.d/sddm "${PAM_ANCHORS_SDDM[@]}" >/dev/null; then
    OMAFACE_LOGIN=yes
    if [[ -n $(sddm_autologin_user) ]]; then
      echo "  note: SDDM signs you in automatically, so the login screen never shows."
      echo "        omaFace is set up for it anyway and works if you turn autologin off."
    fi
  else
    echo "  note: SDDM is not your login manager; omaFace will not be offered at login"
  fi
}
