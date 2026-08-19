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
resolve_tokens = shell_tokens(resolve_script)
reject_heredoc(resolve_tokens, "exact-source resolver")
for required_source_command in (
    ["set", "-euo", "pipefail"],
    ["source_commit=$(git rev-parse HEAD)"],
    ["git", "fetch", "--no-tags", "--force", "origin", "refs/tags/${release_tag}:refs/tags/${release_tag}"],
    ["tag_commit=$(git rev-parse refs/tags/${release_tag}^{commit})"],
    [
        "if", "[[", "!", "$release_tag", "=~", "^v[0-9]+.[0-9]+.[0-9]+$", "]]", ";", "then", ";",
        "echo", "Release tag must match vMAJOR.MINOR.PATCH: $release_tag", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    [
        "if", "[", "$tag_commit", "!=", "$source_commit", "]", ";", "then", ";",
        "echo", "Release tag $release_tag resolves to $tag_commit, not checked-out source $source_commit", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    ["echo", "commit=$source_commit", ">>", "$GITHUB_OUTPUT"],
):
    require_command(resolve_tokens, required_source_command, "exact-source resolver")

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
release_stage_tokens = workflow_run_tokens(release_stage)
reject_heredoc(release_stage_tokens, "curated release staging")
if not re.search(r"(?m)^        id: stage-release\s*$", release_stage):
    raise AssertionError("curated release staging must export draft publication state")
for required_stage_tokens in (
    ["release_notes=docs/release-notes/${POLARIS_PACKAGE_REF_NAME}.md"],
    ["set", "-euo", "pipefail"],
    [
        "if", "[", "!", "-s", "$release_notes", "]", ";", "then", ";",
        "echo", "Missing curated release notes: $release_notes", ">", "&", "2", ";",
        "exit", "1", ";", "fi",
    ],
    [
        "gh", "release", "create", "${POLARIS_PACKAGE_REF_NAME}",
        "--draft", "--verify-tag", "--title", "${POLARIS_PACKAGE_REF_NAME}",
        "--notes-file", "$release_notes",
    ],
    [
        "gh", "release", "edit", "${POLARIS_PACKAGE_REF_NAME}",
        "--verify-tag", "--draft=true", "--title", "${POLARIS_PACKAGE_REF_NAME}",
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
        "--verify-tag", "--title", "${POLARIS_PACKAGE_REF_NAME}",
        "--notes-file", "$release_notes",
    ],
    [
        "release", "edit", "${POLARIS_PACKAGE_REF_NAME}", "--verify-tag",
        "--draft=true", "--title", "${POLARIS_PACKAGE_REF_NAME}",
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
release_upload_tokens = workflow_run_tokens(release_upload)
reject_heredoc(release_upload_tokens, "release asset upload")
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
expected_required_binaries = [
    "Polaris-arch-x86_64.pkg.tar.zst",
    "Polaris-fedora44-x86_64.rpm",
    "Polaris-steamos3.8-x86_64.pkg.tar.zst",
    "Polaris-ubuntu24.04-x86_64.deb",
]
if executable_array(release_verify_tokens, "required_binary_assets") != expected_required_binaries:
    raise AssertionError("release verification must require the exact four binary assets")
for partial_check in ("supported_count", "legacy_count"):
    if any(partial_check in token for token in release_verify_tokens):
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
