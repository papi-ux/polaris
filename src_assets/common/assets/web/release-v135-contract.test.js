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
  '## v1.3.5 - 2026-08-06',
  '## v1.3.4',
)

const requiredFixFacts = [
  'exact package filename',
  'wget --output-document',
  'short-circuit',
  'sudo -H polaris --setup-host',
  '/usr/lib',
  '/etc',
  'polaris-debug',
  'input',
  'playtime',
  'npm audit --audit-level=high',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('historical v1.3.5 release contract', () => {
  it('preserves the v1.3.5 release facts', () => {
    const release = historicalRelease()

    for (const fact of requiredFixFacts) {
      expect(release, `historical release must include: ${fact}`).toContain(fact)
    }
  })

  it('preserves the exact historical four-asset set', () => {
    const actualAssets = historicalRelease().match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? []

    expect(actualAssets.sort()).toEqual(expectedAssets)
  })
})
