import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const sectionBetween = (source, startHeading, endHeading) => {
  const start = source.indexOf(startHeading)
  const end = source.indexOf(endHeading, start + startHeading.length)
  expect(start, `missing release heading: ${startHeading}`).toBeGreaterThanOrEqual(0)
  expect(end, `missing release boundary after ${startHeading}: ${endHeading}`).toBeGreaterThan(start)
  return source.slice(start, end)
}

const releaseSections = () => [
  sectionBetween(read('README.md'), '## What is New in v1.3.4', '## Install'),
  sectionBetween(read('docs/changelog.md'), '## v1.3.4 - 2026-07-31', '## v1.3.3'),
]

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
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.3.4 release contract', () => {
  it('moves the CMake version and public release headings together', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.3.4')
    expect(read('README.md')).toContain('## What is New in v1.3.4')
    expect(read('docs/changelog.md')).toContain('## v1.3.4 - 2026-07-31')
  })

  it('aligns the public checker with the v1.3.4 release contract', () => {
    const publicDocsGate = read('scripts/check-public-docs.sh')

    expect(publicDocsGate).toContain('"## What is New in v1.3.4"')
    expect(publicDocsGate).not.toContain('"## What is New in v1.3.3"')
    expect(publicDocsGate).toContain('"## v1.3.4 - 2026-07-31"')
    for (const fact of [...requiredFixFacts, 'npm audit --audit-level=high', 'webtransport-go v0.10.0']) {
      expect(publicDocsGate, `public checker must require: ${fact}`).toContain(fact)
    }
    for (const asset of expectedAssets) {
      expect(publicDocsGate, `public checker must require: ${asset}`).toContain(asset)
    }
  })

  it('records the #264, #265, and #266 release facts', () => {
    for (const releaseSection of releaseSections()) {
      for (const fact of requiredFixFacts) {
        expect(releaseSection, `release notes must include: ${fact}`).toContain(fact)
      }
    }
  })

  it('keeps npm audit mandatory and the forbidden webtransport-go version absent', () => {
    expect(read('.github/workflows/build.yml')).toContain('npm audit --audit-level=high')
    expect(read('browser_stream_helper/go.mod')).not.toContain('github.com/quic-go/webtransport-go v0.10.0')
    expect(read('browser_stream_helper/go.sum')).not.toContain('github.com/quic-go/webtransport-go v0.10.0')

    for (const releaseSection of releaseSections()) {
      expect(releaseSection).toContain('npm audit --audit-level=high')
      expect(releaseSection).toContain('webtransport-go v0.10.0')
    }
  })

  it('keeps the official release asset set exact', () => {
    for (const releaseSection of releaseSections()) {
      const actualAssets = releaseSection.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? []
      expect(actualAssets.sort()).toEqual(expectedAssets)
    }
  })
})
