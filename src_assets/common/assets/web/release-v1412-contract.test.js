import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const historicalRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.12 - 2026-09-22')
  const end = changelog.indexOf('## v1.4.11 - 2026-09-19')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const historicalNotes = () => read('docs/release-notes/v1.4.12.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const withdrawnSysextAsset = 'Polaris-sysext-x86_64.raw'

describe('historical v1.4.12 release contract', () => {
  it('leads with the Steam Deck, launcher Spaces and Watch, and says which Nova goes with it', () => {
    const notes = historicalNotes()
    const intro = notes.split('\n')[2]
    expect(intro).toMatch(/^Your Steam Deck is a streaming host now, Game Mode and all/)
    expect(intro).toContain('Polaris 1.4.12 is matched with Nova 1.4.12')
    expect(intro).toContain('Nova to the Steam Deck as an Alpha')
    const order = [
      '**Steam Deck and Steam Game Mode**',
      '**Spaces for Heroic and Lutris**',
      '**Watch Stream**',
      '**Gamescope**',
      '**Fixed**',
      '**Heads up**',
    ].map((heading) => notes.indexOf(heading))
    expect(order.every((at) => at >= 0)).toBe(true)
    expect([...order].sort((a, b) => a - b)).toEqual(order)
    for (const fact of [
      'A host in Steam Game Mode streams its Game Mode screen',
      'a title that is already open is joined instead of launched twice',
      'A controller arrives as a DualSense',
      'touch lands where you aim it',
      'End Session closes only the title the stream opened',
      'Proven on a Steam Deck OLED on SteamOS 3.8.16',
      'sudo -H polaris --setup-host --enable-headless-boot',
      'A Space can run Heroic Games Launcher',
      'Only the first Space of a launcher downloads its runtime',
      "a Space borrows this PC's own driver",
      'Device Access is one table',
      'A second device can watch the running stream whatever resolution it would ask for',
      'Mouse and keyboard reach a game under Gamescope Stream',
      'A Space stream no longer sends a keyframe twice a second',
    ]) {
      expect(notes, `v1.4.12 notes must include: ${fact}`).toContain(fact)
    }
  })

  it('names every change in the changelog section', () => {
    const section = historicalRelease()
    for (const fact of [
      'A host in Steam Game Mode streams the Game Mode screen',
      'A Steam title launched from a client on a Game Mode host opens in Game Mode',
      'Touch lands where it is aimed in Game Mode on a Steam Deck',
      'The Desktop tile opens on a Game Mode host right after Polaris restarts',
      "borrows this PC's own driver files",
      'A Space can run Heroic Games Launcher or Lutris, beside Steam',
      'A Space is made by naming its launcher',
      'A launcher window fills the stream in a Space',
      'A Space stream no longer sends a keyframe every half second',
      'A device can watch a stream whatever resolution it would ask for',
      'Device Access on the Spaces page is one table',
      'no longer exits when it restarts its own gamescope (#744)',
    ]) {
      expect(section, `v1.4.12 changelog must include: ${fact}`).toContain(fact)
    }
  })

  it('keeps the heads up honest about runtimes, Heroic, SteamOS, KMS and packaging', () => {
    const notes = historicalNotes()
    const headsUp = notes.slice(notes.indexOf('**Heads up**'))
    for (const fact of [
      'A Space keeps its runtime until you move it',
      "Heroic's own menus do not see a controller in a Space yet",
      'the Deck LCD, suspend and resume, and SteamOS updates are not certified yet',
      'removes the KMS capture permission',
      'sudo -H polaris --setup-host --enable-kms',
      'Fedora 44 RPM through rpm-ostree',
    ]) {
      expect(headsUp, `v1.4.12 heads up must include: ${fact}`).toContain(fact)
    }
    // SteamOS Game Mode is proven now; the notes must not keep the old experimental line.
    expect(notes).not.toContain('rather than certified Game Mode support')
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
      expect(matches[0]).toContain(`releases/download/v1.4.12/${asset}`)
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
