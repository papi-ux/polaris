import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { buildManualInstallCommand } from './update-center.js'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const sectionBetween = (source, startHeading, endHeading) => {
  const start = source.indexOf(startHeading)
  const end = source.indexOf(endHeading, start + startHeading.length)
  expect(start, `missing release heading: ${startHeading}`).toBeGreaterThanOrEqual(0)
  expect(end, `missing release boundary after ${startHeading}: ${endHeading}`).toBeGreaterThan(start)
  return source.slice(start, end)
}

const releaseSections = () => [
  sectionBetween(read('README.md'), '## What is New in v1.3.5', '## Install'),
  sectionBetween(read('docs/changelog.md'), '## v1.3.5 - 2026-08-06', '## v1.3.4'),
]

const requiredUpdateFacts = [
  'v1.3.4',
  'exact package filename',
  'wget --output-document',
  '.1',
  '.2',
  'short-circuit',
  'sudo -H',
  'npm audit --audit-level=high',
]

// A release that changes files on the host at upgrade has to say so. v1.3.5 was
// first written as an updater-only patch and went out of date silently as eleven
// further commits landed; asserting the user-visible packaging change keeps the
// next release from omitting one the same way.
const requiredHostIntegrationFacts = [
  '/usr/lib',
  '/etc',
  'polaris-debug',
  'input',
]

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.3.5 release contract', () => {
  it('moves the CMake version and public release headings together', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.3.5')
    expect(read('README.md')).toContain('## What is New in v1.3.5')
    expect(read('docs/changelog.md')).toContain('## v1.3.5 - 2026-08-06')
  })

  it('records the updater bootstrap and failure-safety facts', () => {
    for (const releaseSection of releaseSections()) {
      for (const fact of requiredUpdateFacts) {
        expect(releaseSection, `release notes must include: ${fact}`).toContain(fact)
      }
    }
  })

  it('states the host integration changes that alter files on upgrade', () => {
    const releaseNotes = read('docs/release-notes/v1.3.5.md')
    for (const fact of requiredHostIntegrationFacts) {
      expect(releaseNotes, `release notes must include: ${fact}`).toContain(fact)
    }
  })

  it('aligns the public checker with the v1.3.5 release contract', () => {
    const publicDocsGate = read('scripts/check-public-docs.sh')

    expect(publicDocsGate).toContain('"## What is New in v1.3.5"')
    expect(publicDocsGate).not.toContain('"## What is New in v1.3.4"')
    expect(publicDocsGate).toContain('"## v1.3.5 - 2026-08-06"')
    for (const fact of requiredUpdateFacts) {
      expect(publicDocsGate, `public checker must require: ${fact}`).toContain(fact)
    }
    for (const asset of expectedAssets) {
      expect(publicDocsGate, `public checker must require: ${asset}`).toContain(asset)
    }
  })

  it('moves versioned package-review metadata to v1.3.5', () => {
    const reviewedWarnings = read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')
    const steamOsBuildScript = read('scripts/ci/build-steamos-package.sh')

    expect(reviewedWarnings).toContain("usr/bin/polaris-1.3.5")
    expect(reviewedWarnings).not.toContain("usr/bin/polaris-1.3.4")
    expect(steamOsBuildScript).toContain("'polaris|1.3.5-1|x86_64'")
    expect(steamOsBuildScript).not.toContain("'polaris|1.3.4-1|x86_64'")
  })

  it('ships ready-to-publish v1.3.4 bootstrap release notes', () => {
    const releaseNotes = read('docs/release-notes/v1.3.5.md')
    const packageCases = [
      ['fedora', 'Polaris-fedora44-x86_64.rpm'],
      ['arch', 'Polaris-arch-x86_64.pkg.tar.zst'],
      ['ubuntu', 'Polaris-ubuntu24.04-x86_64.deb'],
    ]
    const expectedCommands = packageCases.map(([packageFamily, name]) => buildManualInstallCommand({
      name,
      browser_download_url: `https://github.com/papi-ux/polaris/releases/download/v1.3.5/${name}`,
      packageFamily,
    }))
    const documentedCommands = [...releaseNotes.matchAll(/```bash\n([\s\S]*?)\n```/g)].map((match) => match[1])

    for (const fact of ['v1.3.4', '.1', '.2', 'exact package filename']) {
      expect(releaseNotes).toContain(fact)
    }
    expect(documentedCommands).toEqual(expectedCommands)
    expect(releaseNotes.match(/sudo -H polaris --setup-host &&/g)).toHaveLength(3)
    expect(releaseNotes.match(/systemctl --user restart polaris/g)).toHaveLength(3)
  })

  it('keeps the official release asset set exact', () => {
    for (const releaseSection of releaseSections()) {
      const actualAssets = releaseSection.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? []
      expect(actualAssets.sort()).toEqual(expectedAssets)
    }
  })
})
