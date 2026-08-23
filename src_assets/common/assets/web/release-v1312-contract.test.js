import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const sectionBetween = (source, startHeading, endHeading) => {
  const start = source.indexOf(startHeading)
  const end = source.indexOf(endHeading, start + startHeading.length)
  expect(start, `missing historical release heading: ${startHeading}`).toBeGreaterThanOrEqual(0)
  expect(end, `missing historical release boundary after ${startHeading}: ${endHeading}`).toBeGreaterThan(start)
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
  '## v1.3.12 - 2026-08-22',
  '## v1.3.11 - 2026-08-19',
)

const historicalNotes = () => read('docs/release-notes/v1.3.12.md')

describe('historical v1.3.12 release contract', () => {
  it('preserves the packaging-only scope and Boost ABI facts', () => {
    const release = historicalRelease()
    const notes = historicalNotes()
    for (const fact of [
      'packaging',
      'Boost 1.91',
      'Boost 1.92.0-1',
      'libboost_locale.so=1.92.0-64',
      'no Polaris runtime-code',
    ]) {
      expect(`${release}\n${notes}`, `historical release must include: ${fact}`).toContain(fact)
    }
  })

  it('preserves the exact historical asset set and install identities', () => {
    const notes = historicalNotes()
    const assets = [...new Set(notes.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(assets.sort()).toEqual(expectedAssets)

    const blocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(3)
    for (const block of blocks) {
      expect(block).toContain('releases/download/v1.3.12/')
      expect(block).toContain('polaris --setup-host')
      expect(block).toContain('systemctl --user restart polaris')
    }
  })

  it('keeps the versioned Boost package contract active after v1.3.12', () => {
    const pkgbuild = read('packaging/linux/Arch/PKGBUILD')
    const verifier = read('scripts/check-release-package-dependencies.py')
    for (const library of [
      'libboost_filesystem.so',
      'libboost_locale.so',
      'libboost_log.so',
      'libboost_program_options.so',
      'libboost_thread.so',
    ]) {
      expect(pkgbuild).toContain(`'${library}'`)
      expect(verifier).toContain(`"${library}"`)
    }
  })
})
