import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const currentRelease = () => {
  const changelog = read('docs/changelog.md')
  const start = changelog.indexOf('## v1.4.7 - 2026-09-12')
  const end = changelog.indexOf('## v1.4.6 - 2026-09-11')
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return changelog.slice(start, end)
}

const currentNotes = () => read('docs/release-notes/v1.4.7.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()
const withdrawnSysextAsset = 'Polaris-sysext-x86_64.raw'

describe('v1.4.7 release contract', () => {
  it('pins the version every packaging surface agrees on', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.7')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.7"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain(
      'usr/bin/polaris-1.4.7',
    )
    expect(read('scripts/ci/build-steamos-package.sh')).toContain("'polaris|1.4.7-1|x86_64'")
  })

  it('says what the controller fix was, in the words the person who hit it would use', () => {
    const evidence = `${currentRelease()}\n${currentNotes()}`
    for (const fact of [
      'virtual DualSense',
      'Bluetooth bus',
      'triggers released',
      'PlayStation controller',
      'triggers swapped with the right stick',
    ]) {
      expect(evidence, `v1.4.7 must include: ${fact}`).toContain(fact)
    }
  })

  it('says what the configuration directory fixes were', () => {
    const evidence = `${currentRelease()}\n${currentNotes()}`
    for (const fact of ['owned by root', 'umask', 'setup-host', 'pair again']) {
      expect(evidence, `v1.4.7 must include: ${fact}`).toContain(fact)
    }
  })

  it('keeps the heads up honest about what is still open and what was not validated', () => {
    const notes = currentNotes()
    expect(notes).toContain('that is a separate thing and is still open')
    expect(notes).toContain('No new hardware validation run for this release')
    expect(notes).toContain('system extension stays withdrawn')
    expect(notes).not.toContain(withdrawnSysextAsset)
  })

  it('ships exactly the four supported packages and installs them from this tag', () => {
    const notes = currentNotes()
    const blocks = [...notes.matchAll(/```bash\n([\s\S]*?)\n```/g)]
      .map((match) => match[1])
      .filter((block) => block.includes('wget --output-document='))
    expect(blocks).toHaveLength(4)
    for (const asset of expectedAssets) {
      const matches = blocks.filter((block) => block.includes(`/${asset}`))
      expect(matches, `one command block for ${asset}`).toHaveLength(1)
      expect(matches[0]).toContain(`releases/download/v1.4.7/${asset}`)
    }

    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    const listed = [...new Set((assetsLine ?? '').match(/Polaris-[A-Za-z0-9][A-Za-z0-9._+-]*/g) ?? [])]
    expect(listed.sort()).toEqual(expectedAssets)
    expect(assetsLine).not.toContain(withdrawnSysextAsset)
  })

  it('closes the changelog section with the exact four-asset sentence', () => {
    const bullets = currentRelease()
      .split('\n')
      .filter((line) => line.startsWith('- '))
    expect(bullets.at(-1)).toContain(
      'Keeps exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, ' +
        '`Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb` ' +
        'as the official package assets',
    )
  })
})
