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

const releaseSection = () =>
  sectionBetween(read('docs/changelog.md'), '## v1.3.8 - 2026-08-12', '## v1.3.7')

const requiredFacts = [
  'true-headless',
  'GPU-native',
  'streamMode',
  'PIDFD',
  'Doctor',
  '8 MiB',
  'npm audit --audit-level=high',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.3.8 release contract', () => {
  it('moves the CMake version and authoritative changelog heading together', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.3.8')
    expect(read('README.md')).not.toMatch(/^#{1,6}\s+.*(?:release|what is new).*v\d/im)
    expect(read('docs/changelog.md')).toContain('## v1.3.8 - 2026-08-12')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.3.8"')
  })

  it('states release facts in the authoritative changelog', () => {
    for (const fact of requiredFacts) {
      expect(releaseSection(), `release summary must include: ${fact}`).toContain(fact)
    }
  })

  it('aligns the public checker with the v1.3.8 release contract', () => {
    const publicDocsGate = read('scripts/check-public-docs.sh')

    expect(publicDocsGate).toContain('README must not duplicate a version-specific release section')
    expect(publicDocsGate).not.toContain('"## What is New in v1.3.8"')
    expect(publicDocsGate).toContain('"## v1.3.8 - 2026-08-12"')
    for (const asset of expectedAssets) {
      expect(publicDocsGate, `public checker must require: ${asset}`).toContain(asset)
    }
  })

  it('moves versioned package-review metadata to v1.3.8', () => {
    const reviewedWarnings = read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')
    const steamOsBuildScript = read('scripts/ci/build-steamos-package.sh')
    const packagingContract = read('src_assets/common/assets/web/packaging-contracts.test.js')

    expect(reviewedWarnings).toContain('usr/bin/polaris-1.3.8')
    expect(reviewedWarnings).not.toContain('usr/bin/polaris-1.3.7')
    expect(steamOsBuildScript).toContain("'polaris|1.3.8-1|x86_64'")
    expect(steamOsBuildScript).not.toContain("'polaris|1.3.7-1|x86_64'")
    expect(packagingContract).toContain("'polaris|1.3.8-1|x86_64'")
  })

  it('ships release notes whose install commands target v1.3.8', () => {
    const releaseNotes = read('docs/release-notes/v1.3.8.md')
    const benchmarkOpenApi = read('docs/benchmark-control-openapi.json')

    expect(benchmarkOpenApi).toContain('"collector_version": "1.3.8"')
    expect(benchmarkOpenApi).not.toContain('"collector_version": "1.3.7"')
    for (const fact of ['v1.3.7', 'true-headless', 'streamMode', 'PIDFD', 'Doctor', '8 MiB']) {
      expect(releaseNotes, `release notes must include: ${fact}`).toContain(fact)
    }

    const installBlocks = [...releaseNotes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))

    expect(installBlocks.length).toBe(3)
    for (const block of installBlocks) {
      expect(block).toContain('releases/download/v1.3.8/')
      expect(block).not.toContain('releases/download/v1.3.7/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }
  })

  it('keeps the exact four-asset set in the release notes', () => {
    const notes = read('docs/release-notes/v1.3.8.md')
    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]

    expect(listed.sort()).toEqual(expectedAssets)
  })
})
