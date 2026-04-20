#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

status=0

if command -v rg >/dev/null 2>&1; then
  file_search() {
    rg "$@"
  }
else
  file_search() {
    grep -E "$@"
  }
fi

declare -a blocked_exact=(
  "CLAUDE.md"
  "AGENTS.md"
  "docs/agent_handoff.md"
)

for path in "${blocked_exact[@]}"; do
  if git ls-files --error-unmatch "$path" >/dev/null 2>&1 && [[ -e "$path" ]]; then
    echo "Tracked private/development file detected: $path" >&2
    status=1
  fi
done

path_hits="$(
  git ls-files | file_search '(^build-local/|(^|/)\.codex/|(^|/)\.claude/|(^|/)docs/\.claude/|(^|/)\.private/|(^|/)\.envrc$|(^|/)\.direnv/|(^|/).+\.code-workspace$|(^|/)local\.properties$|(^|/)signing\.properties$|(^|/).+\.(keystore|jks)$|(^|/)docs/session-notes-[0-9-]+\.md$)' || true
)"
if [[ -n "$path_hits" ]]; then
  echo "Tracked files that should stay out of the public repo:" >&2
  echo "$path_hits" >&2
  status=1
fi

content_hits="$(
  git grep -nIE '(agent handoff|project memory anchor|VS Code sessions|(^|[^[:alnum:]_])(/home/[^/[:space:]]+|/Users/[^/[:space:]]+)|tinyurl\.com)' -- . ':(exclude)scripts/check-public-surface.sh' || true
)"
if [[ -n "$content_hits" ]]; then
  echo "Suspicious private or maintainer-only strings detected in tracked files:" >&2
  echo "$content_hits" >&2
  status=1
fi

if [[ "$status" -ne 0 ]]; then
  echo "Public repo surface check failed." >&2
  exit "$status"
fi

echo "Public repo surface looks clean."
