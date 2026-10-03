#!/usr/bin/env bash
# Exercises scripts/check-commit-subjects.sh against subjects it must accept and subjects
# it must reject, in a throwaway repository. A gate nobody tested is a gate that fails the
# wrong things, and this one rejects on a regex, which is exactly the kind of rule that
# quietly rejects everything.
set -euo pipefail

gate="${1:?path to check-commit-subjects.sh}"
gate=$(cd "$(dirname "$gate")" && pwd)/$(basename "$gate")

if [[ ! -x "$gate" ]]; then
  echo "test-check-commit-subjects: $gate is not executable." >&2
  echo "  This repository sets core.fileMode=false, so chmod alone does not reach the index." >&2
  echo "  Fix it with: git update-index --chmod=+x ${gate##*/scripts/}" >&2
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

git init -q -b master .
git config user.name papi
git config user.email papi@papi-ux.com
git commit -q --allow-empty -m 'root: a subject from before the convention existed'
base=$(git rev-parse HEAD)

failures=0

check() {
  local expectation="$1" subject="$2"
  git checkout -q -B probe "$base"
  git commit -q --allow-empty -m "$subject"
  if bash "$gate" "$base" probe >/dev/null 2>&1; then
    result=accept
  else
    result=reject
  fi
  if [[ "$result" == "$expectation" ]]; then
    printf '  ok      %-7s %s\n' "$result" "$subject"
  else
    printf '  FAILED  wanted %s, got %s: %s\n' "$expectation" "$result" "$subject"
    failures=$((failures + 1))
  fi
}

echo "must accept:"
check accept 'fix(play-setup): Tuning keeps its presets while the host plan is rechecked'
check accept 'feat(spaces): a Space borrows the host NVIDIA driver'
check accept 'fix: a subject with no scope at all'
check accept 'chore(release): cut 1.4.13'
check accept 'feat(game-mode)!: a breaking change marker'
check accept 'docs(update-center): say which package an upgrade moves'
check accept 'perf(pyrowave): zero copy on the import path'
check accept 'test(kms): pin the helper detection'
check accept 'build(arch): turn LTO off because volk cannot link under it'
check accept 'ci(steamos): pin the pacman archive date'
check accept 'refactor(confighttp): move the predicate to namespace scope'
check accept 'revert(video): undo the clamp that crashes RADV'
check accept 'style(web): run the formatter'
check accept 'fix(device-db): a scope with digits and hyphens 2'
check accept 'Revert "fix(video): a thing git wrote itself"'

echo "must reject:"
check reject 'repair(play-setup): a synonym no tool knows'
check reject 'release(nova): a scope wearing a type'
check reject 'library: another scope wearing a type'
check reject 'fix(play setup): a scope with a space'
check reject 'fix(launch_profile): a scope with an underscore'
check reject 'fix(Play-Setup): a capitalised scope'
check reject 'no prefix at all here'
check reject 'fix(update-center): a subject ending in a period.'
check reject 'fix(): an empty scope'
check reject 'fix(video): an em dash — right here'
check reject 'fix(video): an en dash – right here'
check reject 'fix(video): a spaced hyphen - used as a connector'
check reject 'Fix(video): a capitalised type'

# The two accepted historical commits are pinned by full ID and exact subject.
# Fresh commits carrying those same malformed scopes must still be rejected.
check reject "fix(stream_stats): serve a verdict's figure with the band it was judged into"
check reject 'feat(stream_stats): judge video frame loss and RTT over a 20 second window'

# These immutable commits cannot be recreated in the throwaway repo. The shim
# supplies only the two Git reads the gate uses, so the real validation path
# still decides whether the ID/subject pair is accepted.
mkdir -p "$tmp/history-fixture"
cat > "$tmp/history-fixture/git" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
case "$*" in
  'rev-list --no-merges history-base..history-head')
    printf '%s\n' "$CHECK_SUBJECT_FIXTURE_SHA"
    ;;
  "log -1 --format=%s $CHECK_SUBJECT_FIXTURE_SHA")
    printf '%s\n' "$CHECK_SUBJECT_FIXTURE_TEXT"
    ;;
  *) echo "unexpected history fixture git call: $*" >&2; exit 2 ;;
esac
SH
chmod +x "$tmp/history-fixture/git"

history_check() {
  local expectation="$1" sha="$2" subject="$3" result
  if PATH="$tmp/history-fixture:$PATH" \
      CHECK_SUBJECT_FIXTURE_SHA="$sha" CHECK_SUBJECT_FIXTURE_TEXT="$subject" \
      bash "$gate" history-base history-head >/dev/null 2>&1; then
    result=accept
  else
    result=reject
  fi
  if [[ "$result" == "$expectation" ]]; then
    printf '  ok      %-7s historical identity %s\n' "$result" "$sha"
  else
    printf '  FAILED  wanted %s, got %s for historical identity %s\n' "$expectation" "$result" "$sha"
    failures=$((failures + 1))
  fi
}

echo "historical identities:"
history_check accept 3581a073a749b9abd979d5f2b7f4737adbdac578 \
  "fix(stream_stats): serve a verdict's figure with the band it was judged into"
history_check accept 0171868a989f233927d629c6aa880b644fa3ce2b \
  'feat(stream_stats): judge video frame loss and RTT over a 20 second window'
history_check reject 3581a073a749b9abd979d5f2b7f4737adbdac578 \
  'fix(stream_stats): a different malformed historical subject'
history_check reject 0171868a989f233927d629c6aa880b644fa3ce2b \
  'feat(Stream-Stats): a different malformed historical subject'
history_check reject 3581a073a749b9abd979d5f2b7f4737adbdac579 \
  "fix(stream_stats): serve a verdict's figure with the band it was judged into"

echo
echo "range handling:"
git checkout -q -B probe "$base"
if out=$(bash "$gate" "$base" probe 2>&1) && printf '%s' "$out" | grep -q 'nothing to check'; then
  printf '  ok      an empty range passes and says so\n'
else
  printf '  FAILED  an empty range should pass: %s\n' "$out"
  failures=$((failures + 1))
fi

# A merge commit's subject is written by the forge, so the gate must ignore it.
git checkout -q -B side "$base"
git commit -q --allow-empty -m 'fix(video): a change on a side branch'
git checkout -q -B probe "$base"
git commit -q --allow-empty -m 'fix(audio): a change on the main branch'
git merge -q --no-ff -m 'Merge pull request #1 from papi-ux/side' side
if bash "$gate" "$base" probe >/dev/null 2>&1; then
  printf '  ok      a merge commit subject is ignored\n'
else
  printf '  FAILED  a merge commit subject should be ignored\n'
  failures=$((failures + 1))
fi

echo
if [[ $failures -gt 0 ]]; then
  echo "test-check-commit-subjects: $failures case(s) wrong"
  exit 1
fi
echo "test-check-commit-subjects: every case behaved"
