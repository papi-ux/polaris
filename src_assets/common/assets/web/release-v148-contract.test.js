import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')
const notes = () => read('docs/release-notes/v1.4.8.md')
const release = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.8 - 2026-09-16')
  const end = changelog.indexOf('## v1.4.7 - 2026-09-12')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}
const assets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('v1.4.8 release contract', () => {
  it('keeps package versions and the benchmark example aligned', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.8')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.8"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain('usr/bin/polaris-1.4.8')
    expect(read('scripts/ci/build-steamos-package.sh')).toContain("'polaris|1.4.8-1|x86_64'")
  })

  it('explains the library and streaming improvements in player terms', () => {
    for (const fact of ['matched with Nova v1.4.8', 'ROM folder', 'covers', '240 FPS', 'Doctor', 'Steam Big Picture']) {
      expect(notes(), `Release notes must include: ${fact}`).toContain(fact)
    }
    expect(release()).toContain('Ubuntu 24.04')
    expect(release()).toContain('A refused launch')
  })

  it('keeps preview availability and performance limits visible', () => {
    for (const fact of [
      'Early Preview',
      'configured hosts',
      'public Docker runtime download is not available yet',
      'one active Space',
      'Handheld audio remains under investigation',
      'targets, not performance guarantees',
      'removing a Space retains its games and saves',
      'system extension stays withdrawn',
      'experimental Desktop Mode support',
    ]) {
      expect(notes(), `Release limits must include: ${fact}`).toContain(fact)
    }
    expect(notes()).not.toContain('Polaris-sysext-x86_64.raw')
  })

  it('ships exactly four packages with install commands pinned to this tag', () => {
    const blocks = [...notes().matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(4)
    for (const asset of assets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `One install block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.8/${asset}`)
    }
    const line = notes().split('\n').find((item) => item.startsWith('**Assets:**'))
    expect(line).toBeDefined()
    expect([...new Set(line.match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g))].sort()).toEqual(assets)
    expect(release().split('\n').filter((item) => item.startsWith('- ')).at(-1)).toContain(
      'Keeps exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, ' +
      '`Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb` as the official package assets',
    )
  })
})
