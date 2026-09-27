#!/usr/bin/env bash
# Checks the commit subjects a branch adds, against the conventional-commit form the
# history already overwhelmingly uses: 173 of the last 200 subjects on master carried a
# conventional prefix before this gate existed.
#
# It only ever reads the commits a change adds. The history behind them holds subjects
# that predate the convention, and rewriting those is not worth a gate.
#
# Usage:
#   scripts/check-commit-subjects.sh [<base> [<head>]]
#
# With no arguments it works out the range: GITHUB_BASE_REF on a pull request, otherwise
# the upstream of the current branch, otherwise the default branch.
set -euo pipefail

# The Conventional Commits type set, and nothing besides. A synonym reads as tidy only to
# someone who already knows the local dialect, which is the opposite of what a shared
# convention buys: every changelog tool already knows what `fix` means and none of them
# know what `repair` means.
readonly ALLOWED_TYPES='feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert'

# A scope is lowercase, digits and hyphens. Both repos already agree: `play-setup` beats
# `play setup` 21 to 3 in Nova, `game-mode` appears 17 times, and there is not one
# space-containing scope in 400 Polaris subjects.
readonly SCOPE='[a-z0-9]+(-[a-z0-9]+)*'

readonly DEFAULT_BRANCH="${CHECK_DEFAULT_BRANCH:-master}"

resolve_range() {
  if [[ $# -ge 1 && -n "${1:-}" ]]; then
    printf '%s..%s\n' "$1" "${2:-HEAD}"
    return
  fi

  if [[ -n "${GITHUB_BASE_REF:-}" ]]; then
    local base="origin/${GITHUB_BASE_REF}"
    if git rev-parse --verify --quiet "$base" >/dev/null; then
      printf '%s..%s\n' "$base" "HEAD"
      return
    fi
    if git rev-parse --verify --quiet "${GITHUB_BASE_REF}" >/dev/null; then
      printf '%s..%s\n' "${GITHUB_BASE_REF}" "HEAD"
      return
    fi
  fi

  local upstream
  if upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null); then
    printf '%s..%s\n' "$upstream" "HEAD"
    return
  fi

  for candidate in "origin/${DEFAULT_BRANCH}" "${DEFAULT_BRANCH}"; do
    if git rev-parse --verify --quiet "$candidate" >/dev/null; then
      printf '%s..%s\n' "$candidate" "HEAD"
      return
    fi
  done

  echo "check-commit-subjects: cannot work out which commits are new; pass a base explicitly" >&2
  exit 2
}

range=$(resolve_range "$@")

# Merge commits carry a subject the forge wrote, not one anybody chose, so they are not
# this gate's business.
mapfile -t commits < <(git rev-list --no-merges "$range" 2>/dev/null || true)

if [[ ${#commits[@]} -eq 0 ]]; then
  echo "check-commit-subjects: no new commits in ${range}, nothing to check"
  exit 0
fi

failures=0

fail() {
  printf '  %s\n' "$1" >&2
  failures=$((failures + 1))
}

for sha in "${commits[@]}"; do
  subject=$(git log -1 --format='%s' "$sha")
  short="${sha:0:9}"

  # `git revert` writes this one itself.
  if [[ "$subject" == Revert\ \"* ]]; then
    continue
  fi

  printf '%s %s\n' "$short" "$subject"

  if [[ "$subject" =~ [—–] ]]; then
    fail "$short: an em or en dash in the subject. papi's rule is no dashes: rewrite the sentence."
    continue
  fi

  if [[ "$subject" == *" - "* ]]; then
    fail "$short: a spaced hyphen used as a connector. Rewrite it as a sentence, a colon or a comma."
    continue
  fi

  if [[ ! "$subject" =~ ^[a-z]+(\([^\)]*\))?!?:\  ]]; then
    fail "$short: no conventional prefix. Expected <type>(<scope>): <subject>, for example fix(play-setup): ..."
    continue
  fi

  type="${subject%%[(:!]*}"
  if [[ ! "$type" =~ ^($ALLOWED_TYPES)$ ]]; then
    fail "$short: '$type' is not a type. The set is ${ALLOWED_TYPES//|/, }. If '$type' names the area, it belongs in the scope: fix($type): ... or chore($type): ..."
    continue
  fi

  if [[ "$subject" =~ ^[a-z]+\(([^\)]*)\) ]]; then
    scope="${BASH_REMATCH[1]}"
    if [[ -z "$scope" ]]; then
      fail "$short: an empty scope. Either name one or drop the parentheses."
      continue
    fi
    if [[ ! "$scope" =~ ^$SCOPE$ ]]; then
      suggestion="${scope// /-}"
      suggestion="${suggestion//_/-}"
      suggestion=$(printf '%s' "$suggestion" | tr '[:upper:]' '[:lower:]')
      fail "$short: scope '$scope' is not lowercase and hyphenated. Use ($suggestion)."
      continue
    fi
  fi

  text="${subject#*: }"
  if [[ -z "$text" ]]; then
    fail "$short: nothing after the colon."
    continue
  fi
  if [[ "$text" == *. ]]; then
    fail "$short: the subject ends with a period. Subjects are titles, not sentences."
    continue
  fi
done

if [[ $failures -gt 0 ]]; then
  printf '\ncheck-commit-subjects: %d subject(s) need fixing in %s\n' "$failures" "$range" >&2
  exit 1
fi

printf '\ncheck-commit-subjects: %d subject(s) in %s all conform\n' "${#commits[@]}" "$range"
