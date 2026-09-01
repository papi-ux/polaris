import { readFileSync, readdirSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')
const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

const currentChangelog = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.0 - 2026-09-01')
  const end = changelog.indexOf('## v1.3.13 - 2026-08-22')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const installBlocks = () => [...read('docs/release-notes/v1.4.0.md').matchAll(/```bash\n([\s\S]*?)\n```/g)]
  .map((match) => match[1])
  .filter((block) => block.includes('wget --output-document='))

describe('v1.4.0 release contract', () => {
  it('moves every current release identity to v1.4.0', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.0')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.0"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain('usr/bin/polaris-1.4.0')
    expect(read('scripts/ci/build-steamos-package.sh')).toContain("'polaris|1.4.0-1|x86_64'")
    expect(read('src_assets/common/assets/web/packaging-contracts.test.js')).toContain("'polaris|1.4.0-1|x86_64'")
  })

  it('retires v1.3.13 to a historical contract and makes v1.4.0 current', () => {
    expect(read('src_assets/common/assets/web/release-v1313-contract.test.js')).toContain("describe('historical v1.3.13 release contract'")
    const gate = read('scripts/check-public-docs.sh')
    expect(gate).toContain('docs/release-notes/v1.4.0.md')
    expect(gate).toContain('"## v1.4.0 - 2026-09-01"')
  })

  it('publishes only the approved v1.4.0 scope', () => {
    const release = `${currentChangelog()}\n${read('docs/release-notes/v1.4.0.md')}`
    for (const fact of [
      'frame-pacing',
      'apply_recovery_profile_next_launch',
      'unsupported_deprecated',
      'deterministic',
      'resolved_profile.fields',
      'MangoHud',
      'automatic rollback',
      'Doctor v2',
      'Steam Input',
      'Nova v1.4.0',
    ]) {
      expect(release, `release must include: ${fact}`).toContain(fact)
    }
    for (const excluded of [
      'history_safe',
      'durable next-launch recovery profile',
      'AI Auto Quality Preference',
    ]) {
      expect(release, `release must exclude: ${excluded}`).not.toContain(excluded)
    }
  })

  it('pins all four install commands to v1.4.0 and preserves platform safety', () => {
    const blocks = installBlocks()
    expect(blocks).toHaveLength(4)
    for (const asset of expectedAssets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `one command block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.0/${asset}`)
      expect(matches[0]).toContain('sudo -H polaris --setup-host')
    }

    for (const asset of [
      'Polaris-arch-x86_64.pkg.tar.zst',
      'Polaris-fedora44-x86_64.rpm',
      'Polaris-ubuntu24.04-x86_64.deb',
    ]) {
      const block = blocks.find((candidate) => candidate.includes(`/${asset}`))
      expect(block).toContain('systemctl --user restart polaris')
    }

    const steamOs = blocks.find((block) => block.includes('/Polaris-steamos3.8-x86_64.pkg.tar.zst'))
    expect(steamOs).toContain("trap 'sudo steamos-readonly enable' EXIT")
    expect(steamOs).toContain('sudo steamos-readonly disable || exit $?')
    expect(steamOs).toContain('sudo pacman-key --init || exit $?')
    expect(steamOs).toContain('sudo pacman-key --populate || exit $?')
    expect(steamOs).toContain('sudo steamos-readonly enable || exit $?')
    expect(steamOs).toContain('systemctl --user enable --now polaris')

    const notes = read('docs/release-notes/v1.4.0.md')
    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
  })

  it('publishes release-note guide links that work outside the source tree', () => {
    const releaseNotesDir = join(process.cwd(), 'docs/release-notes')
    const releaseNotes = readdirSync(releaseNotesDir)
      .filter((name) => /^v\d+\.\d+\.\d+\.md$/.test(name))
      .map((name) => read(`docs/release-notes/${name}`))
      .join('\n')

    expect(releaseNotes).not.toMatch(/\]\(\.\.\/(?:bazzite|steamos)\.md(?:[?#][^)]*)?\)/)
    expect(releaseNotes).toContain('[Bazzite guide](https://papi-ux.com/docs/bazzite/)')
    expect(releaseNotes).toContain('[SteamOS guide](https://papi-ux.com/docs/steamos/)')
  })

  it('keeps publication bound to one verified immutable source', () => {
    const workflow = read('.github/workflows/build.yml')
    const ordered = [
      '- name: Check out exact release source',
      '- name: Revalidate release tag against packaged source',
      '- name: Stage curated GitHub release',
      '- name: Upload release assets to GitHub release',
      '- name: Verify release assets on GitHub release',
      '- name: Publish verified draft release',
    ]
    const positions = ordered.map((step) => workflow.indexOf(step))
    expect(positions.every((position) => position >= 0)).toBe(true)
    expect(positions).toEqual([...positions].sort((a, b) => a - b))
    expect(workflow).toContain('release_notes="docs/release-notes/${POLARIS_PACKAGE_REF_NAME}.md"')
    expect(workflow).toContain('tag_commit="$(git rev-parse "refs/tags/${release_tag}^{commit}")"')
    expect(workflow).toContain('--verify-tag')
    expect(workflow).toContain('--notes-file "$release_notes"')
    expect(workflow).toContain('run: gh release edit "${POLARIS_PACKAGE_REF_NAME}" --verify-tag --draft=false')
  })
})
