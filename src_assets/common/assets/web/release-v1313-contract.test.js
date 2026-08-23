import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.3.13 release contract', () => {
  it('moves every current version identity to v1.3.13', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.3.13')
    expect(read('CMakeLists.txt')).not.toContain('project(Polaris VERSION 1.3.11')
    expect(read('docs/changelog.md')).toContain('## v1.3.13 - 2026-08-22')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.3.13"')

    const reviewedWarnings = read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')
    const steamOsBuildScript = read('scripts/ci/build-steamos-package.sh')
    const packagingContract = read('src_assets/common/assets/web/packaging-contracts.test.js')
    expect(reviewedWarnings).toContain('usr/bin/polaris-1.3.13')
    expect(reviewedWarnings).not.toContain('usr/bin/polaris-1.3.11')
    expect(steamOsBuildScript).toContain("'polaris|1.3.13-1|x86_64'")
    expect(steamOsBuildScript).not.toContain("'polaris|1.3.11-1|x86_64'")
    expect(packagingContract).toContain("'polaris|1.3.13-1|x86_64'")
  })

  it('keeps v1.3.11 and v1.3.12 historical while making v1.3.13 current', () => {
    const v1311 = read('src_assets/common/assets/web/release-v1311-contract.test.js')
    const v1312 = read('src_assets/common/assets/web/release-v1312-contract.test.js')
    expect(v1311).toContain("describe('historical v1.3.11 release contract'")
    expect(v1312).toContain("describe('historical v1.3.12 release contract'")

    const publicDocsGate = read('scripts/check-public-docs.sh')
    expect(publicDocsGate).toContain('docs/release-notes/v1.3.13.md')
    expect(publicDocsGate).toContain('"## v1.3.13 - 2026-08-22"')
  })

  it('publishes exact v1.3.13 install identities and runtime scope', () => {
    const notes = read('docs/release-notes/v1.3.13.md')
    for (const fact of [
      'Steam app lineage',
      'Depot Download HTTP',
      'clear stored AI API key',
      'Steam Input',
      'nested Gamescope',
      'private session log',
      'EGL context',
      'Boost 1.92.0-1',
    ]) {
      expect(notes, `release notes must include: ${fact}`).toContain(fact)
    }

    const installBlocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(installBlocks).toHaveLength(3)
    for (const block of installBlocks) {
      expect(block).toContain('releases/download/v1.3.13/')
      expect(block).not.toContain('releases/download/v1.3.11/')
      expect(block).not.toContain('releases/download/v1.3.12/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }

    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
  })

  it('retains exact Boost providers and immutable Arch gates', () => {
    const pkgbuild = read('packaging/linux/Arch/PKGBUILD')
    const workflow = read('.github/workflows/build.yml')
    for (const library of [
      'libboost_filesystem.so',
      'libboost_locale.so',
      'libboost_log.so',
      'libboost_program_options.so',
      'libboost_thread.so',
    ]) {
      expect(pkgbuild).toContain(`'${library}'`)
    }
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
    expect(workflow).toContain('ldd "$installed_binary"')
    expect(workflow).toContain('polaris --version')
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
})
