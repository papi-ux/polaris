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
  '## v1.3.4 - 2026-07-31',
  '## v1.3.3',
)

const requiredFixFacts = [
  'fail-closed',
  'packaged binary path',
  'source-prefix',
  'locale-safe',
  'ImageMagick',
  'secure',
  'Bazzite',
  '/home',
  'var/home',
  'without broad canonicalization',
  'sudo -H',
  'SteamOS 3.8',
  'Desktop Mode',
  'npm audit --audit-level=high',
  'webtransport-go v0.10.0',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('historical v1.3.4 release contract', () => {
  it('preserves the v1.3.4 release facts', () => {
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
