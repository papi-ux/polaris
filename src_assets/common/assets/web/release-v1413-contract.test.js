import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const currentNotes = () => read('docs/release-notes/v1.4.13.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.4.13 release contract', () => {
  // These four are what the source says it builds, not what the last release was. Two of them are
  // only read during a package build, so a mismatch surfaces as a failed SteamOS job rather than as
  // a failed test, which is why they are pinned here where a local run sees them.
  it('pins the version every packaging surface agrees on', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.13')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.13"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain(
      'usr/bin/polaris-1.4.13',
    )
    expect(read('scripts/ci/build-steamos-package.sh')).toContain("'polaris|1.4.13-1|x86_64'")
  })

  // The release workflow refuses a tag whose notes file is missing or empty, and a beta reads the
  // notes of the release it precedes, so this file has to exist from the moment the version opens.
  it('has curated notes a beta can publish', () => {
    const notes = currentNotes()
    expect(notes.split('\n')[0]).toBe('# Polaris v1.4.13')
    expect(notes.trim().length).toBeGreaterThan(0)
  })

  it('ships exactly the four supported packages and installs them from this tag', () => {
    const notes = currentNotes()
    for (const asset of expectedAssets) {
      expect(notes).toContain(asset)
      expect(notes).toContain(
        `https://github.com/papi-ux/polaris/releases/download/v1.4.13/${asset}`,
      )
    }
    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    for (const asset of expectedAssets) {
      expect(assetsLine).toContain(asset)
    }
  })

  // Until this release ships, its notes are read by beta testers, whose packages are attached to the
  // prerelease rather than to the tag the install commands name.
  it('tells a beta reader which packages are theirs', () => {
    expect(currentNotes()).toContain(
      'While 1.4.13 is in beta, use the packages attached to the prerelease you are reading.',
    )
  })

  it('keeps the install heads up about the KMS capture permission', () => {
    expect(currentNotes()).toContain('removes the KMS capture permission')
    expect(currentNotes()).toContain('sudo -H polaris --setup-host --enable-kms')
  })
})
