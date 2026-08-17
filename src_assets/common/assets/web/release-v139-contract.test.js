import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const sectionBetween = (source, startHeading, endHeading) => {
  const start = source.indexOf(startHeading)
  const end = source.indexOf(endHeading, start + startHeading.length)
  expect(start, `missing historical release heading: ${startHeading}`).toBeGreaterThanOrEqual(0)
  expect(end, `missing historical release boundary after ${startHeading}: ${endHeading}`).toBeGreaterThan(start)
  return source.slice(start, end)
}

const historicalRelease = () => sectionBetween(
  read('docs/changelog.md'),
  '## v1.3.9 - 2026-08-15',
  '## v1.3.8 - 2026-08-12',
)

const historicalReleaseNotes = () => read('docs/release-notes/v1.3.9.md')

const requiredFixFacts = [
  'DRM_FORMAT_MOD_LINEAR',
  'xdg-desktop-portal',
  'Gamescope',
  'Hyprland',
  'Doctor',
  'high-refresh',
  'npm audit --audit-level=high',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('historical v1.3.9 release contract', () => {
  it('preserves the v1.3.9 release facts', () => {
    const release = historicalRelease()

    for (const fact of requiredFixFacts) {
      expect(release, `historical release must include: ${fact}`).toContain(fact)
    }
  })

  it('preserves the exact historical four-asset set', () => {
    const actualAssets = historicalRelease().match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? []

    expect(actualAssets.sort()).toEqual(expectedAssets)
  })

  it('preserves the v1.3.9 install commands and lifecycle steps', () => {
    const notes = historicalReleaseNotes()
    const installBlocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))

    expect(installBlocks.length).toBe(3)
    for (const block of installBlocks) {
      expect(block).toContain('releases/download/v1.3.9/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }
    for (const asset of expectedAssets.filter((asset) => !asset.includes('steamos'))) {
      expect(notes).toContain(asset)
    }
  })
})
