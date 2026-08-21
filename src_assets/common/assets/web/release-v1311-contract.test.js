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
  '## v1.3.11 - 2026-08-19',
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
  'POLARIS_PORTAL_DMABUF',
  'Matrix',
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
    expect(read('docs/changelog.md')).toContain('## v1.3.11 - 2026-08-19')
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
    expect(publicDocsGate).toContain('"## v1.3.11 - 2026-08-19"')
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

  it('binds curated notes and assets to one immutable release source', () => {
    const workflow = read('.github/workflows/build.yml')

    expect(workflow).toContain('release_notes="docs/release-notes/${POLARIS_PACKAGE_REF_NAME}.md"')
    expect(workflow).toContain('- name: Check out exact release source')
    expect(workflow).toContain('ref: ${{ needs.resolve-source.outputs.commit }}')
    const orderedSteps = [
      '- name: Check out exact release source',
      '- name: Revalidate release tag against packaged source',
      '- name: Stage curated GitHub release',
      '- name: Upload release assets to GitHub release',
      '- name: Verify release assets on GitHub release',
      '- name: Publish verified draft release',
    ]
    const positions = orderedSteps.map((step) => workflow.indexOf(step))
    expect(positions.every((position) => position >= 0)).toBe(true)
    expect(positions).toEqual([...positions].sort((left, right) => left - right))
    expect(workflow).toContain('^v[0-9]+\\.[0-9]+\\.[0-9]+$')
    expect(workflow).toContain('tag_commit="$(git rev-parse "refs/tags/${release_tag}^{commit}")"')
    expect(workflow).toContain('if [ "$tag_commit" != "$source_commit" ]; then')
    expect(workflow).toContain('--draft')
    expect(workflow).toContain('--draft=true')
    expect(workflow.match(/echo "publish_draft=true"/g)).toHaveLength(2)
    expect(workflow).not.toContain('is_draft=')
    expect(workflow).toContain('--verify-tag')
    expect(workflow).toContain('--notes-file "$release_notes"')
    expect(workflow).toContain('gh release edit "${POLARIS_PACKAGE_REF_NAME}"')
    expect(workflow).toContain('published_notes="$(gh release view "${POLARIS_PACKAGE_REF_NAME}" --json body --jq .body)"')
    expect(workflow).toContain('release_files=(release-assets/final/*)')
    expect(workflow).toContain('expected_asset_names["$(basename "$release_file")"]=1')
    expect(workflow).toContain('gh release delete-asset "${POLARIS_PACKAGE_REF_NAME}" "$published_asset" --yes')
    expect(workflow).toContain('[ "${expected_assets[*]}" != "${published_assets[*]}" ]')
    expect(workflow).toContain('run: gh release edit "${POLARIS_PACKAGE_REF_NAME}" --verify-tag --draft=false')
    expect(workflow).not.toContain('Automated Fedora 44, Ubuntu, Arch, and SteamOS 3.8 release assets')
  })

  it('pins Arch build inputs and separately checks rolling compatibility', () => {
    const workflow = read('.github/workflows/build.yml')

    expect(workflow).toContain('image: archlinux@sha256:')
    expect(workflow).toContain('ARCH_REPOSITORY_SNAPSHOT: 2026/08/19')
    expect(workflow).toContain('ARCH_BOOST_PACKAGE_VERSION: 1.92.0-1')
    expect(workflow).toContain('ARCH_BOOST_SHA256: 0d795c6401c8bfa16012ada7e2e7f34934fb268f9174470edac1b389056f79bb')
    expect(workflow).toContain('ARCH_BOOST_LIBS_SHA256: 4b1392e578e46c1b23910d1c26956927d7e986d6afd5090be59045afb3c04f8d')
    expect(workflow).toContain('archive.archlinux.org/repos/%s/$repo/os/$arch')
    expect(workflow).toContain('- name: Install pinned Boost ABI')
    expect(workflow).toContain('Do not add mirror retries')
    expect(workflow).toContain('arch-current-compatibility:')
    expect(workflow).toContain('geo.mirror.pkgbuild.com/$repo/os/$arch')
    expect(workflow).toContain('- arch-current-compatibility')
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

  it('records the VAAPI containment and affected-host evidence boundary', () => {
    const notes = read('docs/release-notes/v1.3.11.md')

    expect(notes).toContain('Keeps PipeWire VAAPI capture on SHM by default')
    expect(notes).toContain('POLARIS_PORTAL_DMABUF=1')
    expect(notes).toContain('affected RX 7800 XT has not physically tested this exact candidate')
    expect(notes).toContain('does not claim that the underlying DMA-BUF')
    expect(notes).not.toContain('containment remains in draft PR #481')
  })

  it('keeps the exact four-asset set in the release notes', () => {
    const notes = read('docs/release-notes/v1.3.11.md')
    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]

    expect(listed.sort()).toEqual(expectedAssets)
  })
})
