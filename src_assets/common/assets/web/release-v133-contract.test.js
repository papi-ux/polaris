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
  '## v1.3.3 - 2026-07-30',
  '## v1.3.2',
)

describe('historical v1.3.3 release contract', () => {
  it('preserves the v1.3.3 changelog facts', () => {
    const release = historicalRelease()

    for (const phrase of ['response-only', 'controller feedback', 'seat isolation', 'Nix']) {
      expect(release).toContain(phrase)
    }
  })

  it('preserves the exact historical three-asset set', () => {
    const expectedAssets = [
      'Polaris-arch-x86_64.pkg.tar.zst',
      'Polaris-fedora44-x86_64.rpm',
      'Polaris-ubuntu24.04-x86_64.deb',
    ].sort()
    const actualAssets = historicalRelease().match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? []

    expect(actualAssets.sort()).toEqual(expectedAssets)
  })
})
