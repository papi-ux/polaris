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
resolve_job = workflow_job(workflow, "resolve-source")
resolve_script = workflow_run_script(
    workflow_step(resolve_job, "Bind release tag to source commit")
)
for required_source_guard in (
    '^v[0-9]+\\.[0-9]+\\.[0-9]+$',
    'git fetch --no-tags --force origin',
    'tag_commit="$(git rev-parse "refs/tags/${release_tag}^{commit}")"',
    'if [ "$tag_commit" != "$source_commit" ]; then',
    'echo "commit=$source_commit" >> "$GITHUB_OUTPUT"',
):
    if required_source_guard not in resolve_script:
        raise AssertionError(
            f"exact-source resolver is missing release guard: {required_source_guard}"
        )
if resolve_script.index('tag_commit="$(git rev-parse') >= resolve_script.index(
    'echo "commit=$source_commit"'
):
    raise AssertionError("release tag must be validated before exporting the source commit")

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
    "          ref: ${{ needs.resolve-source.outputs.commit }}\n"
)
if release_checkout.strip() != expected_release_checkout.strip():
    raise AssertionError(
        "release-assets must check out the exact packaged ref before reading curated notes"
    )
release_step_names = (
    "Check out exact release source",
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

release_tag_validation = workflow_run_script(
    workflow_step(release_job, "Revalidate release tag against packaged source")
)
for required_tag_guard in (
    'checked_out_commit="$(git rev-parse HEAD)"',
    'if [ "$checked_out_commit" != "$EXPECTED_SOURCE_COMMIT" ]; then',
    'git fetch --no-tags --force origin',
    'tag_commit="$(git rev-parse "refs/tags/${POLARIS_PACKAGE_REF_NAME}^{commit}")"',
    'if [ "$tag_commit" != "$EXPECTED_SOURCE_COMMIT" ]; then',
):
    if required_tag_guard not in release_tag_validation:
        raise AssertionError(
            f"release-assets tag revalidation is missing: {required_tag_guard}"
        )

release_stage = workflow_step(release_job, "Stage curated GitHub release")
release_stage_script = workflow_run_script(release_stage)
if "id: stage-release" not in release_stage:
    raise AssertionError("curated release staging must export draft publication state")
for required_stage_fact in (
    'release_notes="docs/release-notes/${POLARIS_PACKAGE_REF_NAME}.md"',
    'gh release create "${POLARIS_PACKAGE_REF_NAME}"',
    "--draft",
    "--verify-tag",
    'gh release edit "${POLARIS_PACKAGE_REF_NAME}"',
    "--draft=true",
    'published_notes="$(gh release view "${POLARIS_PACKAGE_REF_NAME}" --json body --jq .body)"',
    'if [ "$published_notes" != "$expected_notes" ]; then',
):
    if required_stage_fact not in release_stage_script:
        raise AssertionError(f"curated release staging is missing: {required_stage_fact}")
if release_stage_script.count("--verify-tag") != 2:
    raise AssertionError("both release create and edit must verify that the tag exists")
if release_stage_script.count('echo "publish_draft=true" >> "$GITHUB_OUTPUT"') != 2:
    raise AssertionError("both new and existing releases must remain draft until asset verification")
if "is_draft=" in release_stage_script:
    raise AssertionError("an existing public release must not stay public during rerun mutation")
if "gh release upload" in release_stage_script or "gh release delete-asset" in release_stage_script:
    raise AssertionError("release notes must be verified before any asset mutation")

release_upload = workflow_step(release_job, "Upload release assets to GitHub release")
release_upload_tokens = workflow_run_tokens(release_upload)
if "published_notes=" in workflow_run_script(release_upload):
    raise AssertionError("release-note verification must not move after asset upload")
for exact_upload_fact in (
    'release_files=(release-assets/final/*)',
    'declare -A expected_asset_names=()',
    'expected_asset_names["$(basename "$release_file")"]=1',
    'gh release view "${POLARIS_PACKAGE_REF_NAME}" --json assets --jq \'.assets[].name\'',
    'if [[ -z "${expected_asset_names[$published_asset]+present}" ]]; then',
    'gh release delete-asset "${POLARIS_PACKAGE_REF_NAME}" "$published_asset" --yes',
    'gh release upload "${POLARIS_PACKAGE_REF_NAME}" "${release_files[@]}" --clobber',
):
    if exact_upload_fact not in workflow_run_script(release_upload):
        raise AssertionError(
            f"release upload must remove assets outside the finalized set: {exact_upload_fact}"
        )
if "|| true" in workflow_run_script(release_upload):
    raise AssertionError("remote asset cleanup must fail closed")
release_verify_body = workflow_step(release_job, "Verify release assets on GitHub release")
release_verify_script = workflow_run_script(release_verify_body)
for exact_verify_fact in (
    "set -euo pipefail",
    "find release-assets/final -maxdepth 1 -type f -printf '%f\\n' | sort",
    "required_binary_assets=(",
    'for required_asset in "${required_binary_assets[@]}"; do',
    'gh release view "${POLARIS_PACKAGE_REF_NAME}" --json assets --jq \'.assets[].name\' | sort',
    'if [ "${#expected_assets[@]}" -eq 0 ] || [ "${expected_assets[*]}" != "${published_assets[*]}" ]; then',
):
    if exact_verify_fact not in release_verify_script:
        raise AssertionError(
            f"release verification must compare the exact local and remote asset sets: {exact_verify_fact}"
        )
for partial_check in ("supported_count", "legacy_count"):
    if partial_check in release_verify_script:
        raise AssertionError("release verification must not accept a partial asset subset")
release_publish = workflow_step(release_job, "Publish verified draft release")
expected_release_publish = (
    "        if: steps.stage-release.outputs.publish_draft == 'true'\n"
    "        env:\n"
    "          GH_TOKEN: ${{ github.token }}\n"
    "          GH_REPO: ${{ github.repository }}\n"
    '        run: gh release edit "${POLARIS_PACKAGE_REF_NAME}" --verify-tag --draft=false\n'
)
if release_publish.strip() != expected_release_publish.strip():
    raise AssertionError(
        "only the post-verification step may publish a staged draft release"
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
