import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const historicalRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.8 - 2026-09-16')
  const end = changelog.indexOf('## v1.4.7 - 2026-09-12')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const historicalNotes = () => read('docs/release-notes/v1.4.8.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const withdrawnSysextAsset = 'Polaris-sysext-x86_64.raw'

describe('historical v1.4.8 release contract', () => {
  it('headlines Spaces as a preview and says what it cannot do yet in the first sentence', () => {
    const notes = historicalNotes()
    const intro = notes.split('\n')[2]
    expect(intro).toMatch(/^Spaces arrives as an early preview/)
    expect(intro).toContain('configured hosts only')
    expect(intro).toContain('without a public runtime download yet')
    expect(notes.indexOf('**Spaces, an early preview**')).toBeLessThan(
      notes.indexOf('**More games, less setup**'),
    )
    for (const fact of [
      'public Docker runtime download is not available yet',
      'one active Space',
      'handheld audio remains under investigation',
      'removing a Space retains its games and saves',
      'Ordinary streaming keeps its familiar setup',
      'One set of words everywhere',
      'The host says why',
      'speaks the console',
    ]) {
      expect(notes, `v1.4.8 preview limits must include: ${fact}`).toContain(fact)
    }
    expect(historicalRelease()).toContain('Spaces preview')
    expect(historicalRelease()).toContain('First runtime download remains unavailable')
    expect(historicalRelease()).toContain('A refused Space launch now says why')
    expect(historicalRelease()).toContain("The Spaces console speaks the console's grammar")
  })

  it('says what the emulator library does, in the words a player would use', () => {
    const evidence = `${historicalRelease()}\n${historicalNotes()}`
    for (const fact of [
      'ROM folder',
      'covers',
      'ES-DE',
      'RetroArch',
      'Steam Big Picture',
      'emulators guide',
      'Exit Timeout',
      '240 FPS',
      'Ubuntu 24.04',
      'A refused launch',
      'Doctor',
    ]) {
      expect(evidence, `v1.4.8 must include: ${fact}`).toContain(fact)
    }
  })

  it('keeps the heads up honest about behaviour changes and what was not validated', () => {
    const notes = historicalNotes()
    for (const fact of [
      'split the way a shell would before it runs',
      'at least two seconds and at most thirty',
      'built with CUDA',
      'targets, not performance guarantees',
      'experimental Desktop Mode support',
      'Steam Input stays manual and read-only',
      'system extension stays withdrawn',
    ]) {
      expect(notes, `v1.4.8 heads up must include: ${fact}`).toContain(fact)
    }
    expect(notes).not.toContain(withdrawnSysextAsset)
  })

  it('ships exactly the four supported packages and installs them from this tag', () => {
    const notes = historicalNotes()
    const blocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(4)
    for (const asset of expectedAssets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `one command block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.8/${asset}`)
    }

    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    const listed = [...new Set((assetsLine ?? '').match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
    expect(assetsLine).not.toContain(withdrawnSysextAsset)
  })

  it('closes the changelog section with the exact four-asset sentence and no stray blank line', () => {
    const lines = historicalRelease().trimEnd().split('\n')
    const bullets = lines.filter((line) => line.startsWith('- '))
    expect(bullets.at(-1)).toContain(
      'Keeps exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, ' +
        '`Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb` ' +
        'as the official package assets',
    )
    const firstBullet = lines.findIndex((line) => line.startsWith('- '))
    expect(lines.slice(firstBullet).every((line) => line.startsWith('- '))).toBe(true)
  })
})
