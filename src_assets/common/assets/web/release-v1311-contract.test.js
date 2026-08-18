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

const releaseSection = () => sectionBetween(
  read('docs/changelog.md'),
  '## v1.3.11 - 2026-08-18',
  '## v1.3.10 - 2026-08-16',
)

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
  'npm audit --audit-level=high',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.3.11 release contract', () => {
  it('moves the versioned release metadata together', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.3.11')
    expect(read('README.md')).not.toMatch(/^#{1,6}\s+.*(?:release|what is new).*v\d/im)
    expect(read('docs/changelog.md')).toContain('## v1.3.11 - 2026-08-18')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.3.11"')
  })

  it('states the shipped facts in the authoritative changelog', () => {
    for (const fact of requiredFacts) {
      expect(releaseSection(), `release summary must include: ${fact}`).toContain(fact)
    }
  })

  it('aligns the public checker with the v1.3.11 release contract', () => {
    const publicDocsGate = read('scripts/check-public-docs.sh')

    expect(publicDocsGate).toContain('README must not duplicate a version-specific release section')
    expect(publicDocsGate).toContain('"## v1.3.11 - 2026-08-18"')
    expect(publicDocsGate).toContain('docs/release-notes/v1.3.11.md')
    for (const asset of expectedAssets) {
      expect(publicDocsGate, `public checker must require: ${asset}`).toContain(asset)
    }
  })

  it('moves package-review metadata and identity to v1.3.11', () => {
    const reviewedWarnings = read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')
    const steamOsBuildScript = read('scripts/ci/build-steamos-package.sh')
    const packagingContract = read('src_assets/common/assets/web/packaging-contracts.test.js')

    expect(reviewedWarnings).toContain('usr/bin/polaris-1.3.11')
    expect(reviewedWarnings).not.toContain('usr/bin/polaris-1.3.10')
    expect(steamOsBuildScript).toContain("'polaris|1.3.11-1|x86_64'")
    expect(steamOsBuildScript).not.toContain("'polaris|1.3.10-1|x86_64'")
    expect(packagingContract).toContain("'polaris|1.3.11-1|x86_64'")
  })

  it('ships release notes whose install commands target v1.3.11', () => {
    const notes = read('docs/release-notes/v1.3.11.md')
    const installBlocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))

    expect(installBlocks.length).toBe(3)
    for (const block of installBlocks) {
      expect(block).toContain('releases/download/v1.3.11/')
      expect(block).not.toContain('releases/download/v1.3.10/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }
  })

  it('pins the immutable Arch snapshot rather than mirror retry behavior', () => {
    const workflow = read('.github/workflows/build.yml')

    expect(workflow).toContain('image: archlinux@sha256:')
    expect(workflow).toContain('ARCH_REPOSITORY_SNAPSHOT: 2026/08/17')
    expect(workflow).toContain('archive.archlinux.org/repos/%s/$repo/os/$arch')
    expect(workflow).toContain('Do not add mirror retries')
  })

  it('records the client-report boundary without claiming automatic Nova upload', () => {
    const notes = read('docs/release-notes/v1.3.11.md')

    expect(notes).toContain('POST /polaris/v1/support/client-report')
    expect(notes).toContain("does not yet automatically post to this host endpoint")
    expect(notes).toContain('must not be described as automatic paired reporting')
  })

  it('records the scalar and identifier redaction boundary', () => {
    const notes = read('docs/release-notes/v1.3.11.md')

    expect(notes).toContain('session_id')
    expect(notes).toContain('client_id')
    expect(notes).toContain('user_id')
    expect(notes).toContain('app_id')
    expect(notes).toContain('non-string scalars unchanged unless their field name is sensitive')
    expect(notes).toContain('preserving measurements such as bitrate, frame rate, packet loss, port, and GPU index')
  })

  it('keeps the exact four-asset set in the release notes', () => {
    const notes = read('docs/release-notes/v1.3.11.md')
    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]

    expect(listed.sort()).toEqual(expectedAssets)
  })
})
