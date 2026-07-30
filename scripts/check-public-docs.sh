#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

python3 <<'PY'
from pathlib import Path
import re
import sys

root = Path.cwd()
readme = root / "README.md"
text = readme.read_text(encoding="utf-8")

targets = set()

for match in re.finditer(r'\]\(([^)]+)\)', text):
    target = match.group(1).strip()
    if "://" in target or target.startswith("#") or target.startswith("mailto:"):
        continue
    target = target.split("#", 1)[0].split("?", 1)[0]
    if target:
        targets.add(target)

for match in re.finditer(r'(?:src|srcset)=["\']([^"\']+)["\']', text):
    target = match.group(1).strip().split(",", 1)[0].strip().split(" ", 1)[0]
    if "://" in target or target.startswith("data:") or target.startswith("#"):
        continue
    target = target.split("#", 1)[0].split("?", 1)[0]
    if target:
        targets.add(target)

missing = sorted(str(path) for path in targets if not (root / path).exists())
if missing:
    print("README references missing local files:", file=sys.stderr)
    for path in missing:
        print(f"  - {path}", file=sys.stderr)
    sys.exit(1)
PY

expected_assets=(
  "Polaris-fedora44-x86_64.rpm"
  "Polaris-ubuntu24.04-x86_64.deb"
  "Polaris-arch-x86_64.pkg.tar.zst"
)

legacy_assets=(
  "Polaris-fedora42-x86_64.rpm"
  "Polaris-fedora43-x86_64.rpm"
)

variable_fedora_patterns=(
  'Polaris-fedora${'
  'fedora_version="$(rpm -E %fedora)"'
)

expected_nova_links=(
  "https://github.com/papi-ux/nova/releases/latest"
  "https://github-store.org/app?repo=papi-ux/nova"
  "versionExtractionRegEx%5C%22%3A%5C%22v%28.%2B%29"
  "app-nonRoot_game-arm64-v8a-release.apk"
)

files_to_check=(
  "README.md"
  "docs/building.md"
  "docs/changelog.md"
  ".github/workflows/build.yml"
)

current_docs=(
  "README.md"
  "docs/building.md"
  "docs/bazzite.md"
)

for expected_asset in "${expected_assets[@]}"; do
  for file in "${files_to_check[@]}"; do
    grep -Fq "$expected_asset" "$file"
  done
done

for legacy_asset in "${legacy_assets[@]}"; do
  for file in "${current_docs[@]}"; do
    if grep -Fq "$legacy_asset" "$file"; then
      echo "Legacy Fedora release asset remains in $file: $legacy_asset" >&2
      exit 1
    fi
  done
done

for variable_pattern in "${variable_fedora_patterns[@]}"; do
  for file in "${current_docs[@]}"; do
    if grep -Fq "$variable_pattern" "$file"; then
      echo "Variable-derived Fedora asset remains in $file: $variable_pattern" >&2
      exit 1
    fi
  done
done

for expected_link in "${expected_nova_links[@]}"; do
  grep -Fq "$expected_link" README.md
done

python3 <<'PY'
from collections import Counter
from html.parser import HTMLParser
from pathlib import Path
import re
import shlex
import sys


def fence_context_line(line: str) -> str:
    """Remove Markdown blockquote containers before testing a fence delimiter."""
    return re.sub(r"^(?:[ ]{0,3}>[ \t]?)+", "", line.rstrip("\r\n"))


def advance_fence(fence, line: str):
    """Advance CommonMark-style fenced-code state; return (state, delimiter_line)."""
    candidate = fence_context_line(line)
    if fence is None:
        opener = re.match(r"^[ ]{0,3}(`{3,}|~{3,})(.*)$", candidate)
        if opener:
            marker = opener.group(1)
            info = opener.group(2)
            if marker[0] == "`" and "`" in info:
                return None, False
            return (marker[0], len(marker)), True
        return None, False

    char, minimum = fence
    closer = re.match(
        rf"^[ ]{{0,3}}{re.escape(char)}{{{minimum},}}[ \t]*$",
        candidate,
    )
    if closer:
        return None, True
    return fence, False


def strip_html_comments(text: str) -> str:
    """Remove HTML comments outside fenced code while preserving line boundaries."""
    output: list[str] = []
    fence = None
    in_comment = False

    for line in text.splitlines(keepends=True):
        if fence is not None:
            output.append(line)
            fence, _ = advance_fence(fence, line)
            continue

        if not in_comment:
            next_fence, delimiter = advance_fence(None, line)
            if delimiter:
                output.append(line)
                fence = next_fence
                continue

        newline = ""
        body = line
        if body.endswith("\r\n"):
            body, newline = body[:-2], "\r\n"
        elif body.endswith("\n"):
            body, newline = body[:-1], "\n"

        visible: list[str] = []
        position = 0
        while position < len(body):
            if in_comment:
                end = body.find("-->", position)
                if end < 0:
                    position = len(body)
                    break
                in_comment = False
                position = end + 3
                continue
            start = body.find("<!--", position)
            if start < 0:
                visible.append(body[position:])
                break
            visible.append(body[position:start])
            in_comment = True
            position = start + 4

        output.append("".join(visible) + newline)

    return "".join(output)


building = strip_html_comments(Path("docs/building.md").read_text(encoding="utf-8"))
contributing = strip_html_comments(
    Path(".github/CONTRIBUTING.md").read_text(encoding="utf-8")
)
readme = strip_html_comments(Path("README.md").read_text(encoding="utf-8"))
changelog = strip_html_comments(Path("docs/changelog.md").read_text(encoding="utf-8"))


def markdown_section(
    text: str,
    heading: str,
    expected_following=None,
) -> str:
    """Return one exact Markdown section, bounded by a same/higher-level heading."""
    level = len(heading) - len(heading.lstrip("#"))
    if level < 1 or heading[level:level + 1] != " ":
        raise ValueError(f"Invalid heading: {heading}")

    lines = text.splitlines(keepends=True)
    starts = []
    fence = None
    for index, line in enumerate(lines):
        fence, delimiter = advance_fence(fence, line)
        if delimiter or fence is not None:
            continue
        if line.rstrip("\r\n") == heading:
            starts.append(index)
    if len(starts) != 1:
        print(f"Expected exactly one Markdown heading: {heading}", file=sys.stderr)
        sys.exit(1)

    start = starts[0] + 1
    boundary = re.compile(rf"^#{{1,{level}}}\s+")
    fence = None
    end = len(lines)
    for index in range(start, len(lines)):
        fence, delimiter = advance_fence(fence, lines[index])
        if delimiter or fence is not None:
            continue
        if boundary.match(lines[index]):
            end = index
            break
    if expected_following is not None:
        actual_following = lines[end].rstrip("\r\n") if end < len(lines) else None
        if actual_following != expected_following:
            print(
                f"Expected {expected_following!r} immediately after {heading!r}; "
                f"found {actual_following!r}",
                file=sys.stderr,
            )
            sys.exit(1)
    return "".join(lines[start:end])


class VisibleHTMLText(HTMLParser):
    """Collect browser-visible text while excluding hidden/raw non-content elements."""

    void_tags = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    }
    noncontent_tags = {"script", "style", "template"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.stack = []
        self.hidden_depth = 0

    def _is_hidden(self, tag, attrs):
        values = {name.lower(): (value or "") for name, value in attrs}
        if tag in self.noncontent_tags or "hidden" in values:
            return True
        if values.get("aria-hidden", "").strip().lower() == "true":
            return True
        style = values.get("style", "")
        return bool(re.search(r"(?:^|;)\s*display\s*:\s*none(?:\s*!important)?\s*(?:;|$)", style, re.I))

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        hidden = self._is_hidden(tag, attrs)
        if tag in self.void_tags:
            return
        self.stack.append((tag, hidden))
        if hidden:
            self.hidden_depth += 1

    def handle_startendtag(self, tag, attrs):
        return

    def handle_endtag(self, tag):
        tag = tag.lower()
        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index][0] == tag:
                removed = self.stack[index:]
                del self.stack[index:]
                self.hidden_depth -= sum(1 for _, hidden in removed if hidden)
                return

    def handle_data(self, data):
        if self.hidden_depth == 0:
            self.parts.append(data)


def visible_html_text(text: str) -> str:
    parser = VisibleHTMLText()
    parser.feed(text)
    parser.close()
    return "".join(parser.parts)


def rendered_markdown(text: str) -> str:
    """Return visible Markdown text with fenced blocks and destinations excluded."""
    rendered: list[str] = []
    fence = None
    for line in text.splitlines(keepends=True):
        fence, delimiter = advance_fence(fence, line)
        if delimiter or fence is not None:
            continue
        if re.match(r"^[ ]{0,3}\[[^]]+\]:\s+", line):
            continue
        rendered.append(line)

    visible = "".join(rendered)
    visible = re.sub(
        r"!?\[([^]]*)\]\((?:\\.|[^)\n])*\)",
        lambda match: match.group(1),
        visible,
    )
    visible = re.sub(
        r"!?\[([^]]*)\]\[[^]]*\]",
        lambda match: match.group(1),
        visible,
    )
    return visible_html_text(visible)


def markdown_table(section: str, label: str) -> list[list[str]]:
    """Parse the first contiguous Markdown table in a bounded section."""
    blocks: list[list[str]] = []
    current: list[str] = []
    fence = None
    for line in section.splitlines():
        fence, delimiter = advance_fence(fence, line)
        if delimiter:
            if current:
                blocks.append(current)
                current = []
            continue
        if fence is not None:
            continue
        if line.strip().startswith("|"):
            current.append(line)
        elif current:
            blocks.append(current)
            current = []
    if current:
        blocks.append(current)
    if not blocks:
        print(f"{label} is missing its Markdown table", file=sys.stderr)
        sys.exit(1)

    rows = [
        [cell.strip() for cell in line.strip().strip("|").split("|")]
        for line in blocks[0]
    ]
    if len(rows) < 3 or rows[0] != ["Tool", "Notes"]:
        print(f"{label} has an invalid Tool/Notes table", file=sys.stderr)
        sys.exit(1)
    if len(rows[1]) != 2 or not all(
        re.fullmatch(r":?-{3,}:?", cell) for cell in rows[1]
    ):
        print(f"{label} has an invalid table separator", file=sys.stderr)
        sys.exit(1)
    return rows[2:]


def tokenize_shell_command(command: str) -> list[str]:
    lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|<>")
    lexer.whitespace_split = True
    lexer.commenters = "#"
    return list(lexer)


def shell_commands(block: str) -> list[list[str]]:
    """Return tokenized, uncommented logical shell commands from a fenced block."""
    commands: list[list[str]] = []
    pending = ""
    for raw_line in block.splitlines():
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        continued = stripped.endswith("\\")
        fragment = stripped[:-1].rstrip() if continued else stripped
        pending = f"{pending} {fragment}".strip()
        if continued:
            continue
        commands.append(tokenize_shell_command(pending))
        pending = ""
    if pending:
        commands.append(tokenize_shell_command(pending))
    return commands


requirements_section = markdown_section(
    building,
    "### Requirements",
    "### Example packages",
)
requirements_rows = markdown_table(requirements_section, "docs/building.md Requirements")
compiler_rows = [row for row in requirements_rows if row and row[0].startswith("C++")]
if compiler_rows != [["C++23 compiler", "GCC or Clang"]]:
    print(
        "docs/building.md Requirements table must contain exactly the C++23 compiler row",
        file=sys.stderr,
    )
    sys.exit(1)

arch_section = markdown_section(
    building,
    "#### Arch / CachyOS",
    "#### openSUSE Tumbleweed",
)
arch_block = re.search(
    r"(?ms)^```bash\s*$\n(?P<commands>.*?)^```\s*$",
    arch_section,
)
if not arch_block:
    print("docs/building.md is missing the bounded Arch/CachyOS install command", file=sys.stderr)
    sys.exit(1)
arch_commands = shell_commands(arch_block.group("commands"))
pacman_commands = [
    tokens
    for tokens in arch_commands
    if len(tokens) >= 3 and tokens[:2] == ["sudo", "pacman"] and "-S" in tokens
]
if len(pacman_commands) != 1:
    print("Arch/CachyOS block must contain exactly one sudo pacman -S command", file=sys.stderr)
    sys.exit(1)
arch_command = pacman_commands[0]
if any(token and set(token) <= set(";&|<>") for token in arch_command):
    print("Arch/CachyOS pacman command must not contain chained shell operators", file=sys.stderr)
    sys.exit(1)
arch_tokens = set(arch_command)
for dependency in ("vulkan-headers", "vulkan-icd-loader"):
    if dependency not in arch_tokens:
        print(
            f"Arch/CachyOS install command is missing dependency: {dependency}",
            file=sys.stderr,
        )
        sys.exit(1)

current_release = markdown_section(
    changelog,
    "## v1.3.2 - 2026-07-29",
    "## v1.3.1 - 2026-07-12",
)
current_release_prose = rendered_markdown(current_release)
required_release_facts = (
    "webtransport-go v0.11.1",
    "quic-go v0.60.0",
    "CVE-2026-57497",
    "npm audit --audit-level=high",
    "Polaris-arch-x86_64.pkg.tar.zst",
    "Polaris-fedora44-x86_64.rpm",
    "Polaris-ubuntu24.04-x86_64.deb",
    "vulkan-headers",
    "vulkan-loader-devel",
    "GCC 15",
)
for fact in required_release_facts:
    if fact not in current_release_prose:
        print(f"v1.3.2 changelog is missing final release fact: {fact}", file=sys.stderr)
        sys.exit(1)

readme_release_body = markdown_section(
    readme,
    "## What is New in v1.3.2",
    "## Install",
)
readme_release_prose = rendered_markdown(readme_release_body)
required_readme_facts = (
    "webtransport-go v0.11.1",
    "quic-go v0.60.0",
    "CVE-2026-57497",
    "npm audit",
    "Polaris-arch-x86_64.pkg.tar.zst",
    "Polaris-fedora44-x86_64.rpm",
    "Polaris-ubuntu24.04-x86_64.deb",
    "Vulkan",
    "GCC 15",
)
for fact in required_readme_facts:
    if fact not in readme_release_prose:
        print(f"README v1.3.2 summary is missing: {fact}", file=sys.stderr)
        sys.exit(1)

asset_phrase = (
    "`Polaris-arch-x86_64.pkg.tar.zst`, "
    "`Polaris-fedora44-x86_64.rpm`, and "
    "`Polaris-ubuntu24.04-x86_64.deb`"
)
for label, section in (
    ("README v1.3.2 summary", readme_release_prose),
    ("v1.3.2 changelog", current_release_prose),
):
    if section.count(asset_phrase) != 1:
        print(f"{label} must contain the exact visible three-asset phrase", file=sys.stderr)
        sys.exit(1)

expected_assets = Counter(
    {
        "Polaris-arch-x86_64.pkg.tar.zst": 1,
        "Polaris-fedora44-x86_64.rpm": 1,
        "Polaris-ubuntu24.04-x86_64.deb": 1,
    }
)
asset_pattern = re.compile(r"Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*")
for label, section in (
    ("README v1.3.2 summary", readme_release_prose),
    ("v1.3.2 changelog", current_release_prose),
):
    actual_assets = Counter(asset_pattern.findall(section))
    if actual_assets != expected_assets:
        print(
            f"{label} must name exactly one of each supported asset; "
            f"expected={dict(expected_assets)}, actual={dict(actual_assets)}",
            file=sys.stderr,
        )
        sys.exit(1)

if "bash scripts/check-public-docs.sh" not in contributing:
    print("CONTRIBUTING.md must invoke the non-executable checker through bash", file=sys.stderr)
    sys.exit(1)
if "./scripts/check-public-docs.sh" in contributing:
    print("CONTRIBUTING.md must not execute the mode-100644 checker directly", file=sys.stderr)
    sys.exit(1)
PY

echo "Public docs and release references look clean."
