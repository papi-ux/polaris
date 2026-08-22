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

const boostLibraries = [
  'libboost_filesystem.so',
  'libboost_locale.so',
  'libboost_log.so',
  'libboost_program_options.so',
  'libboost_thread.so',
]

describe('v1.3.12 packaging-only release contract', () => {
  it('moves every current version identity to v1.3.12', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.3.12')
    expect(read('CMakeLists.txt')).not.toContain('project(Polaris VERSION 1.3.11')
    expect(read('docs/changelog.md')).toContain('## v1.3.12 - 2026-08-22')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.3.12"')

    const reviewedWarnings = read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')
    const steamOsBuildScript = read('scripts/ci/build-steamos-package.sh')
    const packagingContract = read('src_assets/common/assets/web/packaging-contracts.test.js')
    expect(reviewedWarnings).toContain('usr/bin/polaris-1.3.12')
    expect(reviewedWarnings).not.toContain('usr/bin/polaris-1.3.11')
    expect(steamOsBuildScript).toContain("'polaris|1.3.12-1|x86_64'")
    expect(steamOsBuildScript).not.toContain("'polaris|1.3.11-1|x86_64'")
    expect(packagingContract).toContain("'polaris|1.3.12-1|x86_64'")
  })

  it('keeps v1.3.11 historical while making v1.3.12 current', () => {
    const historical = read('src_assets/common/assets/web/release-v1311-contract.test.js')
    expect(historical).toContain("describe('historical v1.3.11 release contract'")
    expect(historical).not.toContain('project(Polaris VERSION 1.3.11')

    const publicDocsGate = read('scripts/check-public-docs.sh')
    expect(publicDocsGate).toContain('docs/release-notes/v1.3.12.md')
    expect(publicDocsGate).toContain('"## v1.3.12 - 2026-08-22"')
  })

  it('publishes narrow packaging-only notes with exact install identities', () => {
    const notes = read('docs/release-notes/v1.3.12.md')
    expect(notes).toContain('packaging-only compatibility hotfix')
    expect(notes).toContain('Boost 1.91')
    expect(notes).toContain('Boost 1.92.0-1')
    expect(notes).toContain('no Polaris runtime-code, protocol, configuration, or Nova client change')
    expect(notes).toContain('known private-Steam shutdown coredump is fixed')

    const installBlocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(installBlocks).toHaveLength(3)
    for (const block of installBlocks) {
      expect(block).toContain('releases/download/v1.3.12/')
      expect(block).not.toContain('releases/download/v1.3.11/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }

    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
  })

  it('binds the Arch package to exact Boost providers', () => {
    const pkgbuild = read('packaging/linux/Arch/PKGBUILD')
    for (const library of boostLibraries) {
      expect(pkgbuild).toContain(`'${library}'`)
    }

    const verifier = read('scripts/check-release-package-dependencies.py')
    expect(verifier).toContain('"Arch versioned Boost runtime dependencies"')
    for (const library of boostLibraries) {
      expect(verifier).toContain(`"${library}"`)
    }
    expect(verifier).toContain('Arch package smoke must bind every Boost NEEDED entry to a versioned dependency')
  })

  it('separates reproducible construction from rolling compatibility', () => {
    const workflow = read('.github/workflows/build.yml')
    expect(workflow).toContain('ARCH_REPOSITORY_SNAPSHOT: 2026/08/19')
    expect(workflow).toContain('ARCH_BOOST_PACKAGE_VERSION: 1.92.0-1')
    expect(workflow).toContain('ARCH_BOOST_SHA256: 0d795c6401c8bfa16012ada7e2e7f34934fb268f9174470edac1b389056f79bb')
    expect(workflow).toContain('ARCH_BOOST_LIBS_SHA256: 4b1392e578e46c1b23910d1c26956927d7e986d6afd5090be59045afb3c04f8d')
    expect(workflow).toContain('arch-current-compatibility:')
    expect(workflow).toContain('geo.mirror.pkgbuild.com/$repo/os/$arch')
    expect(workflow).toContain('dependency="${soname%%.so.*}.so"')
    expect(workflow).toContain('depend = ${dependency}=${boost_version}-')
    expect(workflow).toContain('ldd "$installed_binary"')
    expect(workflow).toContain('polaris --version')
    expect(workflow).toContain('- arch-current-compatibility')
  })
})
