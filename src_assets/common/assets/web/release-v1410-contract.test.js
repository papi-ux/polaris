import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const currentRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.10 - 2026-09-18')
  const end = changelog.indexOf('## v1.4.9 - 2026-09-16')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const currentNotes = () => read('docs/release-notes/v1.4.10.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const withdrawnSysextAsset = 'Polaris-sysext-x86_64.raw'

describe('v1.4.10 release contract', () => {
  it('pins the version every packaging surface agrees on', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.10')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.10"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain(
      'usr/bin/polaris-1.4.10',
    )
    expect(read('scripts/ci/build-steamos-package.sh')).toContain("'polaris|1.4.10-1|x86_64'")
  })

  it('leads with Spaces, host sleep and the KWin screen, and says which Nova goes with it', () => {
    const notes = currentNotes()
    const intro = notes.split('\n')[2]
    expect(intro).toMatch(/^Spaces open up, the couch can put your host to sleep/)
    expect(intro).toContain('This is the first Polaris that can create a Space')
    expect(intro).toContain('Nova 1.4.10 comes out alongside it')
    const order = [
      '**Spaces Are Open**',
      '**Sleep From the Couch**',
      '**A Screen of Its Own on KDE Plasma**',
      '**Your Library, Sorted**',
      '**Easier to Get Help**',
      '**Fixed**',
      '**Heads up**',
    ].map((heading) => notes.indexOf(heading))
    expect(order.every((at) => at >= 0)).toBe(true)
    expect([...order].sort((a, b) => a - b)).toEqual(order)
    for (const fact of [
      'Create a Space on a released Polaris for the first time',
      'nobody has run it on that hardware yet',
      'Allow Clients To Sleep This Host',
      'Nova no longer says it is asleep when it stayed awake',
      'Host Virtual Display on KDE Plasma 6 gets a brand new screen for each stream',
      'your desktop, wallpaper and panel stay put',
      'Find Cover works again',
      'Completion estimates are back',
      'installs from Flathub with one click',
      'no longer include network addresses',
      'The right-click menu on an empty Private Stream screen can be read',
      'before starting any thread',
    ]) {
      expect(notes, `v1.4.10 notes must include: ${fact}`).toContain(fact)
    }
  })

  it('names every change in the changelog section', () => {
    const section = currentRelease()
    for (const fact of [
      'A Space can be created on a released Polaris',
      'gets a screen of its own from KWin',
      'before it starts any thread',
      'A paired client can put the host to sleep',
      'Host sleep reports whether the host actually slept',
      'Before you turn it on',
      'no longer carry network addresses',
      'Find Cover in the app editor works again',
      'Completion estimates resolve again',
      'shows on the Apps and Dashboard pages at once',
      'can be installed from the ROM folders panel',
      'The right-click menu on an empty Private Stream screen can be read',
    ]) {
      expect(section, `v1.4.10 changelog must include: ${fact}`).toContain(fact)
    }
  })

  it('keeps the heads up honest about sleep, KMS, KWin and what Spaces still lacks', () => {
    const notes = currentNotes()
    const headsUp = notes.slice(notes.indexOf('**Heads up**'))
    for (const fact of [
      'Host sleep is off by default',
      'removes the KMS capture permission',
      'sudo -H polaris --setup-host --enable-kms',
      'a KWin screen carries no HDR',
      'driver 610.57.04',
      'one Space runs at a time',
      'experimental Desktop Mode support',
      'system extension stays withdrawn',
    ]) {
      expect(headsUp, `v1.4.10 heads up must include: ${fact}`).toContain(fact)
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
      expect(matches[0]).toContain(`releases/download/v1.4.10/${asset}`)
    }

    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    const listed = [...new Set((assetsLine ?? '').match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
    expect(assetsLine).not.toContain(withdrawnSysextAsset)
  })
})
