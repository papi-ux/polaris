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


self_test_cmake_bool_contract()

arch = read("packaging/linux/Arch/PKGBUILD")
require_package(shell_array(arch, "depends"), "vulkan-icd-loader", "Arch runtime dependencies")
require_package(shell_array(arch, "makedepends"), "vulkan-headers", "Arch build dependencies")

fedora = read("packaging/linux/fedora/Polaris.spec")
for package in ("pipewire-devel", "vulkan-loader-devel"):
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
if len(re.findall(r"(?m)^  fedora-rpm-build:\s*$", workflow)) != 1:
    raise AssertionError("release workflow must define exactly one Fedora RPM job")
fedora_clang_job = workflow_job(workflow, "fedora-clang-build")
fedora_job = workflow_job(workflow, "fedora-rpm-build")

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
    "          ref: ${{ env.POLARIS_CHECKOUT_REF }}\n"
)
if release_checkout.strip() != expected_release_checkout.strip():
    raise AssertionError(
        "release-assets must check out the exact packaged ref before reading curated notes"
    )
release_upload = re.search(
    r"(?ms)^      - name: Upload release assets to GitHub release\n(?P<body>.*?)(?=^      - name:|\Z)",
    release_job,
)
if not release_upload:
    raise AssertionError("missing release asset upload workflow step")
if release_job.index("- name: Check out exact release source") >= release_job.index(
    "- name: Upload release assets to GitHub release"
):
    raise AssertionError("release source checkout must precede release publication")
release_upload_tokens = workflow_run_tokens(release_upload.group("body"))
for legacy_version in ("42", "43"):
    for cleanup_asset in (
        f"Polaris-fedora{legacy_version}-x86_64.rpm",
        f"Polaris-fedora{legacy_version}-src.rpm",
    ):
        if cleanup_asset not in release_upload_tokens:
            raise AssertionError(f"release workflow must delete stale asset: {cleanup_asset}")
cleanup_command = [
    "gh", "release", "delete-asset", "${POLARIS_PACKAGE_REF_NAME}", "${legacy_asset}", "--yes",
]
if not contains_command(release_upload_tokens, cleanup_command):
    raise AssertionError("release workflow must invoke gh release delete-asset for each stale Fedora asset")
release_verify = re.search(
    r"(?ms)^      - name: Verify release assets on GitHub release\n(?P<body>.*?)(?=^      - name:|\Z)",
    release_job,
)
if not release_verify:
    raise AssertionError("missing release asset verification workflow step")
release_verify_body = release_verify.group("body")
release_verify_script = workflow_run_script(release_verify_body)
expected_supported_assignment = (
    'supported_count=$(gh release view "${POLARIS_PACKAGE_REF_NAME}" --json assets --jq '
    "'[.assets[].name | select(. == \"Polaris-fedora44-x86_64.rpm\" or "
    ". == \"Polaris-ubuntu24.04-x86_64.deb\" or "
    ". == \"Polaris-arch-x86_64.pkg.tar.zst\" or "
    ". == \"Polaris-steamos3.8-x86_64.pkg.tar.zst\")] | length')"
)
expected_legacy_assignment = (
    'legacy_count=$(gh release view "${POLARIS_PACKAGE_REF_NAME}" --json assets --jq '
    "'[.assets[].name | select(startswith(\"Polaris-fedora42-\") or "
    "startswith(\"Polaris-fedora43-\"))] | length')"
)
for variable, expected_assignment in (
    ("supported_count", expected_supported_assignment),
    ("legacy_count", expected_legacy_assignment),
):
    assignment_mentions = [
        line
        for line in release_verify_script.splitlines()
        if re.search(rf"(^|[^A-Za-z0-9_]){re.escape(variable)}=", line)
    ]
    if assignment_mentions != [expected_assignment]:
        raise AssertionError(
            f"release verification must bind {variable} exactly once at top level using the reviewed query"
        )
expected_release_verify_lines = [
    expected_supported_assignment,
    expected_legacy_assignment,
    'if [ "${supported_count}" -ne 4 ] || [ "${legacy_count}" -ne 0 ]; then',
    '  echo "Expected Fedora 44, Ubuntu 24.04, Arch, and SteamOS 3.8 binary assets with no Fedora 42/43 assets on ${POLARIS_PACKAGE_REF_NAME}; supported=${supported_count}, legacy=${legacy_count}" >&2',
    "  exit 1",
    "fi",
]
if release_verify_script.splitlines() != expected_release_verify_lines:
    raise AssertionError(
        "release verification must use the exact reviewed top-level query and failure program"
    )
arch_job = workflow_job(workflow, "arch-build")
arch_install = re.search(
    r"(?ms)^      - name: Install dependencies\n(?P<body>.*?)(?=^      - name:|\Z)",
    arch_job,
)
if not arch_install:
    raise AssertionError("missing Arch Install dependencies workflow step")
arch_install_tokens = workflow_run_tokens(arch_install.group("body"))
for package in ("vulkan-headers", "vulkan-icd-loader"):
    if package not in arch_install_tokens:
        raise AssertionError(f"Arch CI dependencies must explicitly install {package}")

print("Release package dependency contracts look correct.")
