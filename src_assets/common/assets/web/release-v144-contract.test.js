import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const historicalRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.4 - 2026-09-06')
  const end = changelog.indexOf('## v1.4.3 - 2026-09-05')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const historicalNotes = () => read('docs/release-notes/v1.4.4.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const withdrawnSysextAsset = 'Polaris-sysext-x86_64.raw'

describe('historical v1.4.4 release contract', () => {
  it('preserves the service-first menu entry, fail-closed setup, and withdrawn extension scope', () => {
    const evidence = `${historicalRelease()}\n${historicalNotes()}`
    for (const fact of [
      'application menu',
      'user service',
      'headless boot',
      'setup-host',
      'input group',
      'Bazzite',
      'Nova v1.4.4',
      'Retroid Pocket 6',
    ]) {
      expect(evidence, `historical v1.4.4 must include: ${fact}`).toContain(fact)
    }
    expect(historicalNotes()).toContain('system extension stays withdrawn')
    expect(historicalNotes()).not.toContain(withdrawnSysextAsset)
  })

  it('preserves the exact historical assets and install identities', () => {
    const notes = historicalNotes()
    const blocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(4)
    for (const asset of expectedAssets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `one command block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.4/${asset}`)
    }

    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    const listed = [...new Set((assetsLine ?? '').match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
    expect(assetsLine).not.toContain(withdrawnSysextAsset)
  })
})
