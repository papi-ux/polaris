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
  sectionBetween(read('docs/changelog.md'), '## v1.3.10 - 2026-08-16', '## v1.3.9')

const requiredFacts = [
  'labwc',
  'libdisplay-info',
  'SteamOS',
  'HEVC Main10',
  'adaptive bitrate',
  'Doctor',
  'vaapi_headless_dmabuf_disabled_for_stability',
  'capture_transport=shm',
  'frame_residency=cpu',
  'npm audit --audit-level=high',
  'nested Gamescope',
  'exit-timeout',
  'capture pacing',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.3.10 release contract', () => {
  it('moves the CMake version and authoritative changelog heading together', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.3.10')
    expect(read('README.md')).not.toMatch(/^#{1,6}\s+.*(?:release|what is new).*v\d/im)
    expect(read('docs/changelog.md')).toContain('## v1.3.10 - 2026-08-16')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.3.10"')
  })

  it('states release facts in the authoritative changelog', () => {
    for (const fact of requiredFacts) {
      expect(releaseSection(), `release summary must include: ${fact}`).toContain(fact)
    }
  })

  it('aligns the public checker with the v1.3.10 release contract', () => {
    const publicDocsGate = read('scripts/check-public-docs.sh')

    expect(publicDocsGate).toContain('README must not duplicate a version-specific release section')
    expect(publicDocsGate).not.toContain('"## What is New in v1.3.10"')
    expect(publicDocsGate).toContain('"## v1.3.10 - 2026-08-16"')
    for (const asset of expectedAssets) {
      expect(publicDocsGate, `public checker must require: ${asset}`).toContain(asset)
    }
  })

  it('moves versioned package-review metadata to v1.3.10', () => {
    const reviewedWarnings = read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')
    const steamOsBuildScript = read('scripts/ci/build-steamos-package.sh')
    const packagingContract = read('src_assets/common/assets/web/packaging-contracts.test.js')

    expect(reviewedWarnings).toContain('usr/bin/polaris-1.3.10')
    expect(reviewedWarnings).not.toContain('usr/bin/polaris-1.3.9')
    expect(steamOsBuildScript).toContain("'polaris|1.3.10-1|x86_64'")
    expect(steamOsBuildScript).not.toContain("'polaris|1.3.9-1|x86_64'")
    expect(packagingContract).toContain("'polaris|1.3.10-1|x86_64'")
  })

  it('keeps labwc out of the SteamOS runtime dependencies', () => {
    const pkgbuild = read('packaging/linux/SteamOS/PKGBUILD')
    const dependsBlock = sectionBetween(pkgbuild, 'depends=(', ')')

    expect(dependsBlock, 'labwc must not be a SteamOS runtime dependency').not.toContain('labwc')
    expect(pkgbuild, 'labwc must remain an optional dependency').toContain(
      "'labwc: Private Stream paths; not installable on SteamOS 3.8, see docs/steamos.md'",
    )
  })

  it('ships release notes whose install commands target v1.3.10', () => {
    const releaseNotes = read('docs/release-notes/v1.3.10.md')
    const benchmarkOpenApi = read('docs/benchmark-control-openapi.json')

    expect(benchmarkOpenApi).toContain('"collector_version": "1.3.10"')
    expect(benchmarkOpenApi).not.toContain('"collector_version": "1.3.9"')
    for (const fact of [
      'v1.3.9',
      'labwc',
      'libdisplay-info',
      'SteamOS',
      'HEVC Main10',
      'Doctor',
      'vaapi_headless_dmabuf_disabled_for_stability',
      'capture_transport=shm',
      'frame_residency=cpu',
      '#409',
    ]) {
      expect(releaseNotes, `release notes must include: ${fact}`).toContain(fact)
    }

    const installBlocks = [...releaseNotes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))

    expect(installBlocks.length).toBe(3)
    for (const block of installBlocks) {
      expect(block).toContain('releases/download/v1.3.10/')
      expect(block).not.toContain('releases/download/v1.3.9/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }
  })

  it('records the VAAPI rollback as containment with an open affected-host gate', () => {
    const releaseNotes = read('docs/release-notes/v1.3.10.md')

    expect(releaseSection()).toContain('#367')
    expect(releaseSection()).toContain('#409')
    expect(releaseNotes).toContain('This is crash containment, not the 4K performance fix.')
    expect(releaseNotes).toContain('has not yet confirmed')
    expect(releaseNotes).toContain('no new coredump')
    expect(releaseNotes).toContain(
      'must not be described as affected-host validated until that evidence arrives',
    )
  })

  it('pins the prep-timeout regression fix so it cannot silently return', () => {
    // v1.3.9 bounded prep commands by the app's exit-timeout, default 5s, while
    // polaris-gamescope-session needs roughly 40s to come up. Every nested launch
    // 503'd. The fix is a dedicated budget on both halves of the lifecycle; if a
    // later change puts either back on _app.exit_timeout, this release contract
    // is the thing that notices.
    const process = read('src/process.cpp')

    expect(process).toContain('constexpr auto nested_gamescope_start_timeout = 120s;')
    expect(process).toContain('constexpr auto nested_gamescope_stop_timeout = 30s;')

    const collapsed = process.replace(/\s+/g, ' ')
    expect(collapsed).toContain(
      'const auto prep_timeout = critical_nested_session_prep ? nested_gamescope_start_timeout : _app.exit_timeout;',
    )
    expect(collapsed).toContain(
      'const auto undo_timeout = critical_nested_session_undo ? nested_gamescope_stop_timeout : _app.exit_timeout;',
    )

    // Both halves must stay diagnosable, not just bounded.
    expect(process).toContain('prep_output = stderr;')
    expect(process).toContain('undo_output = stderr;')

    // And the release has to tell the user, because it is a regression they hit.
    const notes = read('docs/release-notes/v1.3.10.md')
    expect(notes).toContain('### Nested Steam session launches')
    expect(notes).toContain('regression')
  })

  it('describes the high-refresh pacing fix without claiming affected-host validation', () => {
    const notes = read('docs/release-notes/v1.3.10.md')

    expect(notes).toContain('### Private high-refresh capture cadence')
    expect(notes).toContain('#434')
    // The measurement host does not reproduce the reported ~60 FPS symptom, so the
    // notes must not imply the reporter confirmed it.
    expect(notes).toContain('does **not** reproduce the reported fault')
    expect(notes).toContain('not affected-host validation')
  })

  it('keeps the exact four-asset set in the release notes', () => {
    const notes = read('docs/release-notes/v1.3.10.md')
    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]

    expect(listed.sort()).toEqual(expectedAssets)
  })
})
