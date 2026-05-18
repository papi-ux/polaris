#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/dev-clean.sh <status|prune|git-prune|nuke-local-builds> [--apply]

Commands:
  status             Report workspace, Git, and generated artifact state.
  prune              Remove allowlisted generated build/test artifacts.
  git-prune          Run safe Git fetch/worktree/object maintenance.
  nuke-local-builds  Run prune and also remove node_modules/.

Destructive commands are dry-run by default. Pass --apply to remove files or run Git maintenance.
USAGE
}

die() {
  echo "$*" >&2
  usage >&2
  exit 2
}

command="${1:-}"
apply=0

if [[ -z "$command" || "$command" == "-h" || "$command" == "--help" ]]; then
  usage
  exit 0
fi
shift || true

for arg in "$@"; do
  case "$arg" in
    --apply)
      apply=1
      ;;
    *)
      die "Unknown option: $arg"
      ;;
  esac
done

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "dev-clean must run inside a Git repository." >&2
  exit 2
}
cd "$repo_root"

print_section() {
  printf '\n== %s ==\n' "$1"
}

print_size() {
  local path="$1"
  if ! du -sh "$path" 2>/dev/null; then
    printf 'unavailable\t%s\n' "$path"
  fi
}

path_exists() {
  [[ -e "$1" || -L "$1" ]]
}

display_path() {
  local path="$1"
  if [[ -d "$path" && ! -L "$path" ]]; then
    printf '%s/\n' "$path"
  else
    printf '%s\n' "$path"
  fi
}

safe_remove() {
  local path="$1"
  local label
  case "$path" in
    ""|.|/|/*|../*|*/../*|.git|.git/*)
      echo "Refusing unsafe cleanup path: $path" >&2
      exit 2
      ;;
  esac

  if path_exists "$path"; then
    label="$(display_path "$path")"
    if [[ "$apply" -eq 1 ]]; then
      rm -rf -- "$path"
      printf 'Removed %s\n' "$label"
    else
      printf 'Would remove %s\n' "$label"
    fi
  fi
}

cleanup_candidates() {
  local path
  for path in \
    build \
    arch-pkgbuild \
    test-results \
    playwright-report \
    compile_commands.json
  do
    path_exists "$path" && printf '%s\n' "$path"
  done

  shopt -s nullglob
  for path in build-* cmake-*; do
    [[ "$path" == "build-deps" ]] && continue
    path_exists "$path" && printf '%s\n' "$path"
  done
  shopt -u nullglob
}

print_candidates() {
  local candidates
  candidates="$(cleanup_candidates || true)"
  if [[ -z "$candidates" ]]; then
    echo "No generated cleanup candidates found."
  else
    while IFS= read -r path; do
      [[ -z "$path" ]] && continue
      display_path "$path"
    done <<< "$candidates"
  fi
}

run_prune() {
  local include_node_modules="${1:-0}"
  local candidates path

  if [[ "$apply" -eq 1 ]]; then
    echo "Applying generated artifact cleanup."
  else
    echo "Dry run. Pass --apply to remove generated artifacts."
  fi

  candidates="$(cleanup_candidates || true)"
  if [[ "$include_node_modules" == "1" && -d node_modules ]]; then
    candidates="${candidates}"$'\n'"node_modules"
  fi

  if [[ -z "${candidates//$'\n'/}" ]]; then
    echo "No generated cleanup candidates found."
    return 0
  fi

  while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    safe_remove "$path"
  done <<< "$candidates"
}

run_git_prune() {
  local remote

  if [[ "$apply" -eq 1 ]]; then
    echo "Applying Git maintenance."
  else
    echo "Dry run. Pass --apply to run Git maintenance."
  fi

  while IFS= read -r remote; do
    [[ -z "$remote" ]] && continue
    if [[ "$apply" -eq 1 ]]; then
      git fetch --prune "$remote"
    else
      printf 'Would run: git fetch --prune %s\n' "$remote"
    fi
  done < <(git remote)

  if [[ "$apply" -eq 1 ]]; then
    git worktree prune
    git gc --prune=now
  else
    echo "Would run: git worktree prune"
    echo "Would run: git gc --prune=now"
  fi
}

run_status() {
  print_section "Workspace"
  git status -sb

  print_section "Branches"
  git branch -vv || true

  print_section "Stashes"
  git stash list || true

  print_section "Worktrees"
  git worktree list || true

  print_section "Generated cleanup candidates"
  print_candidates

  print_section "Sizes"
  for path in . .git node_modules third-party; do
    if path_exists "$path"; then
      print_size "$path"
    fi
  done

  print_section "Git objects"
  git count-objects -vH
}

case "$command" in
  status)
    run_status
    ;;
  prune)
    run_prune 0
    ;;
  git-prune)
    run_git_prune
    ;;
  nuke-local-builds)
    run_prune 1
    ;;
  *)
    die "Unknown command: $command"
    ;;
esac
