import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const sectionBetween = (source, startHeading, endHeading) => {
  const start = source.indexOf(startHeading)
  const end = source.indexOf(endHeading, start + startHeading.length)
  return source.slice(start, end)
}

describe('v1.3.3 release contract', () => {
  it('moves version and public release notes together', () => {
    const cmake = read('CMakeLists.txt')
    const readme = read('README.md')
    const changelog = read('docs/changelog.md')
    const publicDocsGate = read('scripts/check-public-docs.sh')

    expect(cmake).toContain('project(Polaris VERSION 1.3.3')
    expect(readme).toContain('## What is New in v1.3.3')
    expect(publicDocsGate).toContain('"## What is New in v1.3.3"')
    expect(publicDocsGate).not.toContain('"## What is New in v1.3.2"')
    expect(changelog.indexOf('## v1.3.3 - 2026-07-30')).toBeGreaterThanOrEqual(0)
    expect(changelog.indexOf('## v1.3.3 - 2026-07-30')).toBeLessThan(changelog.indexOf('## v1.3.2'))

    for (const phrase of ['response-only', 'controller feedback', 'seat isolation', 'Nix']) {
      expect(`${readme}\n${changelog}`).toContain(phrase)
    }
  })

  it('keeps the official release asset set exact', () => {
    const expectedAssets = [
      'Polaris-arch-x86_64.pkg.tar.zst',
      'Polaris-fedora44-x86_64.rpm',
      'Polaris-ubuntu24.04-x86_64.deb',
    ].sort()
    const releaseSections = [
      sectionBetween(read('README.md'), '## What is New in v1.3.3', '## Install'),
      sectionBetween(read('docs/changelog.md'), '## v1.3.3 - 2026-07-30', '## v1.3.2'),
    ]

    for (const section of releaseSections) {
      const actualAssets = section.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? []
      expect(actualAssets.sort()).toEqual(expectedAssets)
    }
  })
})
