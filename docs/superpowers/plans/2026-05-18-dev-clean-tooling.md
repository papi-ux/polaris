# Dev Cleanup Tooling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a safe committed cleanup helper that reports workspace hygiene and removes only allowlisted generated build artifacts when explicitly applied.

**Architecture:** Implement one Bash CLI in `scripts/dev-clean.sh` with small functions for repo detection, reporting, candidate discovery, deletion, and Git maintenance. Cover behavior with one Bash integration test that copies the script into a temporary Git repo and exercises dry-run, apply, protected ignored files, stash/branch reporting, and usage errors.

**Tech Stack:** Bash, Git CLI, standard POSIX utilities available in the existing Linux build environment.

---

## File Structure

- Create `scripts/dev-clean.sh`: safe cleanup/status command with `status`, `prune`, `git-prune`, and `nuke-local-builds` subcommands.
- Create `tests/scripts/test_dev_clean.sh`: Bash integration tests using a temporary Git repo.
- Modify `docs/building.md`: add the development hygiene workflow under Development Notes.

---

### Task 1: Integration Test

**Files:**
- Create: `tests/scripts/test_dev_clean.sh`
- Read: `docs/superpowers/specs/2026-05-18-dev-clean-design.md`

- [ ] **Step 1: Write the failing test**

Create `tests/scripts/test_dev_clean.sh` with:

```bash
#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
source_script="$repo_root/scripts/dev-clean.sh"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

assert_exists() {
  if [[ ! -e "$1" ]]; then
    echo "expected path to exist: $1" >&2
    exit 1
  fi
}

assert_missing() {
  if [[ -e "$1" ]]; then
    echo "expected path to be removed: $1" >&2
    exit 1
  fi
}

assert_contains() {
  local haystack="$1"
  local needle="$2"
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "expected output to contain: $needle" >&2
    echo "$haystack" >&2
    exit 1
  fi
}

assert_not_contains() {
  local haystack="$1"
  local needle="$2"
  if [[ "$haystack" == *"$needle"* ]]; then
    echo "expected output not to contain: $needle" >&2
    echo "$haystack" >&2
    exit 1
  fi
}

run_clean() {
  "$tmp_dir/repo/scripts/dev-clean.sh" "$@"
}

mkdir -p "$tmp_dir/repo/scripts"
cp "$source_script" "$tmp_dir/repo/scripts/dev-clean.sh"
chmod +x "$tmp_dir/repo/scripts/dev-clean.sh"

cd "$tmp_dir/repo"
git init -q
git config user.email "test@example.invalid"
git config user.name "Dev Clean Test"
printf 'tracked\n' > README.md
git add README.md scripts/dev-clean.sh
git commit -q -m "initial"
git remote add origin "$tmp_dir/remote.git"

mkdir -p build build-cuda cmake-scratch arch-pkgbuild test-results playwright-report node_modules .codex .claude .private docs/.claude .stfolder
printf 'generated\n' > build/output.txt
printf 'generated\n' > build-cuda/output.txt
printf 'generated\n' > cmake-scratch/output.txt
printf 'generated\n' > arch-pkgbuild/output.txt
printf 'generated\n' > test-results/output.txt
printf 'generated\n' > playwright-report/output.txt
printf 'generated\n' > compile_commands.json
printf 'dependency\n' > node_modules/module.txt
printf 'private\n' > .codex
printf 'private\n' > .claude/settings.json
printf 'private\n' > .private/secret
printf 'private\n' > .envrc
printf 'private\n' > .stignore
printf 'private\n' > polaris.code-workspace
printf 'private\n' > docs/.claude/local.md
printf 'private\n' > .stfolder/marker

git switch -q -c feature/dev-clean-test
printf 'change\n' >> README.md
git stash push -q -m "dev-clean-test-stash"

status_output="$(run_clean status)"
assert_contains "$status_output" "Workspace"
assert_contains "$status_output" "Branches"
assert_contains "$status_output" "Stashes"
assert_contains "$status_output" "Worktrees"
assert_contains "$status_output" "Generated cleanup candidates"
assert_contains "$status_output" "build/"
assert_contains "$status_output" "stash@{0}"
assert_contains "$status_output" "feature/dev-clean-test"

dry_run_output="$(run_clean prune)"
assert_contains "$dry_run_output" "Dry run"
assert_contains "$dry_run_output" "build/"
assert_contains "$dry_run_output" "compile_commands.json"
assert_exists build/output.txt
assert_exists .codex
assert_exists .claude/settings.json
assert_exists .private/secret
assert_exists .envrc
assert_exists .stignore
assert_exists polaris.code-workspace
assert_exists docs/.claude/local.md

run_clean prune --apply >/tmp/dev-clean-prune.out
assert_missing build
assert_missing build-cuda
assert_missing cmake-scratch
assert_missing arch-pkgbuild
assert_missing test-results
assert_missing playwright-report
assert_missing compile_commands.json
assert_exists node_modules/module.txt
assert_exists .codex
assert_exists .claude/settings.json
assert_exists .private/secret
assert_exists .envrc
assert_exists .stignore
assert_exists polaris.code-workspace
assert_exists docs/.claude/local.md
assert_contains "$(git stash list)" "dev-clean-test-stash"
assert_contains "$(git branch --list)" "feature/dev-clean-test"

nuke_dry_run="$(run_clean nuke-local-builds)"
assert_contains "$nuke_dry_run" "node_modules/"
assert_exists node_modules/module.txt

run_clean nuke-local-builds --apply >/tmp/dev-clean-nuke.out
assert_missing node_modules
assert_exists .codex
assert_exists .claude/settings.json

git_prune_dry_run="$(run_clean git-prune)"
assert_contains "$git_prune_dry_run" "Dry run"
assert_contains "$git_prune_dry_run" "git fetch --prune origin"
assert_contains "$git_prune_dry_run" "git worktree prune"
assert_contains "$git_prune_dry_run" "git gc --prune=now"

if run_clean invalid-command >/tmp/dev-clean-invalid.out 2>&1; then
  echo "invalid command unexpectedly succeeded" >&2
  exit 1
fi
invalid_output="$(cat /tmp/dev-clean-invalid.out)"
assert_contains "$invalid_output" "Usage:"
assert_not_contains "$invalid_output" "rm -rf"

echo "dev-clean integration test passed"
```

- [ ] **Step 2: Run the test to verify it fails because the script is missing**

Run:

```bash
bash tests/scripts/test_dev_clean.sh
```

Expected: FAIL with a message from `cp` saying `scripts/dev-clean.sh` does not exist.

- [ ] **Step 3: Commit the failing test**

Run:

```bash
git add tests/scripts/test_dev_clean.sh
git commit -m "test: cover dev cleanup helper"
```

---

### Task 2: Cleanup Script

**Files:**
- Create: `scripts/dev-clean.sh`
- Test: `tests/scripts/test_dev_clean.sh`

- [ ] **Step 1: Implement the cleanup script**

Create `scripts/dev-clean.sh` with:

```bash
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

path_exists() {
  [[ -e "$1" || -L "$1" ]]
}

safe_remove() {
  local path="$1"
  case "$path" in
    ""|.|/|/*|../*|*/../*|.git|.git/*)
      echo "Refusing unsafe cleanup path: $path" >&2
      exit 2
      ;;
  esac

  if path_exists "$path"; then
    if [[ "$apply" -eq 1 ]]; then
      rm -rf -- "$path"
      printf 'Removed %s\n' "$path"
    else
      printf 'Would remove %s\n' "$path"
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
    printf '%s\n' "$candidates" | sed 's#/$##; s#$#/#' | sed 's#compile_commands.json/#compile_commands.json#'
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
      du -sh "$path"
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
```

- [ ] **Step 2: Make the script executable**

Run:

```bash
chmod +x scripts/dev-clean.sh
```

- [ ] **Step 3: Verify Bash syntax**

Run:

```bash
bash -n scripts/dev-clean.sh
```

Expected: exit 0 with no output.

- [ ] **Step 4: Run the integration test to verify green**

Run:

```bash
bash tests/scripts/test_dev_clean.sh
```

Expected: `dev-clean integration test passed`.

- [ ] **Step 5: Commit script implementation**

Run:

```bash
git add scripts/dev-clean.sh tests/scripts/test_dev_clean.sh
git commit -m "feat: add safe dev cleanup helper"
```

---

### Task 3: Documentation

**Files:**
- Modify: `docs/building.md`
- Test: `scripts/dev-clean.sh`

- [ ] **Step 1: Add docs under Development Notes**

Modify `docs/building.md` by replacing the Development Notes list with:

```markdown
## Development Notes

- The main local web build commands are `npm run lint` and `npm run build`.
- `build-tests/` is useful when you want to rebuild the served web bundle without touching your main local build.
- Polaris stores runtime config in `~/.config/polaris`.
- Use `scripts/dev-clean.sh status` before or after build sessions to inspect local branches, stashes, worktrees, generated artifacts, and repository size.
- Use `scripts/dev-clean.sh prune --apply` after throwaway local builds to remove allowlisted build/test outputs such as `build/`, `build-*`, `cmake-*`, `arch-pkgbuild/`, `test-results/`, and `playwright-report/`.
- Use `scripts/dev-clean.sh git-prune --apply` after deleting branches, stashes, or worktrees to prune stale remote refs, worktree metadata, and unreachable Git objects.
- Use `scripts/dev-clean.sh nuke-local-builds --apply` when you intentionally want to reset generated build outputs plus `node_modules/`.
```

- [ ] **Step 2: Verify status output on the real repo**

Run:

```bash
scripts/dev-clean.sh status
```

Expected: output includes `Workspace`, `Branches`, `Stashes`, `Worktrees`, `Generated cleanup candidates`, `Sizes`, and `Git objects`.

- [ ] **Step 3: Verify docs mention every command**

Run:

```bash
rg -n "dev-clean\\.sh (status|prune --apply|git-prune --apply|nuke-local-builds --apply)" docs/building.md
```

Expected: one or more matches for each command.

- [ ] **Step 4: Commit docs**

Run:

```bash
git add docs/building.md
git commit -m "docs: document dev cleanup workflow"
```

---

### Task 4: Final Verification

**Files:**
- Verify: `scripts/dev-clean.sh`
- Verify: `tests/scripts/test_dev_clean.sh`
- Verify: `docs/building.md`

- [ ] **Step 1: Run syntax and integration verification**

Run:

```bash
bash -n scripts/dev-clean.sh
bash tests/scripts/test_dev_clean.sh
```

Expected: syntax check exits 0 and integration test prints `dev-clean integration test passed`.

- [ ] **Step 2: Run read-only repo status verification**

Run:

```bash
scripts/dev-clean.sh status
```

Expected: exits 0 and prints the seven status sections.

- [ ] **Step 3: Check Git state**

Run:

```bash
git status -sb
git log --oneline --max-count=5
```

Expected: branch `dev-clean-tooling` with no uncommitted changes except ignored local files, and recent commits for the design, test, script, and docs.
