import { readFileSync, readdirSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')
const packageAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const sysextAsset = 'Polaris-sysext-x86_64.raw'

const currentChangelog = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.3 - 2026-09-05')
  const end = changelog.indexOf('## v1.4.2 - 2026-09-04')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const releaseNotes = () => read('docs/release-notes/v1.4.3.md')
const installBlocks = () => [...releaseNotes().matchAll(/```bash\n([\s\S]*?)\n```/g)]
  .map((match) => match[1])
  .filter((block) => block.includes('wget --output-document='))

describe('v1.4.3 release contract', () => {
  it('moves every current release identity to v1.4.3', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.3')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.3"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain('usr/bin/polaris-1.4.3')
    expect(read('scripts/ci/build-steamos-package.sh')).toContain("'polaris|1.4.3-1|x86_64'")
    expect(read('src_assets/common/assets/web/packaging-contracts.test.js')).toContain("'polaris|1.4.3-1|x86_64'")
  })

  it('makes v1.4.2 historical and promotes the complete v1.4.3 scope', () => {
    expect(read('src_assets/common/assets/web/release-v142-contract.test.js')).toContain("describe('historical v1.4.2 release contract'")
    const gate = read('scripts/check-public-docs.sh')
    expect(gate).toContain('docs/release-notes/v1.4.3.md')
    expect(gate).toContain('"## v1.4.3 - 2026-09-05"')

    const release = `${currentChangelog()}\n${releaseNotes()}`
    for (const fact of [
      'Gamescope Session Helper',
      'polaris-gamescope-session',
      'PATCH',
      'SteamGridDB',
      'explicit launch fields',
      'installed Polaris package',
      'system extension',
      'Nova v1.4.3',
      'Retroid Pocket 6',
    ]) {
      expect(release, `release must include: ${fact}`).toContain(fact)
    }
  })

  it('documents the merge write, the coded artwork failures, and the extension image against the source', () => {
    const notes = releaseNotes()
    const confighttp = read('src/confighttp.cpp')
    const artwork = read('src/game_artwork_manual.cpp')
    const helper = read('src/platform/linux/gamescope_session_helper.cpp')
    const workflow = read('.github/workflows/build.yml')

    expect(notes).toContain('stale or shadowed copy')
    expect(notes).toContain('unproven on a real Bazzite host')
    expect(confighttp).toContain('patchConfig')
    expect(artwork).toContain('classify_search_failure')
    expect(helper).toContain('bundled')
    expect(workflow).toContain('scripts/ci/build-sysext-image.sh')
    expect(workflow).toContain(`release-assets/final/${sysextAsset}`)
  })

  it('pins all four install commands to v1.4.3 and lists the extension image beside the packages', () => {
    const blocks = installBlocks()
    expect(blocks).toHaveLength(4)
    for (const asset of packageAssets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `one command block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.3/${asset}`)
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
    expect(steamOs).toContain('sudo pacman-key --init || exit $?')
    expect(steamOs).toContain('sudo pacman-key --populate || exit $?')
    expect(steamOs).toContain('systemctl --user enable --now polaris')

    const listed = [...new Set(releaseNotes().match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual([...packageAssets, sysextAsset].sort())
  })

  it('publishes release-note guide links that work outside the source tree', () => {
    const releaseNotesDir = join(process.cwd(), 'docs/release-notes')
    const notes = readdirSync(releaseNotesDir)
      .filter((name) => /^v\d+\.\d+\.\d+\.md$/.test(name))
      .map((name) => read(`docs/release-notes/${name}`))
      .join('\n')

    expect(notes).not.toMatch(/\]\(\.\.\/(?:bazzite|steamos)\.md(?:[?#][^)]*)?\)/)
    expect(notes).toContain('[Bazzite guide](https://papi-ux.com/docs/bazzite/)')
    expect(notes).toContain('[SteamOS guide](https://papi-ux.com/docs/steamos/)')
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
