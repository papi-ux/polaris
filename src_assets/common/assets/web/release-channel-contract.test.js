import { spawnSync } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'

import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')
const workflow = read('.github/workflows/build.yml')

const section = (start, end) => {
  const from = workflow.indexOf(start)
  expect(from, `missing workflow section: ${start}`).toBeGreaterThanOrEqual(0)
  const to = workflow.indexOf(end, from + start.length)
  expect(to, `missing section end after ${start}: ${end}`).toBeGreaterThan(from)
  return workflow.slice(from, to)
}

// The tag rule the release job enforces, read out of the workflow itself so this test fails when
// the rule changes rather than when someone rewords the error message next to it.
const tagPattern = () => {
  const match = workflow.match(/release_tag" =~ \^([^\]]*\])?[^ ]*\$/)
  expect(match, 'the release tag test is no longer a bash regex comparison').not.toBeNull()
  const bash = match[0].replace('release_tag" =~ ', '')
  return new RegExp(bash)
}

describe('release channel', () => {
  // A beta carries the number of the release it precedes, and the catalog and the notes stay at that
  // plain version. Its label, beta.N or rc.N, reaches every package build, which spells it so the
  // package sorts below the release: v1.4.13-beta.3 packages carried 1.4.13 itself, and dnf answered
  // the 1.4.13 release with "Nothing to do".
  it('accepts a beta or rc tag for the version the source already carries', () => {
    const pattern = tagPattern()

    for (const tag of ['v1.4.13', 'v1.4.13-beta.1', 'v1.4.13-beta.12', 'v1.4.13-rc.1', 'v10.20.30-rc.99']) {
      expect(pattern.test(tag), `${tag} must be a valid release tag`).toBe(true)
    }
    for (const tag of ['1.4.13', 'v1.4', 'v1.4.13-beta', 'v1.4.13beta.1', 'v1.4.13-alpha.1', 'v1.4.13-beta.1.2', 'v1.4.13 ']) {
      expect(pattern.test(tag), `${tag} must not be a valid release tag`).toBe(false)
    }
  })

  it('binds a beta to the source version and marks it a prerelease', () => {
    const bind = section('release_tag="${POLARIS_PACKAGE_REF_NAME}"', 'git tag -d "$release_tag"')

    // The numeric part of the tag, not the whole tag, is what has to equal the built version.
    expect(bind).toContain('release_version="${release_tag#v}"')
    expect(bind).toContain('release_version="${release_version%%-*}"')
    expect(bind).toContain('if [ "$release_version" != "$build_version" ]; then')
    // Anything carrying a suffix is a prerelease, decided once and passed down.
    expect(bind).toContain('if [ "$release_tag" != "v${build_version}" ]; then')
    expect(bind).toContain('prerelease=true')
    expect(workflow).toContain('prerelease: ${{ steps.source.outputs.prerelease }}')
  })

  it('hands the tag\'s prerelease label to every package build', () => {
    const bind = section('release_tag="${POLARIS_PACKAGE_REF_NAME}"', 'git tag -d "$release_tag"')
    const labelLines = bind.split('\n').map((line) => line.trim()).filter((line) => line.startsWith('prerelease_label='))
    expect(labelLines).toEqual(['prerelease_label="${release_tag#v${build_version}-}"'])
    // Run the workflow's own expression rather than restating it.
    for (const [tag, label] of [['v1.4.13-beta.3', 'beta.3'], ['v1.4.13-rc.12', 'rc.12'], ['v10.20.30-beta.1', 'beta.1']]) {
      const buildVersion = tag.slice(1).split('-')[0]
      const result = spawnSync('bash', ['-c', `release_tag='${tag}'; build_version='${buildVersion}'; ${labelLines[0]}; printf '%s' "$prerelease_label"`], { encoding: 'utf8' })
      expect(result.status).toBe(0)
      expect(result.stdout).toBe(label)
    }
    // Set only where prerelease=true is: a stable tag, a branch and a pull request carry none.
    expect(bind).toContain('prerelease=true\n              prerelease_label="${release_tag#v${build_version}-}"\n            fi')
    expect(workflow).toContain('          prerelease=false\n          prerelease_label=""\n')
    expect(workflow).toContain('echo "prerelease_label=$prerelease_label" >> "$GITHUB_OUTPUT"')
    expect(workflow).toContain('prerelease_label: ${{ steps.source.outputs.prerelease_label }}')

    const labelEnv = 'POLARIS_PRERELEASE_LABEL: ${{ needs.resolve-source.outputs.prerelease_label }}'
    for (const start of [
      '      - name: Generate Arch package files',
      '      - name: Smoke test Arch package',
      '      - name: Build and validate SteamOS 3.8 package',
    ]) {
      expect(section(start, '\n      - name: '), start).toContain(labelEnv)
    }
    for (const [start, end] of [['  ubuntu-build:', '    steps:'], ['  fedora-rpm-build:', '    steps:']]) {
      expect(section(start, end), start).toContain(`      BUILD_VERSION: \${{ needs.resolve-source.outputs.version }}\n      ${labelEnv}\n`)
    }
    expect(section('      - name: Generate Arch package files', '\n      - name: '))
      .toContain('POLARIS_PRERELEASE_LABEL="$POLARIS_PRERELEASE_LABEL" \\\n')
  })

  it('reads the notes of the release a beta precedes', () => {
    const stage = section('- name: Stage curated GitHub release', '- name: Upload release assets')

    expect(stage).toContain('notes_tag="${POLARIS_PACKAGE_REF_NAME%%-*}"')
    expect(stage).toContain('release_notes="docs/release-notes/${notes_tag}.md"')
    expect(stage, 'a missing notes file must still stop the release').toContain('Missing curated release notes')
  })

  it('stages a beta as a prerelease through both the create and the edit path', () => {
    const stage = section('- name: Stage curated GitHub release', '- name: Upload release assets')

    expect(stage).toContain('IS_PRERELEASE: ${{ needs.resolve-source.outputs.prerelease }}')
    expect(stage).toContain('channel=(--prerelease)')
    // Both paths are used: create for a first attempt, edit when a rerun finds the release already
    // staged. A beta that took the edit path and lost its flag would publish as a stable release.
    expect(stage.match(/"\$\{channel\[@\]\}"/g)?.length, 'create and edit must both carry the channel').toBe(2)
  })

  it('never lets a beta become the latest release', () => {
    const publish = workflow.slice(workflow.indexOf('- name: Publish verified draft release'))

    expect(publish).toContain('--draft=false --prerelease --latest=false')
    // The stable path stays exactly as it was: GitHub decides latest for it.
    expect(publish).toContain('gh release edit "${POLARIS_PACKAGE_REF_NAME}" --verify-tag --draft=false\n')
  })
})
