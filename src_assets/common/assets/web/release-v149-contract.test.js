import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const currentRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.9 - 2026-09-16')
  const end = changelog.indexOf('## v1.4.8 - 2026-09-16')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const currentNotes = () => read('docs/release-notes/v1.4.9.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const withdrawnSysextAsset = 'Polaris-sysext-x86_64.raw'

describe('v1.4.9 release contract', () => {
  it('pins the version every packaging surface agrees on', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.9')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.9"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain(
      'usr/bin/polaris-1.4.9',
    )
    expect(read('scripts/ci/build-steamos-package.sh')).toContain("'polaris|1.4.9-1|x86_64'")
  })

  it('leads with what a fresh install changed and says Nova needs no update', () => {
    const notes = currentNotes()
    const intro = notes.split('\n')[2]
    expect(intro).toMatch(/^A setup update shaped by a fresh install/)
    expect(intro).toContain('Nova stays at v1.4.8 and needs no update')
    expect(notes.indexOf('**New**')).toBeLessThan(notes.indexOf('**Fixed**'))
    expect(notes.indexOf('**Fixed**')).toBeLessThan(notes.indexOf('**Heads up**'))
    for (const fact of [
      'First-time setup has two optional steps',
      'takes effect right away',
      'the model Codex is set to use is the default',
      'uninstall guide',
      'Restart from the console or the tray works again',
      'show the Polaris icon',
      'left in /etc',
      'cannot be created on this version yet',
      'New installs start in Private Stream',
      'can be removed for good',
      'can trust your home network',
      'Artwork alternatives in Nova work',
      'checks the gaming runtime this PC needs',
      "no longer borrows another game's pictures",
    ]) {
      expect(notes, `v1.4.9 notes must include: ${fact}`).toContain(fact)
    }
  })

  it('names every fix in the changelog section', () => {
    const section = currentRelease()
    for (const fact of [
      'The first-run wizard gains two optional steps',
      'follow the Codex CLI',
      'Restart from the console and the tray works again',
      'takes effect while Polaris runs',
      'every version Polaris shipped',
      '--setup-host --enable-kms',
      'one per binary',
      'reads as ready and waiting',
      'uninstall page',
      'Removing a Space can now delete it for good',
      "Nova's artwork alternatives work",
      'Quick setup has eight steps',
      'has a Gaming runtime check',
      'Spaces turned on from the Spaces page can start and change',
      'marks Steam Big Picture on the desktop',
      'Automatic artwork only takes a match that is the entry',
    ]) {
      expect(section, `v1.4.9 changelog must include: ${fact}`).toContain(fact)
    }
  })

  it('keeps the heads up honest about KMS after updates and what Spaces still lacks', () => {
    const notes = currentNotes()
    for (const fact of [
      'removes the KMS capture permission',
      'sudo -H polaris --setup-host --enable-kms',
      'still not available',
      'experimental Desktop Mode support',
      'Steam Input stays manual and read-only',
      'system extension stays withdrawn',
    ]) {
      expect(notes, `v1.4.9 heads up must include: ${fact}`).toContain(fact)
    }
    expect(notes).not.toContain(withdrawnSysextAsset)
  })

  it('ships exactly the four supported packages and installs them from this tag', () => {
    const notes = currentNotes()
    const blocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(4)
    for (const asset of expectedAssets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `one command block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.9/${asset}`)
    }

    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    const listed = [...new Set((assetsLine ?? '').match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
    expect(assetsLine).not.toContain(withdrawnSysextAsset)
  })

  it('closes the changelog section with the exact four-asset sentence and no stray blank line', () => {
    const lines = currentRelease().trimEnd().split('\n')
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
