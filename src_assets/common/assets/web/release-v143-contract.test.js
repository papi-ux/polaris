import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const historicalRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.3 - 2026-09-05')
  const end = changelog.indexOf('## v1.4.2 - 2026-09-04')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const historicalNotes = () => read('docs/release-notes/v1.4.3.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const withdrawnSysextAsset = 'Polaris-sysext-x86_64.raw'

describe('historical v1.4.3 release contract', () => {
  it('preserves the diagnostics, merge-write, and system-extension withdrawal scope', () => {
    const evidence = `${historicalRelease()}\n${historicalNotes()}`
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
      expect(evidence, `historical v1.4.3 must include: ${fact}`).toContain(fact)
    }
  })

  it('preserves the withdrawal warning and the exact historical assets and install identities', () => {
    const notes = historicalNotes()
    expect(notes).toContain('Bazzite system extension withdrawn on September 5, 2026')
    expect(notes).toContain('Do not use cached copies')
    expect(notes).toContain(withdrawnSysextAsset)

    const blocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(4)
    for (const asset of expectedAssets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `one command block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.3/${asset}`)
    }

    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    const listed = [...new Set((assetsLine ?? '').match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
    expect(assetsLine).not.toContain(withdrawnSysextAsset)
  })
})
