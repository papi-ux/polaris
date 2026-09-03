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
  const start = changelog.indexOf('## v1.4.1 - 2026-09-03')
  const end = changelog.indexOf('## v1.4.0 - 2026-09-01')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const releaseNotes = () => read('docs/release-notes/v1.4.1.md')
const installBlocks = () => [...releaseNotes().matchAll(/```bash\n([\s\S]*?)\n```/g)]
  .map((match) => match[1])
  .filter((block) => block.includes('wget --output-document='))

describe('v1.4.1 release contract', () => {
  it('moves every current release identity to v1.4.1', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.1')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.1"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain('usr/bin/polaris-1.4.1')
    expect(read('scripts/ci/build-steamos-package.sh')).toContain("'polaris|1.4.1-1|x86_64'")
    expect(read('src_assets/common/assets/web/packaging-contracts.test.js')).toContain("'polaris|1.4.1-1|x86_64'")
  })

  it('makes v1.4.0 historical and promotes the complete v1.4.1 scope', () => {
    expect(read('src_assets/common/assets/web/release-v140-contract.test.js')).toContain("describe('historical v1.4.0 release contract'")
    const gate = read('scripts/check-public-docs.sh')
    expect(gate).toContain('docs/release-notes/v1.4.1.md')
    expect(gate).toContain('"## v1.4.1 - 2026-09-03"')

    const release = `${currentChangelog()}\n${releaseNotes()}`
    for (const fact of [
      'Vulkan Video',
      'DRM/KMS',
      'guest pairing',
      'temporary_authorization',
      'Desktop Takeover',
      'live_bitrate_control',
      'Heroic',
      'seat isolation',
      'Nova v1.4.1',
      'Retroid Pocket 6',
    ]) {
      expect(release, `release must include: ${fact}`).toContain(fact)
    }
  })

  it('documents explicit Vulkan breadth and bounded automatic selection', () => {
    const notes = releaseNotes()
    const policy = read('src/platform/linux/encoder_auto_policy.h')
    const portal = read('src/platform/linux/portal_grab.cpp')

    expect(notes).toContain('DRM/KMS, wlroots, and Portal')
    expect(notes).toContain('NVIDIA remains on NVENC by default')
    expect(notes).toContain('Intel remains on VA-API')
    expect(notes).toContain('has not received physical AMD-host validation')
    expect(policy).toMatch(/kernel_driver == "amdgpu" && private_compositor_live_probe_available[\s\S]*fallback_encoder = "vaapi"/)
    expect(portal).toContain('make_avcodec_encode_device_ram')
  })

  it('keeps requested wlroots output selection exact and observable', () => {
    const wlgrab = read('src/platform/linux/wlgrab.cpp')
    const policyTests = read('tests/unit/test_wlgrab_capture_policy.cpp')

    expect(wlgrab).toContain('Selected monitor [')
    expect(wlgrab).toContain('selected_monitor_identity')
    expect(wlgrab).toContain('description=[')
    expect(policyTests).toContain('HostVirtualOutputDoesNotFallBackToPhysicalMonitorZero')
    expect(policyTests).toContain('POLARIS-HEADLESS-unknown')
  })

  it('pins all four install commands to v1.4.1 and preserves platform safety', () => {
    const blocks = installBlocks()
    expect(blocks).toHaveLength(4)
    for (const asset of expectedAssets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `one command block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.1/${asset}`)
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
    expect(listed.sort()).toEqual(expectedAssets)
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
