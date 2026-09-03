import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const historicalRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.0 - 2026-09-01')
  const end = changelog.indexOf('## v1.3.13 - 2026-08-22')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const historicalNotes = () => read('docs/release-notes/v1.4.0.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('historical v1.4.0 release contract', () => {
  it('preserves the evidence-first deterministic launch scope', () => {
    const evidence = `${historicalRelease()}\n${historicalNotes()}`
    for (const fact of [
      'frame-pacing',
      'apply_recovery_profile_next_launch',
      'deterministic',
      'Doctor v2',
      'Steam Input',
      'Heroic',
      'Nova v1.4.0',
    ]) {
      expect(evidence, `historical v1.4.0 must include: ${fact}`).toContain(fact)
    }
  })

  it('preserves the exact historical assets and install identities', () => {
    const notes = historicalNotes()
    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)

    const blocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(4)
    for (const block of blocks) {
      expect(block).toContain('releases/download/v1.4.0/')
      expect(block).toContain('polaris --setup-host')
    }
  })
})
