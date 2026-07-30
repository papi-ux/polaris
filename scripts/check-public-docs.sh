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


def is_indented_code(line: str) -> bool:
    candidate = fence_context_line(line)
    return candidate.startswith("\t") or candidate.startswith("    ")


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


INLINE_CODE_LT = "\uf000inline-code-lt\uf001"
INLINE_CODE_GT = "\uf000inline-code-gt\uf001"


def backtick_is_escaped(text: str, index: int) -> bool:
    backslashes = 0
    index -= 1
    while index >= 0 and text[index] == "\\":
        backslashes += 1
        index -= 1
    return backslashes % 2 == 1


def backtick_run_end(text: str, start: int) -> int:
    end = start
    while end < len(text) and text[end] == "`":
        end += 1
    return end


def blockquote_context(line: str):
    depth = 0
    while True:
        match = re.match(r"^[ \t]{0,3}>[ \t]?", line)
        if not match:
            return depth, line
        depth += 1
        line = line[match.end() :]


def line_block_kind(line: str) -> str:
    """Classify block lines that cannot continue an inline-code paragraph."""
    _, content = blockquote_context(line)
    if not content.strip():
        return "blank"
    if is_indented_code(content):
        return "indented"
    _, fence_delimiter = advance_fence(None, content)
    if fence_delimiter:
        return "opaque"
    stripped = content.lstrip(" ")
    lower = stripped.lower()
    html_block_tag = (
        r"address|article|aside|base|basefont|blockquote|body|caption|center|col|"
        r"colgroup|dd|details|dialog|dir|div|dl|dt|fieldset|figcaption|figure|"
        r"footer|form|frame|frameset|h[1-6]|head|header|hr|html|iframe|legend|"
        r"li|link|main|menu|menuitem|nav|noframes|ol|optgroup|option|p|param|"
        r"pre|script|search|section|style|summary|table|tbody|td|textarea|tfoot|"
        r"th|thead|title|tr|track|ul"
    )
    if (
        lower.startswith(("<!--", "<?", "<![cdata["))
        or re.match(r"<![A-Z]", stripped)
        or re.match(rf"</?(?:{html_block_tag})(?:\s|/?>|$)", stripped, re.I)
    ):
        return "opaque"
    if re.match(r"(?:[-+*]|\d{1,9}[.)])\s+", stripped):
        return "list"
    if (
        re.match(r"#{1,6}(?:\s|$)", stripped)
        or re.match(r"(?:\*\s*){3,}$|(?:-\s*){3,}$|(?:_\s*){3,}$", stripped)
    ):
        return "single"
    return "paragraph"


def list_content_indent(line: str):
    _, content = blockquote_context(line)
    match = re.match(r"^(?:[-+*]|\d{1,9}[.)])([ \t]+)", content.lstrip(" "))
    if match is None:
        return None
    marker_offset = len(content) - len(content.lstrip(" "))
    prefix = content[:marker_offset] + content.lstrip(" ")[: match.end()]
    return len(prefix.expandtabs(4))


def backtick_closer_map(text: str):
    """Map opener runs to equal runs in the same CommonMark inline container."""
    closers = {}
    segments = []
    paragraph_start = None
    paragraph_depth = None
    paragraph_list_indent = None
    offset = 0

    def close_paragraph(end: int) -> None:
        nonlocal paragraph_start, paragraph_depth, paragraph_list_indent
        if paragraph_start is not None:
            segments.append((paragraph_start, end))
            paragraph_start = None
            paragraph_depth = None
            paragraph_list_indent = None

    for line in text.splitlines(keepends=True):
        line_end = offset + len(line)
        depth, _ = blockquote_context(line)
        kind = line_block_kind(line)
        if (
            kind == "indented"
            and paragraph_list_indent is not None
            and depth == paragraph_depth
        ):
            kind = "paragraph"
        if kind == "paragraph":
            if paragraph_start is None:
                paragraph_start = offset
                paragraph_depth = depth
                paragraph_list_indent = None
            elif depth > paragraph_depth:
                close_paragraph(offset)
                paragraph_start = offset
                paragraph_depth = depth
                paragraph_list_indent = None
            elif depth < paragraph_depth:
                # Unmarked lazy blockquote continuation remains in the open paragraph.
                pass
        elif kind == "list":
            close_paragraph(offset)
            paragraph_start = offset
            paragraph_depth = depth
            paragraph_list_indent = list_content_indent(line)
        else:
            close_paragraph(offset)
            if kind == "single":
                segments.append((offset, line_end))
        offset = line_end
    close_paragraph(len(text))

    for segment_start, segment_end in segments:
        runs = []
        position = segment_start
        while position < segment_end:
            if text[position] == "`":
                end = backtick_run_end(text, position)
                runs.append((position, end))
                position = end
                continue
            position += 1

        next_by_length = {}
        for run_start, run_end in reversed(runs):
            raw_length = run_end - run_start
            opener_start = run_start + 1 if backtick_is_escaped(text, run_start) else run_start
            opener_length = run_end - opener_start
            if opener_length:
                closers[opener_start] = next_by_length.get(opener_length)
            next_by_length[raw_length] = (run_start, run_end)
    return closers


def protect_inline_code_markup(text: str) -> str:
    """Protect rendered code-span markup, but not markup inside HTML comments."""
    if INLINE_CODE_LT in text or INLINE_CODE_GT in text:
        raise ValueError("Markdown contains reserved inline-code sentinel text")

    closers = backtick_closer_map(text)
    output = []
    position = 0
    while position < len(text):
        if text.startswith("<!--", position):
            end = text.find("-->", position + 4)
            if end < 0:
                output.append(text[position:])
                break
            output.append(text[position:end + 3])
            position = end + 3
            continue

        if text[position] == "`" and not backtick_is_escaped(text, position):
            opener_end = backtick_run_end(text, position)
            closing = closers.get(position)
            if closing is not None:
                _, closer_end = closing
                span = text[position:closer_end]
                output.append(
                    span.replace("<", INLINE_CODE_LT).replace(">", INLINE_CODE_GT)
                )
                position = closer_end
                continue
            output.append(text[position:opener_end])
            position = opener_end
            continue

        output.append(text[position])
        position += 1

    return "".join(output)


def restore_inline_code_markup(text: str) -> str:
    return text.replace(INLINE_CODE_LT, "<").replace(INLINE_CODE_GT, ">")


def strip_html_comments(text: str) -> str:
    """Remove HTML comments outside fenced/inline code while preserving lines."""
    text = protect_inline_code_markup(text)
    output: list[str] = []
    fence = None
    in_comment = False
    lines = text.splitlines(keepends=True)
    suffix_has_comment_close = [False] * (len(lines) + 1)
    for index in range(len(lines) - 1, -1, -1):
        suffix_has_comment_close[index] = (
            "-->" in lines[index] or suffix_has_comment_close[index + 1]
        )

    for line_index, line in enumerate(lines):
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
            if (
                body.find("-->", start + 4) < 0
                and not suffix_has_comment_close[line_index + 1]
            ):
                visible.append(body[start:])
                break
            in_comment = True
            position = start + 4

        output.append("".join(visible) + newline)

    return restore_inline_code_markup("".join(output))


def verify_html_comment_parser() -> None:
    cases = (
        ("visible <!-- hidden --> prose", "visible  prose", "ordinary HTML comment"),
        (
            "visible <!-- unclosed Polaris-extra-x86_64.AppImage",
            "visible <!-- unclosed Polaris-extra-x86_64.AppImage",
            "unclosed comment marker rendered literally",
        ),
        (
            "`<!--` Polaris-extra-x86_64.AppImage `-->`",
            "`<!--` Polaris-extra-x86_64.AppImage `-->`",
            "comment delimiters inside an inline code span",
        ),
        (
            "``code <!-- visible --> with ` tick``",
            "``code <!-- visible --> with ` tick``",
            "comment delimiters inside a multi-backtick code span",
        ),
        (
            "`code <!-- visible\nacross lines -->`",
            "`code <!-- visible\nacross lines -->`",
            "comment delimiters inside a multiline code span",
        ),
    )
    for source, expected, label in cases:
        actual = strip_html_comments(source)
        if actual != expected:
            print(f"HTML-comment parser mishandled {label}", file=sys.stderr)
            sys.exit(1)

    early_close = strip_html_comments(
        "<!-- hidden `-->` Polaris-extra-x86_64.AppImage -->"
    )
    if "Polaris-extra-x86_64.AppImage" not in early_close:
        print("HTML-comment parser ignored the first real comment close", file=sys.stderr)
        sys.exit(1)

    escaped_ticks = strip_html_comments(
        r"\`<!--\` Polaris-extra-x86_64.AppImage \`-->\`"
    )
    if "Polaris-extra-x86_64.AppImage" in escaped_ticks:
        print("Escaped backticks incorrectly opened an inline code span", file=sys.stderr)
        sys.exit(1)


verify_html_comment_parser()


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
    protected = protect_inline_code_markup(text)
    text = restore_inline_code_markup(mask_hidden_html(protected))
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


HTML_VOID_TAGS = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr",
}
HTML_NONCONTENT_TAGS = {"script", "style", "template"}


def html_element_is_hidden(tag, attrs):
    values = {name.lower(): (value or "") for name, value in attrs}
    if tag in HTML_NONCONTENT_TAGS or "hidden" in values:
        return True
    if values.get("aria-hidden", "").strip().lower() == "true":
        return True
    style = values.get("style", "")
    return bool(
        re.search(
            r"(?:^|;)\s*display\s*:\s*none(?:\s*!important)?\s*(?:;|$)",
            style,
            re.I,
        )
    )


class HiddenHTMLRangeParser(HTMLParser):
    """Locate non-rendered HTML ranges so Markdown headings cannot hide in them."""

    def __init__(self, source):
        super().__init__(convert_charrefs=False)
        self.source = source
        self.line_offsets = [0]
        for match in re.finditer(r"\n", source):
            self.line_offsets.append(match.end())
        self.stack = []
        self.hidden_depth = 0
        self.hidden_start = None
        self.ranges = []

    def _offset(self):
        line, column = self.getpos()
        return self.line_offsets[line - 1] + column

    def _tag_end(self, start):
        end = self.source.find(">", start)
        return len(self.source) if end < 0 else end + 1

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        hidden = html_element_is_hidden(tag, attrs)
        start = self._offset()
        if tag in HTML_VOID_TAGS:
            if hidden and self.hidden_depth == 0:
                self.ranges.append((start, self._tag_end(start)))
            return
        if hidden and self.hidden_depth == 0:
            self.hidden_start = start
        self.stack.append((tag, hidden))
        if hidden:
            self.hidden_depth += 1

    def handle_startendtag(self, tag, attrs):
        if html_element_is_hidden(tag.lower(), attrs) and self.hidden_depth == 0:
            start = self._offset()
            self.ranges.append((start, self._tag_end(start)))

    def handle_endtag(self, tag):
        tag = tag.lower()
        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index][0] != tag:
                continue
            removed = self.stack[index:]
            del self.stack[index:]
            prior_depth = self.hidden_depth
            self.hidden_depth -= sum(1 for _, hidden in removed if hidden)
            if prior_depth > 0 and self.hidden_depth == 0 and self.hidden_start is not None:
                self.ranges.append((self.hidden_start, self._tag_end(self._offset())))
                self.hidden_start = None
            return

    def close(self):
        super().close()
        if self.hidden_start is not None:
            self.ranges.append((self.hidden_start, len(self.source)))
            self.hidden_start = None


def mask_hidden_html(text):
    parser = HiddenHTMLRangeParser(text)
    parser.feed(text)
    parser.close()
    masked = list(text)
    for start, end in parser.ranges:
        for index in range(start, end):
            if masked[index] not in "\r\n":
                masked[index] = " "
    return "".join(masked)


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
    closer_map = backtick_closer_map(text)
    inline_ranges = [
        (opener, closer[1])
        for opener, closer in closer_map.items()
        if closer is not None
    ]
    line_offset = 0
    inline_range_index = 0
    for line in text.splitlines(keepends=True):
        line_start = line_offset
        line_end = line_start + len(line)
        line_offset = line_end
        while (
            inline_range_index < len(inline_ranges)
            and inline_ranges[inline_range_index][1] <= line_start
        ):
            inline_range_index += 1
        overlaps_inline_code = False
        if inline_range_index < len(inline_ranges):
            span_start, span_end = inline_ranges[inline_range_index]
            overlaps_inline_code = line_start < span_end and line_end > span_start
        fence, delimiter = advance_fence(fence, line)
        if delimiter or fence is not None:
            continue
        if is_indented_code(line) and not overlaps_inline_code:
            continue
        if re.match(r"^[ ]{0,3}\[[^]]+\]:\s+", line):
            continue
        rendered.append(line)

    visible = "".join(rendered)
    visible = re.sub(
        r"!\[[^]]*\]\((?:\\.|[^)\n])*\)",
        "",
        visible,
    )
    visible = re.sub(
        r"\[([^]]*)\]\((?:\\.|[^)\n])*\)",
        lambda match: match.group(1),
        visible,
    )
    visible = re.sub(r"!\[[^]]*\]\[[^]]*\]", "", visible)
    visible = re.sub(
        r"\[([^]]*)\]\[[^]]*\]",
        lambda match: match.group(1),
        visible,
    )
    protected = protect_inline_code_markup(visible)
    return restore_inline_code_markup(visible_html_text(protected))


def verify_rendered_markdown_parser() -> None:
    inline = "`<!--` Polaris-extra-x86_64.AppImage `-->`"
    rendered_inline = rendered_markdown(inline)
    if "Polaris-extra-x86_64.AppImage" not in rendered_inline:
        print("Rendered Markdown dropped visible inline-code content", file=sys.stderr)
        sys.exit(1)

    for markup in (
        "`<div hidden>Polaris-extra-x86_64.AppImage</div>`",
        "``<script>Polaris-extra-x86_64.AppImage</script>``",
        "```<span hidden>Polaris-extra-x86_64.AppImage</span>```",
        "`<div hidden>` Polaris-extra-x86_64.AppImage `</div>`",
    ):
        if "Polaris-extra-x86_64.AppImage" not in rendered_markdown(markup):
            print("Rendered Markdown interpreted literal inline-code HTML", file=sys.stderr)
            sys.exit(1)

    rendered_comment = rendered_markdown("visible <!-- hidden --> prose")
    if "hidden" in rendered_comment or "visible" not in rendered_comment:
        print("Rendered Markdown exposed an actual HTML comment", file=sys.stderr)
        sys.exit(1)

    early_close = rendered_markdown(
        "<!-- hidden `-->` Polaris-extra-x86_64.AppImage -->"
    )
    if "Polaris-extra-x86_64.AppImage" not in early_close:
        print("Rendered Markdown ignored the first real comment close", file=sys.stderr)
        sys.exit(1)

    escaped_ticks = rendered_markdown(
        r"\`<!--\` Polaris-extra-x86_64.AppImage \`-->\`"
    )
    if "Polaris-extra-x86_64.AppImage" in escaped_ticks:
        print("Rendered Markdown treated escaped backticks as code", file=sys.stderr)
        sys.exit(1)

    escaped_split_run = rendered_markdown(
        r"\``<span hidden>Polaris-extra-x86_64.AppImage</span>`"
    )
    if "Polaris-extra-x86_64.AppImage" not in escaped_split_run:
        print("Escaping one backtick incorrectly escaped the entire run", file=sys.stderr)
        sys.exit(1)

    escaped_closer = rendered_markdown(
        r"`<span hidden>Polaris-extra-x86_64.AppImage</span>\`"
    )
    if "Polaris-extra-x86_64.AppImage" not in escaped_closer:
        print("Backslash inside code incorrectly escaped its closing delimiter", file=sys.stderr)
        sys.exit(1)

    for list_code_span in (
        "- `\n  <span hidden>Polaris-extra-x86_64.AppImage</span>\n  `",
        "1. `\n   <span hidden>Polaris-extra-x86_64.AppImage</span>\n   `",
        "10. `\n    <span hidden>Polaris-extra-x86_64.AppImage</span>\n    `",
    ):
        if "Polaris-extra-x86_64.AppImage" not in rendered_markdown(list_code_span):
            print("Inline-code span was not preserved inside a list paragraph", file=sys.stderr)
            sys.exit(1)

    lazy_quote_code = "> `\n<span hidden>Polaris-extra-x86_64.AppImage</span>\n> `"
    if "Polaris-extra-x86_64.AppImage" not in rendered_markdown(lazy_quote_code):
        print("Inline-code span did not preserve a lazy blockquote continuation", file=sys.stderr)
        sys.exit(1)

    separate_list_items = "- `\n- <!-- Polaris-extra-x86_64.AppImage -->\n- `"
    if "Polaris-extra-x86_64.AppImage" in rendered_markdown(separate_list_items):
        print("Inline-code span crossed a list-item boundary", file=sys.stderr)
        sys.exit(1)

    for cross_block in (
        "`\n\n<!-- Polaris-extra-x86_64.AppImage -->\n\n`",
        "`\n\n<span hidden>Polaris-extra-x86_64.AppImage</span>\n\n`",
        "> `\n>\n> <!-- Polaris-extra-x86_64.AppImage -->\n>\n> `",
        "`\n<!-- Polaris-extra-x86_64.AppImage -->\n`",
        "`\n<script>Polaris-extra-x86_64.AppImage</script>\n`",
        "`\n<div hidden>Polaris-extra-x86_64.AppImage</div>\n`",
        "`\n<center data-probe=\"Polaris-extra-x86_64.AppImage\">prose</center>\n`",
        "`\n<hr data-probe=\"Polaris-extra-x86_64.AppImage\">\n`",
        "`\n<textarea data-probe=\"Polaris-extra-x86_64.AppImage\">prose</textarea>\n`",
        "`\n> <!-- Polaris-extra-x86_64.AppImage -->\n`",
    ):
        if "Polaris-extra-x86_64.AppImage" in rendered_markdown(cross_block):
            print("Inline-code span crossed a Markdown paragraph boundary", file=sys.stderr)
            sys.exit(1)


verify_rendered_markdown_parser()


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
        if is_indented_code(line):
            if current:
                blocks.append(current)
                current = []
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
    lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|<>()")
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
if any(
    (token and set(token) <= set(";&|<>()")) or "$" in token or "`" in token
    for token in arch_command
):
    print(
        "Arch/CachyOS pacman command must use literal arguments without shell control or expansion",
        file=sys.stderr,
    )
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
    "## v1.3.2 - 2026-07-30",
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
