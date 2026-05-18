#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
source_script="$repo_root/scripts/dev-clean.sh"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

if [[ ! -x "$source_script" ]]; then
  echo "expected scripts/dev-clean.sh to be executable" >&2
  exit 1
fi

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

mkdir -p build build-cuda cmake-scratch arch-pkgbuild test-results playwright-report node_modules .claude .private docs/.claude .stfolder
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

du_bin="$(command -v du)"
mkdir -p "$tmp_dir/bin"
cat > "$tmp_dir/bin/du" <<FAKE_DU
#!/usr/bin/env bash
if [[ "\$*" == *".git"* ]]; then
  echo "du: simulated transient failure" >&2
  exit 1
fi
exec "$du_bin" "\$@"
FAKE_DU
chmod +x "$tmp_dir/bin/du"

status_with_du_failure="$(PATH="$tmp_dir/bin:$PATH" run_clean status)"
assert_contains "$status_with_du_failure" "Sizes"
assert_contains "$status_with_du_failure" "unavailable"
assert_contains "$status_with_du_failure" "Git objects"

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
