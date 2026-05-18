# Dev Cleanup Tool Design

## Purpose

Polaris needs a committed maintenance tool that keeps local build work from accumulating into stale build trees, temporary package outputs, unreachable Git objects, and hard-to-audit workspace state. The tool should support daily development without touching private local files or active feature work.

## Scope

Add a Bash script at `scripts/dev-clean.sh` with read-only reporting and explicit cleanup modes. The tool is for developer workstations and CI-adjacent manual cleanup, not runtime host cleanup.

The tool will not delete local branches, stashes, private agent/editor files, configured remotes, submodules, or active worktrees. It may report those items so a developer can decide what to inspect manually.

## Commands

`scripts/dev-clean.sh status`

Prints a read-only report:

- `git status -sb`
- local branches with upstream tracking
- stash list
- worktree list
- ignored generated artifacts from an allowlist
- size of `.`, `.git`, `node_modules`, and `third-party` when present
- Git object-store summary from `git count-objects -vH`

`scripts/dev-clean.sh prune`

Removes generated build/test artifacts from a fixed allowlist only when called with `--apply`. Without `--apply`, it prints the paths that would be removed. The allowlist includes top-level generated paths such as `build/`, `build-*`, `cmake-*`, `arch-pkgbuild/`, `test-results/`, `playwright-report/`, and `compile_commands.json`.

`scripts/dev-clean.sh git-prune`

Runs safe Git maintenance only when called with `--apply`: `git fetch --prune` for configured remotes, `git worktree prune`, and `git gc --prune=now`. Without `--apply`, it reports the commands it would run.

`scripts/dev-clean.sh nuke-local-builds`

Extends `prune` by also allowing removal of `node_modules/`. This mode still requires `--apply` and still uses an explicit allowlist.

## Safety Rules

- Dry-run is the default for every destructive mode.
- Destructive modes require `--apply`.
- Deletion candidates must be top-level paths from a hard-coded allowlist or match narrow top-level generated directory patterns.
- The script must refuse to remove paths outside the repository root.
- The script must not use `git clean -xdf` as its implementation because the repo intentionally contains ignored private files that should survive cleanup.
- The script must not delete `.codex`, `.claude`, `.private`, `.envrc`, `.stignore`, workspace files, `docs/.claude/`, or active worktrees.
- Branch and stash cleanup remains report-only.

## Documentation

Document the normal workflow in `docs/building.md`:

1. Run `scripts/dev-clean.sh status` before or after a build session.
2. Run `scripts/dev-clean.sh prune --apply` after throwaway local builds.
3. Run `scripts/dev-clean.sh git-prune --apply` after deleting branches, worktrees, or stashes.
4. Run `scripts/dev-clean.sh nuke-local-builds --apply` when the web dependency/build state should be fully reset.

## Testing

Add a Bash integration test under `tests/scripts/test_dev_clean.sh`. The test should create a temporary Git repo, copy in `scripts/dev-clean.sh`, create generated paths and protected ignored paths, then verify:

- dry-run reports removal candidates but keeps files;
- `prune --apply` removes generated paths;
- protected ignored files survive cleanup;
- branch and stash state is reported but not deleted;
- `nuke-local-builds --apply` removes `node_modules/`;
- invalid commands fail with usage output.

Use `bash -n scripts/dev-clean.sh` and `bash tests/scripts/test_dev_clean.sh` for verification.
