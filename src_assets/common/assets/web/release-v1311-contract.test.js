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
  '## v1.3.11 - 2026-08-19',
  '## v1.3.10 - 2026-08-16',
)

const historicalNotes = () => read('docs/release-notes/v1.3.11.md')

const requiredFacts = [
  'Flatpak',
  'Heroic',
  'Lutris',
  'headless_dongle',
  'session_overridable',
  'Hyprland',
  'client report',
  'session_id',
  'Map',
  'Set',
  'Date',
  'Error',
  '2026/08/17',
  '20260818T000000Z',
  'POLARIS_PRIVATE_SESSION',
  'polaris-gamescope-session',
  'gamescope-0',
  'HDR',
  'empty pairing',
  'POLARIS_PORTAL_DMABUF',
  'Matrix',
  'npm audit --audit-level=high',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('historical v1.3.11 release contract', () => {
  it('preserves the shipped v1.3.11 facts', () => {
    const release = historicalRelease()
    for (const fact of requiredFacts) {
      expect(release, `historical release must include: ${fact}`).toContain(fact)
    }
  })

  it('preserves the exact historical asset set', () => {
    const assets = [...new Set(historicalNotes().match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(assets.sort()).toEqual(expectedAssets)
  })

  it('preserves historical install commands without owning the current version', () => {
    const blocks = [...historicalNotes().matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))

    expect(blocks).toHaveLength(3)
    for (const block of blocks) {
      expect(block).toContain('releases/download/v1.3.11/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }
  })

  it('preserves the v1.3.11 support and validation boundaries', () => {
    const notes = historicalNotes()
    expect(notes).toContain('POST /polaris/v1/support/client-report')
    expect(notes).toContain('does not yet automatically post to this host endpoint')
    expect(notes).toContain('must not be described as automatic paired reporting')
    expect(notes).toContain('session_id')
    expect(notes).toContain('client_id')
    expect(notes).toContain('user_id')
    expect(notes).toContain('app_id')
    expect(notes).toContain('non-string scalars unchanged unless their field name is sensitive')
    expect(notes).toContain('preserving measurements such as bitrate, frame rate, packet loss, port, and GPU index')
    expect(notes).toContain('Keeps PipeWire VAAPI capture on SHM by default')
    expect(notes).toContain('POLARIS_PORTAL_DMABUF=1')
    expect(notes).toContain('affected RX 7800 XT has not physically tested this exact candidate')
    expect(notes).toContain('does not claim that the underlying DMA-BUF')
    expect(notes).not.toContain('containment remains in draft PR #481')
  })
})
