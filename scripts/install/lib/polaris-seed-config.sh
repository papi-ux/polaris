# Shared by Nix activation and the non-NixOS launcher (Bash).
# Seed from a file argument, or stdin, without changing any existing path.
polaris_seed_config() (
  local config_path="$1" config_tmp
  if [ -e "$config_path" ] || [ -L "$config_path" ]; then
    return 0
  fi
  # Keep even option-like relative paths safe for mktemp and POSIX link, whose
  # BSD implementation accepts no options (including --).
  case "$config_path" in
    /*) ;;
    *) config_path="./$config_path" ;;
  esac
  umask 077
  config_tmp="$(mktemp "${config_path}.tmp.XXXXXX")" || return
  trap 'rm -f -- "$config_tmp"' EXIT
  trap 'exit 1' HUP INT TERM
  if [ "$#" -ge 2 ]; then
    cat -- "$2" >"$config_tmp" || return
  else
    cat >"$config_tmp" || return
  fi
  # Unlike ordinary ln, link takes an exact destination and never descends into
  # a racing directory or symlink to one. The hard link publishes the complete
  # 0600 inode atomically, and cannot replace a concurrent creator.
  if link "$config_tmp" "$config_path" 2>/dev/null; then
    :
  elif [ -e "$config_path" ] || [ -L "$config_path" ]; then
    :
  else
    echo "polaris: cannot publish config at $config_path" >&2
    return 1
  fi
  rm -f -- "$config_tmp" || return
  trap - EXIT
)
