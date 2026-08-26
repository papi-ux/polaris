import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const sectionBetween = (source, startHeading, endHeading) => {
  const start = source.indexOf(startHeading)
  const end = source.indexOf(endHeading, start + startHeading.length)
  expect(start, `missing historical release heading: ${startHeading}`).toBeGreaterThanOrEqual(0)
  expect(end, `missing historical release boundary: ${endHeading}`).toBeGreaterThan(start)
  return source.slice(start, end)
}

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

const historicalRelease = () => sectionBetween(
  read('docs/changelog.md'),
  '## v1.3.13 - 2026-08-22',
  '## v1.3.12 - 2026-08-22',
)

const historicalNotes = () => read('docs/release-notes/v1.3.13.md')

describe('historical v1.3.13 release contract', () => {
  it('preserves the runtime reliability and configuration scope', () => {
    const evidence = `${historicalRelease()}\n${historicalNotes()}`
    for (const fact of [
      'SteamLaunch AppId=<id>',
      'Depot Download HTTP',
      'SIGKILL',
      'nested Gamescope',
      'EGL context',
      'Steam Input',
      'clear_ai_api_key',
      'Boost 1.92.0-1',
    ]) {
      expect(evidence, `historical v1.3.13 must include: ${fact}`).toContain(fact)
    }
  })

  it('preserves the exact historical assets and install identities', () => {
    const notes = historicalNotes()
    const listed = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)

    const blocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(3)
    for (const block of blocks) {
      expect(block).toContain('releases/download/v1.3.13/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }
  })

  it('preserves the exact Boost provider and immutable Arch gates', () => {
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
    expect(workflow).toContain('ARCH_REPOSITORY_SNAPSHOT: 2026/08/19')
    expect(workflow).toContain('ARCH_BOOST_PACKAGE_VERSION: 1.92.0-1')
    expect(workflow).toContain('ARCH_BOOST_SHA256: 0d795c6401c8bfa16012ada7e2e7f34934fb268f9174470edac1b389056f79bb')
    expect(workflow).toContain('ARCH_BOOST_LIBS_SHA256: 4b1392e578e46c1b23910d1c26956927d7e986d6afd5090be59045afb3c04f8d')
  })
})
