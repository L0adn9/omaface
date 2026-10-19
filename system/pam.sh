# Where omaFace goes in each PAM stack, shared by install.sh (checks before
# anything changes), install-root.sh (the edit) and omaface doctor.
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
