#!/usr/bin/env python3
"""Validate explicit release-package build and runtime dependencies."""

from pathlib import Path
import re
import shlex

ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def shell_array(text: str, name: str) -> str:
    match = re.search(rf"(?ms)^{re.escape(name)}=\(\n(?P<body>.*?)^\)", text)
    if not match:
        raise AssertionError(f"missing {name} array")
    return match.group("body")


def shell_tokens(text: str) -> list[str]:
    tokens: list[str] = []
    for line in text.replace("\\\n", " ").splitlines():
        lexer = shlex.shlex(line, posix=True, punctuation_chars=";&|")
        lexer.whitespace_split = True
        lexer.commenters = "#"
        line_tokens = list(lexer)
        if not line_tokens:
            continue
        if tokens and tokens[-1] != ";":
            tokens.append(";")
        tokens.extend(line_tokens)
    return tokens


def shell_array_tokens(text: str, name: str) -> list[str]:
    return [token for token in shell_tokens(shell_array(text, name)) if token != ";"]


def require_package(section: str, package: str, context: str) -> None:
    if not re.search(rf"(?m)^\s*'{re.escape(package)}'\s*$", section):
        raise AssertionError(f"{context} must explicitly include {package}")


def require_single_cmake_bool(tokens: list[str], option: str, expected: str, context: str) -> None:
    definition = re.compile(rf"-D{re.escape(option)}(?::BOOL)?=(ON|OFF)")
    definitions = [match.group(1) for token in tokens if (match := definition.fullmatch(token))]
    mentions = [token for token in tokens if option in token]
    if definitions != [expected] or len(mentions) != 1:
        raise AssertionError(
            f"{context} must set exactly one literal -D{option}={expected} and contain no override"
        )


def self_test_cmake_bool_contract() -> None:
    option = "POLARIS_ENABLE_PIPEWIRE"
    require_single_cmake_bool(
        shell_tokens(f"-D{option}=OFF\n"),
        option,
        "OFF",
        "valid self-test",
    )
    rejected = {
        "comment-only": f"# -D{option}=OFF\n",
        "missing": "-DUNRELATED=ON\n",
        "duplicate": f"-D{option}=OFF -D{option}=OFF\n",
        "opposite": f"-D{option}=ON\n",
        "both": f"-D{option}=OFF -D{option}=ON\n",
        "nonliteral": f"-D{option}=${{PIPEWIRE_SETTING}}\n",
    }
    for label, script in rejected.items():
        try:
            require_single_cmake_bool(
                shell_tokens(script),
                option,
                "OFF",
                f"{label} self-test",
            )
        except AssertionError:
            continue
        raise AssertionError(f"CMake option contract must reject the {label} mutation")


def workflow_job(text: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\n(?P<body>.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
        text,
    )
    if not match:
        raise AssertionError(f"missing {name} workflow job")
    return match.group("body")


def workflow_step(job: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^      - name: {re.escape(name)}\n(?P<body>.*?)(?=^      - name:|\Z)",
        job,
    )
    if not match:
        raise AssertionError(f"missing workflow step: {name}")
    return match.group("body")


def workflow_run_script(step: str) -> str:
    run = re.search(
        r"(?ms)^        run: \|\n(?P<script>(?:^          .*(?:\n|\Z))*)",
        step,
    )
    if not run:
        raise AssertionError("missing shell run block in workflow step")
    return "\n".join(line[10:] for line in run.group("script").splitlines())


def workflow_run_tokens(step: str) -> list[str]:
    return shell_tokens(workflow_run_script(step))


def command_arguments(tokens: list[str], executable: str) -> list[list[str]]:
    boundaries = {";", "&&", "||", "do", "then"}
    commands: list[list[str]] = []
    for index, token in enumerate(tokens):
        if token != executable or (index > 0 and tokens[index - 1] not in boundaries):
            continue
        end = index + 1
        while end < len(tokens) and tokens[end] not in boundaries:
            end += 1
        commands.append(tokens[index + 1:end])
    return commands


def require_consumed_cmake_bool(
    executable_tokens: list[str],
    consumed_arguments: list[str],
    option: str,
    expected: str,
    context: str,
) -> None:
    require_single_cmake_bool(consumed_arguments, option, expected, context)
    executable_mentions = [token for token in executable_tokens if option in token]
    if len(executable_mentions) != 1:
        raise AssertionError(f"{context} contains an executable override outside the consumed CMake arguments")


def contains_command(tokens: list[str], expected: list[str]) -> bool:
    width = len(expected)
    boundaries = {";", "&&", "||", "do", "then"}
    for index in range(len(tokens) - width + 1):
        if tokens[index:index + width] != expected:
            continue
        if index == 0 or tokens[index - 1] in boundaries:
            return True
    return False


def require_command(
    tokens: list[str], expected: list[str], context: str
) -> None:
    if not contains_command(tokens, expected):
        raise AssertionError(f"{context} must contain executable tokens: {expected}")


def command_count(tokens: list[str], expected: list[str]) -> int:
    width = len(expected)
    boundaries = {";", "&&", "||", "do", "then"}
    return sum(
        tokens[index:index + width] == expected
        and (index == 0 or tokens[index - 1] in boundaries)
        for index in range(len(tokens) - width + 1)
    )


def executable_array(tokens: list[str], name: str) -> list[str]:
    marker = f"{name}=("
    starts = [index for index, token in enumerate(tokens) if token == marker]
    if len(starts) != 1:
        raise AssertionError(f"expected one executable {name} array")
    end = tokens.index(")", starts[0] + 1)
    return [token for token in tokens[starts[0] + 1:end] if token != ";"]


def reject_heredoc(tokens: list[str], context: str) -> None:
    if any(token.startswith("<<") for token in tokens):
        raise AssertionError(f"{context} must not use heredoc payloads as contract evidence")


def require_fail_closed_step(step: str, context: str) -> None:
    """Refuse the ways a step can keep its commands and still never fail."""
    if re.search(r"(?m)^        continue-on-error\s*:", step):
        raise AssertionError(f"{context} must not continue on error")
    tokens = workflow_run_tokens(step)
    if "||" in tokens:
        raise AssertionError(f"{context} must not use || to swallow a failure")
    for swallow in (["true"], [":"], ["exit", "0"]):
        if command_count(tokens, swallow):
            raise AssertionError(f"{context} must not run {' '.join(swallow)} to end in success")
    if command_arguments(tokens, "set") != [["-euo", "pipefail"]]:
        raise AssertionError(f"{context} must set -euo pipefail, once, and no other shell option")


def self_test_fail_closed_step() -> None:
    clean = (
        "        if: steps.source.outputs.prerelease == 'false'\n"
        "        run: |\n"
        "          set -euo pipefail\n"
        "          python3 scripts/check-stable-release-notes.py notes.md\n"
    )
    require_fail_closed_step(clean, "valid fail-closed step self-test")
    for label, step in {
        "or true": clean.replace("notes.md\n", "notes.md || true\n"),
        "or colon": clean.replace("notes.md\n", "notes.md || :\n"),
        "or echo": clean.replace("notes.md\n", "notes.md || echo ignored\n"),
        "a trailing true": clean + "          true\n",
        "an early exit 0": clean.replace("          python3", "          exit 0\n          python3"),
        "set +e": clean.replace("pipefail\n", "pipefail\n          set +e\n"),
        "no set -e": clean.replace("          set -euo pipefail\n", ""),
        "continue-on-error": clean.replace(
            "        run: |\n", "        continue-on-error: true\n        run: |\n"
        ),
    }.items():
        try:
            require_fail_closed_step(step, "fail-closed step self-test")
        except AssertionError:
            continue
        raise AssertionError(f"fail-closed step contract must reject {label}")


def self_test_executable_release_contract() -> None:
    expected = ["gh", "release", "edit", "v1.2.3", "--draft=true"]
    require_command(
        shell_tokens('gh release edit "v1.2.3" --draft=true\n'),
        expected,
        "valid release command self-test",
    )
    for label, script in {
        "commented": '# gh release edit "v1.2.3" --draft=true\n',
        "echoed": 'echo gh release edit "v1.2.3" --draft=true\n',
    }.items():
        if contains_command(shell_tokens(script), expected):
            raise AssertionError(
                f"release command contract must reject {label} inert text"
            )


self_test_cmake_bool_contract()
self_test_executable_release_contract()
self_test_fail_closed_step()

arch = read("packaging/linux/Arch/PKGBUILD")
require_package(shell_array(arch, "depends"), "vulkan-icd-loader", "Arch runtime dependencies")
for boost_library in (
    "libboost_filesystem.so",
    "libboost_locale.so",
    "libboost_log.so",
    "libboost_program_options.so",
    "libboost_thread.so",
):
    require_package(
        shell_array(arch, "depends"),
        boost_library,
        "Arch versioned Boost runtime dependencies",
    )
for package in ("shaderc", "vulkan-headers"):
    require_package(shell_array(arch, "makedepends"), package, "Arch build dependencies")
# libei is found at configure time and left out without a word when it is
# missing, and gamescope_stream then has no mouse or keyboard. Every recipe
# names it, and every package job asks the binary it built.
require_package(shell_array(arch, "depends"), "libei", "Arch runtime dependencies")
steamos = read("packaging/linux/SteamOS/PKGBUILD")
require_package(shell_array(steamos, "depends"), "libei", "SteamOS runtime dependencies")

fedora = read("packaging/linux/fedora/Polaris.spec")
for package in ("glslc", "libei-devel", "pipewire-devel", "vulkan-loader-devel"):
    if not re.search(rf"(?m)^BuildRequires:\s+{re.escape(package)}\s*$", fedora):
        raise AssertionError(f"Fedora build dependencies must explicitly include {package}")
fedora_build_match = re.search(r"(?ms)^%build\n(?P<body>.*?)(?=^%check\n)", fedora)
if not fedora_build_match:
    raise AssertionError("missing Fedora RPM spec build section")
fedora_build = fedora_build_match.group("body")
fedora_spec_tokens = shell_tokens(fedora_build)
fedora_spec_cmake_args = shell_array_tokens(fedora_build, "cmake_args")
fedora_spec_cmake_commands = command_arguments(fedora_spec_tokens, "cmake")
if fedora_spec_cmake_commands.count(["${cmake_args[@]}"]) != 1:
    raise AssertionError("Fedora RPM spec must consume cmake_args exactly once in its configure command")
for option, expected in (
    ("POLARIS_ENABLE_PIPEWIRE", "OFF"),
    ("POLARIS_ENABLE_PORTAL", "ON"),
):
    require_consumed_cmake_bool(
        fedora_spec_tokens,
        fedora_spec_cmake_args,
        option,
        expected,
        "Fedora RPM spec",
    )

workflow = read(".github/workflows/build.yml")
withdrawn_sysext_builder = ROOT / "scripts/ci/build-sysext-image.sh"
if withdrawn_sysext_builder.exists():
    raise AssertionError(
        "withdrawn system-extension image builder must not remain in the repository"
    )
for withdrawn_marker in (
    "  sysext-build:\n",
    "      - sysext-build\n",
    "name: Polaris-sysext-image",
    "Polaris-sysext-x86_64.raw",
    "release-assets/raw/sysext",
    "Download systemd extension image",
):
    if withdrawn_marker in workflow:
        raise AssertionError(
            f"release workflow must not build, upload, or publish the withdrawn system extension: {withdrawn_marker.strip()}"
        )

resolve_job = workflow_job(workflow, "resolve-source")
resolve_script = workflow_run_script(
    workflow_step(resolve_job, "Bind release tag to source commit")
)
resolve_tokens = shell_tokens(resolve_script)
reject_heredoc(resolve_tokens, "exact-source resolver")
if resolve_job.count("      version: ${{ steps.source.outputs.version }}\n") != 1:
    raise AssertionError("exact-source resolver must publish one source-derived version output")
if resolve_job.count("      prerelease_label: ${{ steps.source.outputs.prerelease_label }}\n") != 1:
    raise AssertionError("exact-source resolver must publish the tag's prerelease label beside the version")
for required_source_command in (
    ["set", "-euo", "pipefail"],
    ["source_commit=$(git rev-parse HEAD)"],
    ["build_version=$(grep -Pom1 '^project\\(Polaris VERSION \\K[^ ]+' CMakeLists.txt)"],
    ["git", "fetch", "--no-tags", "--force", "origin", "refs/tags/${release_tag}:refs/tags/${release_tag}"],
    ["tag_commit=$(git rev-parse refs/tags/${release_tag}^{commit})"],
    # A beta or rc tag is accepted, and only the numeric part of it has to equal the built version.
    # Its label travels to the package builds, which spell it so a prerelease sorts below its release.
    [
        "if", "[[", "!", "$release_tag", "=~", "^v[0-9]+.[0-9]+.[0-9]+(-(beta", "|", "rc).[0-9]+)?$", "]]", ";", "then", ";",
        "echo", "Release tag must match vMAJOR.MINOR.PATCH, optionally -beta.N or -rc.N: $release_tag", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    ["release_version=${release_tag#v}"],
    ["release_version=${release_version%%-*}"],
    [
        "if", "[", "$release_version", "!=", "$build_version", "]", ";", "then", ";",
        "echo", "Release tag $release_tag does not match source version v${build_version}", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    # Anything carrying a channel suffix is a prerelease, decided once here and carried downstream,
    # so the staging and publishing steps cannot disagree about what they are publishing.
    ["prerelease=false"],
    ["prerelease_label="],
    [
        "if", "[", "$release_tag", "!=", "v${build_version}", "]", ";", "then", ";",
        "prerelease=true", ";", "prerelease_label=${release_tag#v${build_version}-}", ";", "fi",
    ],
    [
        "if", "[", "$tag_commit", "!=", "$source_commit", "]", ";", "then", ";",
        "echo", "Release tag $release_tag resolves to $tag_commit, not checked-out source $source_commit", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    ["echo", "commit=$source_commit", ">>", "$GITHUB_OUTPUT"],
    ["echo", "version=$build_version", ">>", "$GITHUB_OUTPUT"],
    ["echo", "prerelease=$prerelease", ">>", "$GITHUB_OUTPUT"],
    ["echo", "prerelease_label=$prerelease_label", ">>", "$GITHUB_OUTPUT"],
):
    require_command(resolve_tokens, required_source_command, "exact-source resolver")

# A beta publishes the notes of the release it precedes, and the stable page is published from the
# same file as it stands, so a stable tag is where anything those notes told beta testers has to be
# refused. The resolver is where that costs nothing: every packaging job waits on it.
stable_notes_tests = workflow_step(resolve_job, "Verify stable release notes gate")
if not stable_notes_tests.startswith("        run: |\n"):
    raise AssertionError("exact-source resolver must test the stable release notes gate on every build")
stable_notes_test_tokens = workflow_run_tokens(stable_notes_tests)
reject_heredoc(stable_notes_test_tokens, "stable release notes gate tests")
for required_test_command in (
    ["set", "-euo", "pipefail"],
    # A rebuild of a tag cut before the gate is refused by name. unittest alone would refuse it too,
    # by finding no tests, which reads as a broken runner rather than as a decision.
    [
        "if", "[", "!", "-f", "scripts/check-stable-release-notes.py", "]", ";", "then", ";",
        "echo",
        (
            "This checkout predates scripts/check-stable-release-notes.py, so nothing has checked the "
            "release notes a rebuild would publish over its page. For v1.4.13 they are the text its "
            "hand-posted page replaced. Rebuilding a tag this old is refused."
        ),
        ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    [
        "python3", "-m", "unittest", "discover", "-s", "tests/scripts",
        "-p", "test_check_stable_release_notes.py",
    ],
):
    require_command(stable_notes_test_tokens, required_test_command, "stable release notes gate tests")
# Keeping the commands proves nothing if the step cannot fail: `|| true` after the gate, or
# continue-on-error on its step, leaves every command above in place and publishes the notes anyway.
require_fail_closed_step(stable_notes_tests, "stable release notes gate tests")
stable_notes_gate = workflow_step(resolve_job, "Refuse beta-only text in stable release notes")
if not stable_notes_gate.startswith(
    "        if: (startsWith(github.ref, 'refs/tags/v') || inputs.release_tag != '') "
    "&& steps.source.outputs.prerelease == 'false'\n"
):
    raise AssertionError(
        "the stable release notes gate must run for every stable release tag and for no prerelease"
    )
stable_notes_tokens = workflow_run_tokens(stable_notes_gate)
reject_heredoc(stable_notes_tokens, "stable release notes gate")
for required_gate_command in (
    ["set", "-euo", "pipefail"],
    [
        "python3", "scripts/check-stable-release-notes.py",
        "docs/release-notes/${POLARIS_PACKAGE_REF_NAME}.md",
    ],
):
    require_command(stable_notes_tokens, required_gate_command, "stable release notes gate")
require_fail_closed_step(stable_notes_gate, "stable release notes gate")
if resolve_job.index("- name: Refuse beta-only text in stable release notes") < resolve_job.index(
    "- name: Bind release tag to source commit"
):
    raise AssertionError("the stable release notes gate must follow the step that decides the channel")

exact_checkout_ref = "ref: ${{ needs.resolve-source.outputs.commit }}"
for job_name in (
    "web-checks",
    "cpp-sanitizer-tests",
    "arch-build",
    "fedora-clang-build",
    "steamos-build",
    "ubuntu-build",
    "fedora-rpm-build",
    "release-assets",
):
    job = workflow_job(workflow, job_name)
    if job.count("uses: actions/checkout@v7") != 1:
        raise AssertionError(f"{job_name} must contain exactly one checkout")
    if job.count(exact_checkout_ref) != 1:
        raise AssertionError(
            f"{job_name} must check out the immutable resolve-source output"
        )
    if "resolve-source" not in job.split("    steps:", 1)[0]:
        raise AssertionError(f"{job_name} must directly depend on resolve-source")

if len(re.findall(r"(?m)^  fedora-rpm-build:\s*$", workflow)) != 1:
    raise AssertionError("release workflow must define exactly one Fedora RPM job")
fedora_clang_job = workflow_job(workflow, "fedora-clang-build")
fedora_job = workflow_job(workflow, "fedora-rpm-build")
ubuntu_job = workflow_job(workflow, "ubuntu-build")

expected_package_version_env = (
    "      BRANCH: ${{ github.head_ref || inputs.release_tag || github.ref_name }}\n"
    "      BUILD_VERSION: ${{ needs.resolve-source.outputs.version }}\n"
    "      POLARIS_PRERELEASE_LABEL: ${{ needs.resolve-source.outputs.prerelease_label }}\n"
)
for job_name, job in (("Ubuntu DEB", ubuntu_job), ("Fedora RPM", fedora_job)):
    if job.count(expected_package_version_env) != 1:
        raise AssertionError(
            f"{job_name} CI must bind BRANCH, BUILD_VERSION and the prerelease label to the exact resolved source"
        )

# X-Resource names the host pid behind each window of a private app, which a sandboxed game's own
# _NET_WM_PID does not, and the build leaves it out without a word when its headers are missing.
# Debian's Depends is written by hand, so the library the binary links is named there, and the
# Ubuntu package job installs its headers. Fedora, Arch and SteamOS ship it inside libxcb.
linux_packaging = read("cmake/packaging/linux.cmake")
debian_depends = re.search(r'(?ms)^set\(CPACK_DEBIAN_PACKAGE_DEPENDS "(?P<body>.*?)"\)', linux_packaging)
if not debian_depends or not re.search(r"(?m)^\s*libxcb-res0,\s*\\$", debian_depends.group("body")):
    raise AssertionError("Debian runtime dependencies must explicitly include libxcb-res0")
if not re.search(r"(?m)^\s+.*\blibxcb-res0-dev\b", ubuntu_job):
    raise AssertionError("the Ubuntu DEB job must install libxcb-res0-dev so its package links X-Resource")
for recipe_path in ("packaging/linux/Arch/PKGBUILD", "packaging/linux/SteamOS/PKGBUILD"):
    require_package(shell_array(read(recipe_path), "depends"), "libxcb", f"{recipe_path} runtime dependencies")
if not re.search(r"(?m)^BuildRequires:\s+libxcb-devel\s*$", fedora):
    raise AssertionError("Fedora build dependencies must explicitly include libxcb-devel")

fedora_clang_configure = workflow_step(fedora_clang_job, "Configure")
fedora_clang_tokens = workflow_run_tokens(fedora_clang_configure)
fedora_clang_commands = command_arguments(fedora_clang_tokens, "cmake")
fedora_clang_configure_commands = [
    arguments for arguments in fedora_clang_commands if arguments[:2] == ["-B", "build"]
]
if len(fedora_clang_configure_commands) != 1:
    raise AssertionError("Fedora Clang CI must contain exactly one directly parsed cmake configure command")
for option, expected in (
    ("POLARIS_ENABLE_PIPEWIRE", "OFF"),
    ("POLARIS_ENABLE_PORTAL", "ON"),
):
    require_consumed_cmake_bool(
        fedora_clang_tokens,
        fedora_clang_configure_commands[0],
        option,
        expected,
        "Fedora Clang CI",
    )

fedora_rpm_configure = workflow_step(fedora_job, "Configure")
fedora_rpm_script = workflow_run_script(fedora_rpm_configure)
fedora_rpm_tokens = shell_tokens(fedora_rpm_script)
fedora_rpm_cmake_args = shell_array_tokens(fedora_rpm_script, "cmake_args")
fedora_rpm_cmake_commands = command_arguments(fedora_rpm_tokens, "cmake")
if fedora_rpm_cmake_commands.count(["${cmake_args[@]}"]) != 1:
    raise AssertionError("Fedora RPM CI must consume cmake_args exactly once in its configure command")
for option, expected in (
    ("POLARIS_ENABLE_PIPEWIRE", "OFF"),
    ("POLARIS_ENABLE_PORTAL", "ON"),
):
    require_consumed_cmake_bool(
        fedora_rpm_tokens,
        fedora_rpm_cmake_args,
        option,
        expected,
        "Fedora RPM CI",
    )

package_identity_contracts = (
    (
        "Ubuntu DEB",
        ubuntu_job,
        "Smoke test Ubuntu DEB package",
        (
            'package_name="$(dpkg-deb --field "$deb_path" Package)"',
            'package_version="$(dpkg-deb --field "$deb_path" Version)"',
            'package_architecture="$(dpkg-deb --field "$deb_path" Architecture)"',
            'package_identity="${package_name}|${package_version}|${package_architecture}"',
            # A prerelease is X.Y.Z~beta.N, the spelling dpkg sorts below X.Y.Z, and it has to.
            'expected_package_version="${BUILD_VERSION}"',
            'if [ -n "$POLARIS_PRERELEASE_LABEL" ]; then',
            'expected_package_version="${BUILD_VERSION}~${POLARIS_PRERELEASE_LABEL}"',
            'expected_package_identity="polaris|${expected_package_version}|amd64"',
            "printf '%s\\n' \"$package_identity\" | tee build/cpack_artifacts/package-identity.txt",
            'if [ "$package_identity" != "$expected_package_identity" ]; then',
            'echo "Ubuntu package identity mismatch: expected \'$expected_package_identity\', got \'$package_identity\'" >&2',
            'if [ -n "$POLARIS_PRERELEASE_LABEL" ] && ! dpkg --compare-versions "$package_version" lt "$BUILD_VERSION"; then',
            'test "$kms_identity" = "polaris-kms|${expected_package_version}|amd64"',
            'test "$(dpkg-deb --field "$kms_deb_path" Depends)" = "polaris (= ${expected_package_version}), libcap2-bin, passwd"',
        ),
    ),
    (
        "Fedora RPM",
        fedora_job,
        "Smoke test Fedora RPM package",
        (
            'package_identity="$(rpm -qp --qf \'%{NAME}|%{VERSION}|%{RELEASE}|%{ARCH}\\n\' "$rpm_path")"',
            # A prerelease is X.Y.Z~beta.N, the spelling rpm sorts below X.Y.Z, and it has to.
            'expected_package_version="${BUILD_VERSION}"',
            'if [ -n "$POLARIS_PRERELEASE_LABEL" ]; then',
            'expected_package_version="${BUILD_VERSION}~${POLARIS_PRERELEASE_LABEL}"',
            'expected_package_identity="polaris|${expected_package_version}|1|x86_64"',
            "printf '%s\\n' \"$package_identity\" | tee build/cpack_artifacts/package-identity.txt",
            'if [ "$package_identity" != "$expected_package_identity" ]; then',
            'echo "Fedora package identity mismatch: expected \'$expected_package_identity\', got \'$package_identity\'" >&2',
            'package_evr="$(rpm -qp --qf \'%{VERSION}-%{RELEASE}\' "$rpm_path")"',
            'if [ -n "$POLARIS_PRERELEASE_LABEL" ] && [ "$(rpm --eval "%{lua: print(rpm.vercmp(\'${package_evr}\', \'${BUILD_VERSION}-1\'))}")" != "-1" ]; then',
            'test "$kms_identity" = "polaris-kms|${expected_package_version}|1|x86_64"',
            'rpm -qpR "$kms_rpm_path" | grep -Fx "polaris = ${expected_package_version}"',
        ),
    ),
)
for job_name, job, step_name, required_lines in package_identity_contracts:
    smoke_script = workflow_run_script(workflow_step(job, step_name))
    for required_line in required_lines:
        if required_line not in smoke_script:
            raise AssertionError(
                f"{job_name} smoke must execute exact package identity check: {required_line}"
            )
fedora_strategy = re.search(
    r"(?ms)^    strategy:\n.*?(?=^    env:\n)",
    fedora_job,
)
expected_fedora_strategy = (
    "    strategy:\n"
    "      fail-fast: false\n"
    "      matrix:\n"
    "        include:\n"
    "          - fedora: '44'\n"
    "            cc: gcc-15\n"
    "            cxx: g++-15\n"
    "            boost_static: OFF\n"
)
if not fedora_strategy or fedora_strategy.group(0) != expected_fedora_strategy:
    raise AssertionError("Fedora CI strategy must contain only the exact reviewed Fedora 44 lane")
for legacy_version in ("42", "43"):
    for legacy_marker in (
        f"fedora-{legacy_version}-rpm-artifacts",
        f"release-assets/raw/fedora{legacy_version}",
        f"copy_fedora_rpms {legacy_version}",
    ):
        if legacy_marker in workflow:
            raise AssertionError(f"release workflow retains Fedora {legacy_version}: {legacy_marker}")
release_job = workflow_job(workflow, "release-assets")
release_checkout = workflow_step(release_job, "Check out exact release source")
expected_release_checkout = (
    "        uses: actions/checkout@v7\n"
    "        with:\n"
    "          ref: ${{ needs.resolve-source.outputs.commit }}\n"
)
if release_checkout.strip() != expected_release_checkout.strip():
    raise AssertionError(
        "release-assets must check out the exact packaged ref before reading curated notes"
    )
release_step_names = (
    "Check out exact release source",
    "Prepare release asset names",
    "Verify release package versions match the tag",
    "Revalidate release tag against packaged source",
    "Stage curated GitHub release",
    "Upload release assets to GitHub release",
    "Verify release assets on GitHub release",
    "Publish verified draft release",
)
release_step_positions = [release_job.index(f"- name: {name}") for name in release_step_names]
if release_step_positions != sorted(release_step_positions):
    raise AssertionError(
        "release checkout, tag validation, note staging, asset upload, verification, and publication must remain ordered"
    )

release_tag_validation_tokens = workflow_run_tokens(
    workflow_step(release_job, "Revalidate release tag against packaged source")
)
reject_heredoc(release_tag_validation_tokens, "release tag revalidation")
for required_tag_command in (
    ["set", "-euo", "pipefail"],
    ["checked_out_commit=$(git rev-parse HEAD)"],
    ["git", "fetch", "--no-tags", "--force", "origin", "refs/tags/${POLARIS_PACKAGE_REF_NAME}:refs/tags/${POLARIS_PACKAGE_REF_NAME}"],
    ["tag_commit=$(git rev-parse refs/tags/${POLARIS_PACKAGE_REF_NAME}^{commit})"],
    [
        "if", "[", "$checked_out_commit", "!=", "$EXPECTED_SOURCE_COMMIT", "]", ";", "then", ";",
        "echo", "Release checkout $checked_out_commit does not match packaged source $EXPECTED_SOURCE_COMMIT", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    [
        "if", "[", "$tag_commit", "!=", "$EXPECTED_SOURCE_COMMIT", "]", ";", "then", ";",
        "echo", "Release tag ${POLARIS_PACKAGE_REF_NAME} moved to $tag_commit; expected $EXPECTED_SOURCE_COMMIT", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
):
    require_command(
        release_tag_validation_tokens,
        required_tag_command,
        "release tag revalidation",
    )

release_stage = workflow_step(release_job, "Stage curated GitHub release")
release_stage_script = workflow_run_script(release_stage)
release_stage_tokens = workflow_run_tokens(release_stage)
reject_heredoc(release_stage_tokens, "curated release staging")
if len(re.findall(r"\bgh\s+release\s+create\b", release_stage_script)) != 1 or len(
    re.findall(r"\bgh\s+release\s+edit\b", release_stage_script)
) != 1:
    raise AssertionError("release staging must contain only one create and one edit mutation")
for forbidden_stage_mutation in ("upload", "delete-asset"):
    if re.search(
        rf"\bgh\s+release\s+{re.escape(forbidden_stage_mutation)}\b",
        release_stage_script,
    ):
        raise AssertionError("release staging must not mutate assets")
if not re.search(r"(?m)^        id: stage-release\s*$", release_stage):
    raise AssertionError("curated release staging must export draft publication state")
for required_stage_tokens in (
    # A beta reads the notes of the release it precedes, so the channel suffix is stripped first.
    ["notes_tag=${POLARIS_PACKAGE_REF_NAME%%-*}"],
    ["release_notes=docs/release-notes/${notes_tag}.md"],
    ["set", "-euo", "pipefail"],
    # The channel is carried into both release paths below. A rerun that finds the release already
    # staged takes the edit path, and a beta that lost its flag there would publish as stable.
    ["channel=()"],
    ["channel=(--prerelease)"],
    [
        "if", "[", "!", "-s", "$release_notes", "]", ";", "then", ";",
        "echo", "Missing curated release notes: $release_notes", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    [
        "gh", "release", "create", "${POLARIS_PACKAGE_REF_NAME}",
        "--draft", "--verify-tag", "${channel[@]}", "--title", "${POLARIS_PACKAGE_REF_NAME}",
        "--notes-file", "$release_notes",
    ],
    [
        "gh", "release", "edit", "${POLARIS_PACKAGE_REF_NAME}",
        "--verify-tag", "--draft=true", "${channel[@]}", "--title", "${POLARIS_PACKAGE_REF_NAME}",
        "--notes-file", "$release_notes",
    ],
    ["published_notes=$(gh release view ${POLARIS_PACKAGE_REF_NAME} --json body --jq .body)"],
    [
        "if", "[", "$published_notes", "!=", "$expected_notes", "]", ";", "then", ";",
        "echo", "Published release notes do not match $release_notes", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
):
    require_command(
        release_stage_tokens,
        required_stage_tokens,
        "curated release staging",
    )
if release_stage_tokens.count("--verify-tag") != 2:
    raise AssertionError("both release create and edit must verify that the tag exists")
stage_release_mutations = [
    arguments
    for arguments in command_arguments(release_stage_tokens, "gh")
    if arguments[:2] in (["release", "create"], ["release", "edit"])
]
expected_stage_release_mutations = [
    [
        "release", "create", "${POLARIS_PACKAGE_REF_NAME}", "--draft",
        "--verify-tag", "${channel[@]}", "--title", "${POLARIS_PACKAGE_REF_NAME}",
        "--notes-file", "$release_notes",
    ],
    [
        "release", "edit", "${POLARIS_PACKAGE_REF_NAME}", "--verify-tag",
        "--draft=true", "${channel[@]}", "--title", "${POLARIS_PACKAGE_REF_NAME}",
        "--notes-file", "$release_notes",
    ],
]
if stage_release_mutations != expected_stage_release_mutations:
    raise AssertionError("release staging must contain only the reviewed create and edit mutations")
if command_count(
    release_stage_tokens,
    ["echo", "publish_draft=true", ">>", "$GITHUB_OUTPUT"],
) != 2:
    raise AssertionError("both new and existing releases must remain draft until asset verification")
if any(token.startswith("is_draft=") for token in release_stage_tokens):
    raise AssertionError("an existing public release must not stay public during rerun mutation")
if any(
    contains_command(release_stage_tokens, command)
    for command in (["gh", "release", "upload"], ["gh", "release", "delete-asset"])
):
    raise AssertionError("release notes must be verified before any asset mutation")

release_upload = workflow_step(release_job, "Upload release assets to GitHub release")
release_upload_script = workflow_run_script(release_upload)
release_upload_tokens = workflow_run_tokens(release_upload)
reject_heredoc(release_upload_tokens, "release asset upload")
for mutation, expected_count in (("delete-asset", 1), ("upload", 1)):
    if len(
        re.findall(rf"\bgh\s+release\s+{re.escape(mutation)}\b", release_upload_script)
    ) != expected_count:
        raise AssertionError(f"release upload must contain exactly one {mutation} mutation")
for forbidden_upload_mutation in ("create", "edit"):
    if re.search(
        rf"\bgh\s+release\s+{re.escape(forbidden_upload_mutation)}\b",
        release_upload_script,
    ):
        raise AssertionError("release upload must not create or edit release metadata")
if any(token.startswith("published_notes=") for token in release_upload_tokens):
    raise AssertionError("release-note verification must not move after asset upload")
for exact_upload_tokens in (
    ["set", "-euo", "pipefail"],
    ["release_files=(release-assets/final/*)"],
    [
        "if", "[", "${#release_files[@]}", "-eq", "0", "]", ";", "then", ";",
        "echo", "No finalized release assets are available", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    ["declare", "-A", "expected_asset_names=()"],
    ["expected_asset_names[$(basename $release_file)]=1"],
    ["gh", "release", "view", "${POLARIS_PACKAGE_REF_NAME}", "--json", "assets", "--jq", ".assets[].name"],
    ["if", "[[", "-z", "${expected_asset_names[$published_asset]+present}", "]]", ";", "then"],
    ["gh", "release", "delete-asset", "${POLARIS_PACKAGE_REF_NAME}", "$published_asset", "--yes"],
    ["gh", "release", "upload", "${POLARIS_PACKAGE_REF_NAME}", "${release_files[@]}", "--clobber"],
):
    require_command(
        release_upload_tokens,
        exact_upload_tokens,
        "release upload must remove assets outside the finalized set",
    )
if command_count(release_upload_tokens, ["true"]):
    raise AssertionError("remote asset cleanup must fail closed")
upload_release_mutations = [
    arguments
    for arguments in command_arguments(release_upload_tokens, "gh")
    if arguments[:2] in (["release", "delete-asset"], ["release", "upload"])
]
if upload_release_mutations != [
    ["release", "delete-asset", "${POLARIS_PACKAGE_REF_NAME}", "$published_asset", "--yes"],
    ["release", "upload", "${POLARIS_PACKAGE_REF_NAME}", "${release_files[@]}", "--clobber"],
]:
    raise AssertionError("release upload must contain only exact cleanup and finalized upload mutations")
release_verify_body = workflow_step(release_job, "Verify release assets on GitHub release")
release_verify_tokens = workflow_run_tokens(release_verify_body)
reject_heredoc(release_verify_tokens, "release asset verification")
for exact_verify_tokens in (
    ["set", "-euo", "pipefail"],
    ["find", "release-assets/final", "-maxdepth", "1", "-type", "f", "-printf", "%f\\n", "|", "sort"],
    ["required_binary_assets=("],
    ["for", "required_asset", "in", "${required_binary_assets[@]}", ";", "do"],
    [
        "if", "[[", "!", " ${expected_assets[*]} ", "=~", " ${required_asset} ", "]]", ";", "then", ";",
        "echo", "Finalized release assets are missing required binary: $required_asset", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    ["gh", "release", "view", "${POLARIS_PACKAGE_REF_NAME}", "--json", "assets", "--jq", ".assets[].name", "|", "sort"],
    [
        "if", "[", "${#expected_assets[@]}", "-eq", "0", "]", "||",
        "[", "${expected_assets[*]}", "!=", "${published_assets[*]}", "]", ";", "then", ";",
        "echo", "Published assets do not match the exact finalized asset set on ${POLARIS_PACKAGE_REF_NAME}", ">", "&", "2", ";",
        "printf", "expected: %s\\n", "${expected_assets[*]}", ">", "&", "2", ";",
        "printf", "published: %s\\n", "${published_assets[*]}", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
):
    require_command(
        release_verify_tokens,
        exact_verify_tokens,
        "release verification must compare the exact local and remote asset sets",
    )
# Four Polaris packages and, for each, the DRM/KMS capture helper that goes with it. The helper is
# a release asset rather than a repository-only package because SteamOS and Ubuntu have no
# repository, so leaving one out takes DRM/KMS capture away from those hosts with nothing said.
#
# Then the two debug packages, for the two formats built from source here. A crash is read with
# coredumpctl debug against symbols from the same release, so publishing without them leaves the
# documented debugging path pointing at nothing, and it fails silently: the release looks complete.
expected_required_binaries = [
    "Polaris-arch-x86_64.pkg.tar.zst",
    "Polaris-fedora44-x86_64.rpm",
    "Polaris-steamos3.8-x86_64.pkg.tar.zst",
    "Polaris-ubuntu24.04-x86_64.deb",
    "Polaris-kms-arch-x86_64.pkg.tar.zst",
    "Polaris-kms-fedora44-x86_64.rpm",
    "Polaris-kms-steamos3.8-x86_64.pkg.tar.zst",
    "Polaris-kms-ubuntu24.04-x86_64.deb",
    "Polaris-debug-arch-x86_64.pkg.tar.zst",
    "Polaris-debug-steamos3.8-x86_64.pkg.tar.zst",
]
if executable_array(release_verify_tokens, "required_binary_assets") != expected_required_binaries:
    raise AssertionError(
        "release verification must require the exact %d binary assets"
        % len(expected_required_binaries))
for partial_check in ("supported_count", "legacy_count"):
    if any(partial_check in token for token in release_verify_tokens):
        raise AssertionError("release verification must not accept a partial asset subset")
release_publish = workflow_step(release_job, "Publish verified draft release")
# A beta publishes without becoming the latest release, so a stable install and every download link
# that resolves "latest" keep answering the last stable version. Both branches stay pinned here,
# because a prerelease published as latest is the one mistake this whole channel exists to prevent.
expected_release_publish = (
    "        if: steps.stage-release.outputs.publish_draft == 'true'\n"
    "        env:\n"
    "          GH_TOKEN: ${{ github.token }}\n"
    "          GH_REPO: ${{ github.repository }}\n"
    "          IS_PRERELEASE: ${{ needs.resolve-source.outputs.prerelease }}\n"
    "        run: |\n"
    "          set -euo pipefail\n"
    "          # A beta is published without becoming the latest release, so every stable install and\n"
    '          # every download link that resolves "latest" keeps answering the last stable version.\n'
    '          if [ "$IS_PRERELEASE" = "true" ]; then\n'
    '            gh release edit "${POLARIS_PACKAGE_REF_NAME}" --verify-tag --draft=false --prerelease --latest=false\n'
    "          else\n"
    '            gh release edit "${POLARIS_PACKAGE_REF_NAME}" --verify-tag --draft=false\n'
    "          fi\n"
)
if release_publish.strip() != expected_release_publish.strip():
    raise AssertionError(
        "only the post-verification step may publish a staged draft release"
    )
arch_job = workflow_job(workflow, "arch-build")
for exact_arch_input in (
    "ARCH_REPOSITORY_SNAPSHOT: 2026/08/19",
    "ARCH_BOOST_PACKAGE_VERSION: 1.92.0-1",
    "ARCH_BOOST_SHA256: 0d795c6401c8bfa16012ada7e2e7f34934fb268f9174470edac1b389056f79bb",
    "ARCH_BOOST_LIBS_SHA256: 4b1392e578e46c1b23910d1c26956927d7e986d6afd5090be59045afb3c04f8d",
):
    if arch_job.count(exact_arch_input) != 1:
        raise AssertionError(f"Arch CI must pin exact input: {exact_arch_input}")

arch_install = re.search(
    r"(?ms)^      - name: Install dependencies\n(?P<body>.*?)(?=^      - name:|\Z)",
    arch_job,
)
if not arch_install:
    raise AssertionError("missing Arch Install dependencies workflow step")
arch_install_tokens = workflow_run_tokens(arch_install.group("body"))
for package in ("libei", "shaderc", "vulkan-headers", "vulkan-icd-loader"):
    if package not in arch_install_tokens:
        raise AssertionError(f"Arch CI dependencies must explicitly install {package}")

boost_install = workflow_step(arch_job, "Install pinned Boost ABI")
boost_install_tokens = workflow_run_tokens(boost_install)
for exact_boost_command in (
    [
        "curl", "--fail", "--location", "--show-error", "--output",
        "$boost_package",
        "${archive_base}/boost/boost-${ARCH_BOOST_PACKAGE_VERSION}-x86_64.pkg.tar.zst",
    ],
    [
        "curl", "--fail", "--location", "--show-error", "--output",
        "$boost_libraries_package",
        "${archive_base}/boost-libs/boost-libs-${ARCH_BOOST_PACKAGE_VERSION}-x86_64.pkg.tar.zst",
    ],
    ["pacman", "-U", "--noconfirm", "$boost_package", "$boost_libraries_package"],
):
    require_command(boost_install_tokens, exact_boost_command, "pinned Arch Boost overlay")
if boost_install_tokens.count("sha256sum") != 2 or boost_install_tokens.count("--check") != 2:
    raise AssertionError("both pinned Arch Boost packages must pass SHA-256 verification")

arch_package_condition = (
    "if: ${{ github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch' || startsWith(github.ref, 'refs/tags/v') || inputs.release_tag != '' }}"
)
for step_name in ("Validate Arch package", "Smoke test Arch package"):
    step = workflow_step(arch_job, step_name)
    if step.count(arch_package_condition) != 1:
        raise AssertionError(f"{step_name} must run for pull requests, manual validation and exact releases")

arch_smoke = workflow_step(arch_job, "Smoke test Arch package")
for metadata_contract in (
    'dependency="${soname%%.so.*}.so"',
    'grep -Fq "depend = ${dependency}=${boost_version}-" arch-pkgbuild/package-pkginfo.txt',
):
    if arch_smoke.count(metadata_contract) != 1:
        raise AssertionError(
            "Arch package smoke must bind every Boost NEEDED entry to a versioned dependency"
        )

optional_native_job_needs = {
    "arch-current-compatibility": (
        "needs: [resolve-source, cpp-sanitizer-tests, arch-build]"
    ),
    "fedora-clang-build": (
        "needs: [resolve-source, web-checks, cpp-sanitizer-tests, arch-build]"
    ),
    "steamos-build": (
        "needs: [resolve-source, web-checks, cpp-sanitizer-tests, arch-build]"
    ),
    "ubuntu-build": (
        "needs: [resolve-source, web-checks, cpp-sanitizer-tests, arch-build]"
    ),
    "fedora-rpm-build": (
        "needs: [resolve-source, web-checks, cpp-sanitizer-tests, arch-build]"
    ),
}
optional_native_jobs = {}
for job_name, expected_needs in optional_native_job_needs.items():
    job = workflow_job(workflow, job_name)
    if job.count(expected_needs) != 1:
        raise AssertionError(f"{job_name} must wait for both required native gates")
    optional_native_jobs[job_name] = job

arch_current_job = optional_native_jobs["arch-current-compatibility"]
if arch_current_job.count(arch_package_condition) != 1:
    raise AssertionError("current Arch compatibility must run for pull requests, manual validation and exact releases")
for current_step in (
    "Synchronize current Arch repositories",
    "Download exact Arch package",
    "Install and launch against current Arch",
):
    workflow_step(arch_current_job, current_step)
for current_contract in (
    "pacman -Syu --noconfirm",
    'pacman -U --noconfirm "$pkg_path"',
    'ldd "$installed_binary"',
    "polaris --version",
):
    if arch_current_job.count(current_contract) != 1:
        raise AssertionError(f"current Arch compatibility is missing: {current_contract}")

if release_job.count("      - arch-current-compatibility\n") != 1:
    raise AssertionError("release publication must wait for current Arch compatibility")

# The Ubuntu build job installs its DEB on the runner that built it, which has every library the
# binary links, so a library missing from Depends passes there. 1.4.13 shipped without
# libpipewire-0.3-0t64 that way. ubuntu-minimal-install installs the same DEB on a bare
# ubuntu:24.04 without Recommends, and a release waits for it.
ubuntu_minimal_job = workflow_job(workflow, "ubuntu-minimal-install")
workflow_step(ubuntu_minimal_job, "Download exact Ubuntu DEB")
ubuntu_minimal_install_step = workflow_step(ubuntu_minimal_job, "Install and launch on a minimal Ubuntu 24.04")
# The step runs in the bare container, whose sh is dash, and dash stops at "set -o pipefail" before
# the DEB is installed. That is how every run failed from the job's first day until it said bash.
if ubuntu_minimal_install_step.count("        shell: bash\n") != 1:
    raise AssertionError("the minimal Ubuntu install step must run under bash: the container's sh is dash, which has no pipefail")
ubuntu_minimal_not_found = (
    '          if grep -Fq "not found" ubuntu-minimal-package/package-ldd.txt; then\n'
    '            echo "The Ubuntu DEB leaves a library it links uninstalled on a bare Ubuntu 24.04;'
    ' name its package in CPACK_DEBIAN_PACKAGE_DEPENDS" >&2\n'
    '            exit 1\n'
    '          fi\n'
)
for minimal_contract in (
    "needs: [resolve-source, ubuntu-build]",
    "if: needs.resolve-source.outputs.native_required == 'true'",
    'apt-get -o Acquire::Retries=3 install -y --no-install-recommends "./$deb_path"',
    ubuntu_minimal_not_found,
    "polaris --version | tee ubuntu-minimal-package/package-version.txt",
):
    if ubuntu_minimal_job.count(minimal_contract) != 1:
        raise AssertionError(f"the minimal Ubuntu install is missing: {minimal_contract.strip()}")
if not re.search(r"(?m)^      image: ubuntu@sha256:[0-9a-f]{64}$", ubuntu_minimal_job):
    raise AssertionError("the minimal Ubuntu install must pin its image by digest")
if release_job.count("      - ubuntu-minimal-install\n") != 1:
    raise AssertionError("release publication must wait for the minimal Ubuntu install")

libei_needed = "NEEDED.*\\[libei\\.so\\.1\\]"
for job_name, job in (("Arch", arch_job), ("Ubuntu DEB", ubuntu_job), ("Fedora RPM", fedora_job)):
    if job.count(libei_needed) != 1:
        raise AssertionError(f"{job_name} CI must check that the packaged binary links libei")
steamos_package_script = read("scripts/ci/build-steamos-package.sh")
if steamos_package_script.count("NEEDED +libei[.]so[.]1") != 1:
    raise AssertionError("SteamOS packaging must check that the packaged binary links libei")
steamos_pacstrap = re.search(r"(?m)^pacstrap (?:.*\\\n)*.*$", read("scripts/ci/run-steamos-build.sh"))
if not steamos_pacstrap or "libei" not in steamos_pacstrap.group(0).replace("\\\n", " ").split():
    raise AssertionError("SteamOS build root must install libei")
for apt_job, apt_job_name in (
    (ubuntu_job, "Ubuntu DEB"),
    (workflow_job(workflow, "cpp-sanitizer-tests"), "C++ sanitizer"),
):
    if len(re.findall(r"(?m)^ +lib[^\n]* libei-dev \\$", apt_job)) != 1:
        raise AssertionError(f"{apt_job_name} CI must install libei-dev")
packaging_cmake = read("cmake/packaging/linux.cmake")
for cpack_dependency in ("libei1, \\\n", "libei >= 1.0, \\\n"):
    if packaging_cmake.count(cpack_dependency) != 1:
        raise AssertionError(f"CPack runtime dependencies must name {cpack_dependency.split(',')[0]}")
# The binary links libpipewire-0.3 whenever PipeWire audio or portal capture is built, and
# dpkg-shlibdeps is off, so the DEB names it by hand. 1.4.13 did not, and on an Ubuntu 24.04 without
# PipeWire the loader refused to start Polaris at all. The Ubuntu build job could not see that, since
# it installs the package on the machine that built it; ubuntu-minimal-install, pinned above, now
# installs it on a bare ubuntu:24.04. (Fedora's rpmbuild finds the library itself, and both
# PKGBUILDs name it.)
linux_compile_cmake = read("cmake/compile_definitions/linux.cmake")
if linux_compile_cmake.count("list(APPEND PLATFORM_LIBRARIES ${PIPEWIRE_LIBRARIES})") != 1:
    raise AssertionError("expected Polaris to link libpipewire-0.3 exactly once; revisit the DEB dependency on it")
deb_dependencies = re.search(
    r'(?ms)^set\(CPACK_DEBIAN_PACKAGE_DEPENDS "\\\n(?P<body>.*?)"\)$',
    packaging_cmake,
)
if not deb_dependencies:
    raise AssertionError("missing the CPack DEB runtime dependency list")
if not re.search(r"(?m)^\s+libpipewire-0\.3-0t64, \\$", deb_dependencies.group("body")):
    raise AssertionError("CPack DEB runtime dependencies must name libpipewire-0.3-0t64, which the binary links")
if not re.search(r"(?m)^Requires:\s+libei >= 1\.0\s*$", fedora):
    raise AssertionError("Fedora runtime dependencies must explicitly include libei")

# A prerelease has to sort below its release in every format, and the binary has to report the label
# its package was named for. Each check is pinned whole, down to the exit that fails the step: a check
# that only prints is no check.
def require_block(script: str, block: str, context: str) -> None:
    if script.count(block) != 1:
        raise AssertionError(f"{context} must run this check exactly once and fail on it:\n{block}")


def runtime_version_block(release: str, output: str, package: str) -> str:
    return (
        f'expected_runtime_version="${{{release}}}"\n'
        'if [ -n "$POLARIS_PRERELEASE_LABEL" ]; then\n'
        f'  expected_runtime_version="${{{release}}}-${{POLARIS_PRERELEASE_LABEL}}"\n'
        'fi\n'
        f'if ! grep -Fq "Polaris version: ${{expected_runtime_version}} commit:" {output}; then\n'
        f'  echo "Installed {package} reports a version other than ${{expected_runtime_version}}" >&2\n'
        '  exit 1\n'
        'fi\n'
    )


ubuntu_smoke = workflow_run_script(workflow_step(ubuntu_job, "Smoke test Ubuntu DEB package"))
fedora_smoke = workflow_run_script(workflow_step(fedora_job, "Smoke test Fedora RPM package"))
arch_smoke_script = workflow_run_script(arch_smoke)
for context, script, blocks in (
    ("Ubuntu DEB smoke", ubuntu_smoke, (
        'if [ "$package_identity" != "$expected_package_identity" ]; then\n'
        '  echo "Ubuntu package identity mismatch: expected \'$expected_package_identity\', got \'$package_identity\'" >&2\n'
        '  exit 1\n'
        'fi\n'
        'if [ -n "$POLARIS_PRERELEASE_LABEL" ] && ! dpkg --compare-versions "$package_version" lt "$BUILD_VERSION"; then\n'
        '  echo "Ubuntu prerelease $package_version does not sort below $BUILD_VERSION, so an upgrade to that release would not replace it" >&2\n'
        '  exit 1\n'
        'fi\n',
        'polaris --version | tee build/cpack_artifacts/package-version.txt\n',
        runtime_version_block("BUILD_VERSION", "build/cpack_artifacts/package-version.txt", "Ubuntu DEB"),
    )),
    ("Fedora RPM smoke", fedora_smoke, (
        'if [ "$package_identity" != "$expected_package_identity" ]; then\n'
        '  echo "Fedora package identity mismatch: expected \'$expected_package_identity\', got \'$package_identity\'" >&2\n'
        '  exit 1\n'
        'fi\n'
        'package_evr="$(rpm -qp --qf \'%{VERSION}-%{RELEASE}\' "$rpm_path")"\n'
        'if [ -n "$POLARIS_PRERELEASE_LABEL" ] && [ "$(rpm --eval "%{lua: print(rpm.vercmp(\'${package_evr}\', \'${BUILD_VERSION}-1\'))}")" != "-1" ]; then\n'
        '  echo "Fedora prerelease $package_evr does not sort below ${BUILD_VERSION}-1, so an upgrade to that release would not replace it" >&2\n'
        '  exit 1\n'
        'fi\n',
        'polaris --version | tee build/cpack_artifacts/package-version.txt\n',
        runtime_version_block("BUILD_VERSION", "build/cpack_artifacts/package-version.txt", "Fedora RPM"),
    )),
    ("Arch package smoke", arch_smoke_script, (
        'release_version="$(grep -Pom1 \'^project\\(Polaris VERSION \\K[^ ]+\' CMakeLists.txt)"\n'
        'expected_pkgver="${release_version}${POLARIS_PRERELEASE_LABEL}-1"\n'
        'if ! grep -Fqx "pkgver = ${expected_pkgver}" arch-pkgbuild/package-pkginfo.txt; then\n'
        '  echo "Arch package version mismatch: expected ${expected_pkgver}" >&2\n'
        '  exit 1\n'
        'fi\n'
        'if [ -n "$POLARIS_PRERELEASE_LABEL" ] && [ "$(vercmp "$expected_pkgver" "${release_version}-1")" != "-1" ]; then\n'
        '  echo "Arch prerelease ${expected_pkgver} does not sort below ${release_version}-1, so an upgrade to that release would not replace it" >&2\n'
        '  exit 1\n'
        'fi\n'
        'kms_pkg_path="$(find arch-pkgbuild -maxdepth 1 -type f -name \'polaris-kms-[0-9]*-x86_64.pkg.tar.*\' | sort | head -n 1)"\n'
        'if [ -z "$kms_pkg_path" ]; then\n'
        '  echo "No Arch polaris-kms package was produced" >&2\n'
        '  exit 1\n'
        'fi\n'
        'tar -xOf "$kms_pkg_path" .PKGINFO > arch-pkgbuild/kms-package-pkginfo.txt\n'
        'if ! grep -Fqx "pkgver = ${expected_pkgver}" arch-pkgbuild/kms-package-pkginfo.txt ||\n'
        '   ! grep -Fqx "depend = polaris=${expected_pkgver}" arch-pkgbuild/kms-package-pkginfo.txt; then\n'
        '  echo "Arch polaris-kms must be ${expected_pkgver} and depend on exactly polaris=${expected_pkgver}" >&2\n'
        '  exit 1\n'
        'fi\n',
        'polaris --version | tee arch-pkgbuild/package-version.txt\n',
        runtime_version_block("release_version", "arch-pkgbuild/package-version.txt", "Arch package"),
    )),
):
    for block in blocks:
        require_block(script, block, context)
    # The binary is asked after it is installed, and only then is its answer read.
    if script.index("polaris --version | tee ") > script.index('expected_runtime_version="'):
        raise AssertionError(f"{context} must read the installed binary's version before checking it")

steamos_build_script = read("scripts/ci/build-steamos-package.sh")
for block in (
    'EXPECTED_PKGVER="1.4.14${POLARIS_PRERELEASE_LABEL}-1"\n'
    'if [ "$PACKAGE_IDENTITY" != "polaris|$EXPECTED_PKGVER|x86_64" ]; then\n'
    "  printf 'unexpected SteamOS package identity: %s\\n' \"$PACKAGE_IDENTITY\" >&2\n"
    '  exit 1\n'
    'fi\n'
    'if [ -n "$POLARIS_PRERELEASE_LABEL" ] && [ "$(vercmp "$PACKAGE_VERSION" "$BUILD_VERSION-1")" != -1 ]; then\n'
    "  printf 'SteamOS prerelease %s does not sort below %s-1, so an upgrade to that release would not replace it\\n' \\\n"
    '    "$PACKAGE_VERSION" "$BUILD_VERSION" >&2\n'
    '  exit 1\n'
    'fi\n',
    'if [ "$KMS_IDENTITY" != "polaris-kms|$EXPECTED_PKGVER|x86_64" ]; then\n'
    "  printf 'unexpected SteamOS polaris-kms package identity: %s\\n' \"$KMS_IDENTITY\" >&2\n"
    '  exit 1\n'
    'fi\n'
    '# Exactly this version of Polaris, so the helper and the binary can never disagree.\n'
    'if ! grep -Fqx "depend = polaris=$EXPECTED_PKGVER" "$KMS_RECEIPT_ROOT/.PKGINFO"; then\n'
    "  printf '%s\\n' 'polaris-kms must depend on the exact Polaris it was built with' >&2\n"
    "  sed -n 's/^depend = /  depends: /p' \"$KMS_RECEIPT_ROOT/.PKGINFO\" >&2\n"
    '  exit 1\n'
    'fi\n',
):
    require_block(steamos_build_script, block, "SteamOS package build")
require_block(
    read("scripts/ci/run-steamos-build.sh"),
    'chroot "$STEAMOS_ROOT" /usr/bin/polaris --version \\\n'
    '  > /output/steamos3.8-installed-version.txt\n'
    '# The binary reports the release number, and a prerelease\'s label the way the host spells it,\n'
    '# X.Y.Z-beta.N. The package version alone cannot show that the build itself had the label.\n'
    'EXPECTED_RUNTIME_VERSION="$(grep -Pom1 \'^project\\(Polaris VERSION \\K[^ ]+\' /workspace/CMakeLists.txt)"\n'
    'if [ -n "$POLARIS_PRERELEASE_LABEL" ]; then\n'
    '  EXPECTED_RUNTIME_VERSION="$EXPECTED_RUNTIME_VERSION-$POLARIS_PRERELEASE_LABEL"\n'
    'fi\n'
    'if ! grep -Fq "Polaris version: $EXPECTED_RUNTIME_VERSION commit:" /output/steamos3.8-installed-version.txt; then\n'
    "  printf 'installed SteamOS binary reports a version other than %s\\n' \"$EXPECTED_RUNTIME_VERSION\" >&2\n"
    '  exit 1\n'
    'fi\n',
    "SteamOS installed binary",
)

# Each package lane receives the label on its own, so release assembly reads every final package
# against the tag and the resolved channel before anything is published.
release_versions = workflow_step(release_job, "Verify release package versions match the tag")
if "        if:" in release_versions:
    raise AssertionError("the release package version check must run on every release")
for binding in (
    "          BUILD_VERSION: ${{ needs.resolve-source.outputs.version }}\n",
    "          IS_PRERELEASE: ${{ needs.resolve-source.outputs.prerelease }}\n",
    "          POLARIS_PRERELEASE_LABEL: ${{ needs.resolve-source.outputs.prerelease_label }}\n",
):
    if release_versions.count(binding) != 1:
        raise AssertionError(f"the release package version check must bind {binding.strip()}")
require_block(
    workflow_run_script(release_versions),
    'set -euo pipefail\n'
    'if ! command -v rpm >/dev/null; then\n'
    '  sudo apt-get update -qq\n'
    '  sudo apt-get install -y -qq --no-install-recommends rpm\n'
    'fi\n'
    'python3 scripts/ci/check-release-package-versions.py \\\n'
    '  --tag "$POLARIS_PACKAGE_REF_NAME" \\\n'
    '  --version "$BUILD_VERSION" \\\n'
    '  --prerelease "$IS_PRERELEASE" \\\n'
    '  --label "$POLARIS_PRERELEASE_LABEL" \\\n'
    '  release-assets/final\n',
    "release package version check",
)

# The ordering test runs wherever a package manager lives, and each run names the one it must have:
# dpkg on the Ubuntu runner, vercmp in the Arch container, rpm in the Fedora container.
for job_name, job, step_name, tool in (
    ("resolve-source", resolve_job, "Verify prerelease package versions sort below their release", "dpkg"),
    ("arch-build", arch_job, "Verify prerelease pacman versions sort below their release", "vercmp"),
    ("fedora-rpm-build", fedora_job, "Verify prerelease RPM versions sort below their release", "rpm"),
):
    order_step = workflow_step(job, step_name)
    expected_order_step = (
        "        env:\n"
        f"          POLARIS_VERSION_ORDER_TOOLS: {tool}\n"
        "        run: python3 -m unittest discover -s tests/scripts -p 'test_prerelease_package_versions.py'\n"
    )
    if order_step.count(expected_order_step) != 1 or "        if:" in order_step:
        raise AssertionError(f"{job_name} must always run the prerelease ordering test and require {tool}")
release_check_test = workflow_step(resolve_job, "Verify the release package version check")
if release_check_test.strip() != (
    "run: python3 -m unittest discover -s tests/scripts -p 'test_release_package_versions.py'"
):
    raise AssertionError("resolve-source must run the release package version check's own tests")

print("Release package dependency contracts look correct.")
