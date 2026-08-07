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
  sectionBetween(read('README.md'), '## What is New in v1.3.6', '## Install'),
  sectionBetween(read('docs/changelog.md'), '## v1.3.6 - 2026-08-07', '## v1.3.5'),
]

const requiredFacts = [
  'ostree',
  'usermod',
  'ujust add-user-to-input-group',
  'npm audit --audit-level=high',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.3.6 release contract', () => {
  it('moves the CMake version and public release headings together', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.3.6')
    expect(read('README.md')).toContain('## What is New in v1.3.6')
    expect(read('docs/changelog.md')).toContain('## v1.3.6 - 2026-08-07')
  })

  it('states the release facts in both public summaries', () => {
    for (const releaseSection of releaseSections()) {
      for (const fact of requiredFacts) {
        expect(releaseSection, `release summary must include: ${fact}`).toContain(fact)
      }
    }
  })

  it('aligns the public checker with the v1.3.6 release contract', () => {
    const publicDocsGate = read('scripts/check-public-docs.sh')

    expect(publicDocsGate).toContain('"## What is New in v1.3.6"')
    expect(publicDocsGate).not.toContain('"## What is New in v1.3.5"')
    expect(publicDocsGate).toContain('"## v1.3.6 - 2026-08-07"')
    for (const asset of expectedAssets) {
      expect(publicDocsGate, `public checker must require: ${asset}`).toContain(asset)
    }
  })

  it('moves versioned package-review metadata to v1.3.6', () => {
    const reviewedWarnings = read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')
    const steamOsBuildScript = read('scripts/ci/build-steamos-package.sh')

    expect(reviewedWarnings).toContain('usr/bin/polaris-1.3.6')
    expect(reviewedWarnings).not.toContain('usr/bin/polaris-1.3.5')
    expect(steamOsBuildScript).toContain("'polaris|1.3.6-1|x86_64'")
    expect(steamOsBuildScript).not.toContain("'polaris|1.3.5-1|x86_64'")
  })

  it('ships release notes whose install commands target v1.3.6', () => {
    const releaseNotes = read('docs/release-notes/v1.3.6.md')

    // The upgrade section has to name the release being upgraded from, and the
    // ostree remedy, because following the v1.3.5 advice on Bazzite did nothing.
    for (const fact of ['v1.3.5', 'ostree', 'ujust add-user-to-input-group']) {
      expect(releaseNotes, `release notes must include: ${fact}`).toContain(fact)
    }

    // Install blocks only. The property is that documented install commands
    // point at this release; other examples in the notes are not install
    // commands and must not be forced to look like them.
    const installBlocks = [...releaseNotes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))

    expect(installBlocks.length).toBe(3)
    for (const block of installBlocks) {
      expect(block).toContain('releases/download/v1.3.6/')
      expect(block).not.toContain('releases/download/v1.3.5/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }
  })

  it('keeps the exact four-asset set in the release notes', () => {
    const notes = read('docs/release-notes/v1.3.6.md')
    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]

    expect(listed.sort()).toEqual(expectedAssets)
  })
})
