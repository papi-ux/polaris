import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const historicalRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.11 - 2026-09-19')
  const end = changelog.indexOf('## v1.4.10 - 2026-09-18')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const historicalNotes = () => read('docs/release-notes/v1.4.11.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const withdrawnSysextAsset = 'Polaris-sysext-x86_64.raw'

describe('historical v1.4.11 release contract', () => {
  it('leads with the KWin screen, Spaces and couch co-op, and says which Nova goes with it', () => {
    const notes = historicalNotes()
    const intro = notes.split('\n')[2]
    expect(intro).toMatch(/^Host Virtual Display on KDE Plasma gets the game, the controller and the touch it was missing/)
    expect(intro).toContain('Polaris 1.4.11 is matched with Nova 1.4.11')
    const order = [
      '**Host Virtual Display on KDE Plasma**',
      '**Spaces Across Driver Updates**',
      '**Desktop Streaming**',
      '**Couch Co-op and Controllers**',
      '**Heads up**',
    ].map((heading) => notes.indexOf(heading))
    expect(order.every((at) => at >= 0)).toBe(true)
    expect([...order].sort((a, b) => a - b)).toEqual(order)
    for (const fact of [
      "Plasma uses KWin's own screen by default, even with EVDI loaded",
      'the controller works from the first press',
      'Touch and pen from the client land on the stream screen',
      'Your monitors stay where they are when a stream starts',
      'Troubleshooting lists every host finding now',
      'NVIDIA driver 615.71.09 gets its own Spaces runtime',
      'moves to the runtime for the new driver',
      'Mirror Desktop streams again on KDE and GNOME',
      'no longer adds a controller of its own',
      'The Steam Controller (2026) gets an emulated DualSense',
    ]) {
      expect(notes, `v1.4.11 notes must include: ${fact}`).toContain(fact)
    }
    // KWin places an absolute pointer over the whole desktop, so the notes must not promise it.
    expect(notes).not.toMatch(/touch, pen and mouse/i)
  })

  it('names every change in the changelog section', () => {
    const section = historicalRelease()
    for (const fact of [
      "uses KWin's own screen by default, even with EVDI loaded",
      'gives the game the focus on the stream screen',
      'Touch and pen from the client land on the KWin stream screen',
      'keeps a tie you made yourself in System Settings',
      'Your monitors stay where they are',
      'The Doctor names three Host Virtual Display problems',
      'Spaces work with NVIDIA driver 615.71.09',
      'can move to the runtime for the new driver',
      'Spaces survive a reboot that renumbers the graphics devices',
      'A desktop stream uses the capture backend Polaris found',
      'capture_backend_unavailable',
      'no longer adds a controller to the host',
      'The Steam Controller (2026) gets an emulated DualSense',
      'names every player on the host',
    ]) {
      expect(section, `v1.4.11 changelog must include: ${fact}`).toContain(fact)
    }
  })

  it('keeps the heads up honest about driver updates, the Steam Controller, KMS and packaging', () => {
    const notes = historicalNotes()
    const headsUp = notes.slice(notes.indexOf('**Heads up**'))
    for (const fact of [
      'move each Space to the runtime for the new driver',
      'back grip buttons are not passed on yet',
      'removes the KMS capture permission',
      'sudo -H polaris --setup-host --enable-kms',
      'experimental Desktop Mode support',
      'system extension stays withdrawn',
    ]) {
      expect(headsUp, `v1.4.11 heads up must include: ${fact}`).toContain(fact)
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
      expect(matches[0]).toContain(`releases/download/v1.4.11/${asset}`)
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
