import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

import { afterAll, beforeAll, describe, expect, it } from 'vitest'

// #673: the AppStream release entry was dated 1990-01-01 for every semver build, 1.5.0 included,
// because only a date-style version (YYYY.MMDD.N) carried a date. cmake/prep/release_date.cmake now
// takes POLARIS_RELEASE_DATE, then the HEAD commit date in UTC, then a SOURCE_DATE_EPOCH after
// 2020, and only then falls back to 1990-01-01 with a warning. These cases run the real
// build_version.cmake and configure the real metainfo template with what it sets.

const root = process.cwd()
const readSource = (path) => readFileSync(join(root, path), 'utf8')
const minimum = readSource('CMakeLists.txt').match(/^cmake_minimum_required\([^\n]+\)$/m)[0]
const template = join(root, 'packaging/linux/dev.polaris-stream.app.Polaris.metainfo.xml')

// What the build reads from its environment. Each case starts without all of them and sets its own.
const BUILD_ENVIRONMENT = [
  'BRANCH', 'BUILD_VERSION', 'CLONE_URL', 'COMMIT', 'TAG',
  'POLARIS_PRERELEASE_LABEL', 'POLARIS_RELEASE_DATE', 'SOURCE_DATE_EPOCH',
]

// 23:30 on the 14th in UTC-5 is the 15th in UTC. git's %cs would say the 14th.
const COMMITTED = '2026-03-14T23:30:00-05:00'
const COMMIT_DATE = '2026-03-15'

let fixtures
let checkout
let plain

const environment = (extra = {}) => {
  const env = { ...process.env }
  for (const name of BUILD_ENVIRONMENT) delete env[name]
  return { ...env, ...extra }
}

const git = (cwd, args, extra = {}) => {
  const result = spawnSync('git', args, {
    cwd,
    encoding: 'utf8',
    env: environment({
      GIT_CONFIG_GLOBAL: '/dev/null',
      GIT_CONFIG_NOSYSTEM: '1',
      GIT_AUTHOR_NAME: 'Polaris fixture',
      GIT_AUTHOR_EMAIL: 'fixture@polaris.invalid',
      GIT_COMMITTER_NAME: 'Polaris fixture',
      GIT_COMMITTER_EMAIL: 'fixture@polaris.invalid',
      ...extra,
    }),
  })
  expect(result.status, result.stderr).toBe(0)
  return result.stdout.trim()
}

// Run build_version.cmake as the project does, from source, and configure the metainfo with it.
const configure = (source, { version = '1.5.0', env = {}, defines = {} } = {}) => {
  const work = mkdtempSync(join(fixtures, 'configure-'))
  const output = join(work, 'metainfo.xml')
  const after = join(work, 'environment.txt')
  const driver = join(work, 'driver.cmake')
  writeFileSync(driver, `${minimum}
set(PROJECT_NAME "Polaris")
set(PROJECT_FQDN "dev.polaris-stream.app.Polaris")
set(PROJECT_VERSION "\${VERSION}")
include("${join(root, 'cmake/prep/build_version.cmake')}")
configure_file("${template}" "\${OUTPUT}" @ONLY)
if(DEFINED ENV{SOURCE_DATE_EPOCH})
  file(WRITE "\${AFTER}" "SOURCE_DATE_EPOCH=$ENV{SOURCE_DATE_EPOCH}")
else()
  file(WRITE "\${AFTER}" "SOURCE_DATE_EPOCH unset")
endif()
`)
  const args = [`-DVERSION=${version}`, `-DOUTPUT=${output}`, `-DAFTER=${after}`]
  for (const [name, value] of Object.entries(defines)) args.push(`-D${name}=${value}`)
  args.push('-P', driver)
  // In script mode CMAKE_SOURCE_DIR is the working directory, so that is where the source is.
  const result = spawnSync('cmake', args, { cwd: source, encoding: 'utf8', env: environment(env) })
  const generated = existsSync(output) ? readFileSync(output, 'utf8') : null
  const release = generated?.match(/<release version="([^"]*)" date="([^"]*)">/)
  return {
    status: result.status,
    log: `${result.stdout}${result.stderr}`,
    stderr: result.stderr,
    version: release?.[1],
    date: release?.[2],
    after: existsSync(after) ? readFileSync(after, 'utf8') : null,
  }
}

const configured = (source, options) => {
  const result = configure(source, options)
  expect(result.status, result.log).toBe(0)
  return result
}

beforeAll(() => {
  fixtures = mkdtempSync(join(tmpdir(), 'polaris-release-date-'))
  checkout = join(fixtures, 'checkout')
  plain = join(fixtures, 'tarball')
  mkdirSync(checkout)
  mkdirSync(plain)
  writeFileSync(join(checkout, 'CMakeLists.txt'), 'project(Polaris VERSION 1.5.0)\n')
  writeFileSync(join(plain, 'CMakeLists.txt'), 'project(Polaris VERSION 1.5.0)\n')
  git(checkout, ['init', '--quiet'])
  git(checkout, ['add', 'CMakeLists.txt'])
  git(checkout, ['commit', '--quiet', '--no-gpg-sign', '-m', 'fixture'], {
    GIT_AUTHOR_DATE: COMMITTED,
    GIT_COMMITTER_DATE: COMMITTED,
  })
})

afterAll(() => {
  if (fixtures) rmSync(fixtures, { force: true, recursive: true })
})

describe('AppStream release date', () => {
  it('dates a semver CI build by its HEAD commit, in UTC', () => {
    const ci = { BRANCH: 'staging', BUILD_VERSION: '1.5.0' }
    expect(configured(checkout, { env: ci })).toMatchObject({ version: '1.5.0', date: COMMIT_DATE })
    // A beta tag build reaches CMake as the release number and a label; its entry is dated the same.
    expect(configured(checkout, { env: { ...ci, POLARIS_PRERELEASE_LABEL: 'beta.2' } }))
      .toMatchObject({ version: '1.5.0', date: COMMIT_DATE })
  })

  it('dates a local git build by its HEAD commit too', () => {
    const result = configured(checkout)
    expect(result.version).toMatch(/^1\.5\.0/)
    expect(result.date).toBe(COMMIT_DATE)
    expect(result.log).toContain('POLARIS_RELEASE_DATE_SOURCE: HEAD commit date')
  })

  it('puts SOURCE_DATE_EPOCH back after borrowing it to format the commit time', () => {
    const ci = { BRANCH: 'staging', BUILD_VERSION: '1.5.0' }
    expect(configured(checkout, { env: ci }).after).toBe('SOURCE_DATE_EPOCH unset')
    expect(configured(checkout, { env: { ...ci, SOURCE_DATE_EPOCH: '315532800' } }))
      .toMatchObject({ date: COMMIT_DATE, after: 'SOURCE_DATE_EPOCH=315532800' })
  })

  it('takes POLARIS_RELEASE_DATE over the commit, and the CMake variable over the environment', () => {
    const ci = { BRANCH: 'staging', BUILD_VERSION: '1.5.0' }
    expect(configured(checkout, { env: { ...ci, POLARIS_RELEASE_DATE: '2026-11-20' } }).date).toBe('2026-11-20')
    expect(configured(plain, { env: { ...ci, POLARIS_RELEASE_DATE: '2026-11-20' } }).date).toBe('2026-11-20')
    expect(configured(checkout, {
      env: { ...ci, POLARIS_RELEASE_DATE: '2026-11-20' },
      defines: { POLARIS_RELEASE_DATE: '2026-11-21' },
    }).date).toBe('2026-11-21')
    expect(configured(plain, { env: { ...ci, POLARIS_RELEASE_DATE: '2028-02-29' } }).date).toBe('2028-02-29')
    expect(configured(plain, { env: { ...ci, POLARIS_RELEASE_DATE: '2000-02-29' } }).date).toBe('2000-02-29')
    // Empty is unset, the way a PKGBUILD or spec exports a variable it has no value for.
    expect(configured(checkout, { env: { ...ci, POLARIS_RELEASE_DATE: '' } }).date).toBe(COMMIT_DATE)
  })

  it('fails the configure on a malformed POLARIS_RELEASE_DATE', () => {
    const ci = { BRANCH: 'staging', BUILD_VERSION: '1.5.0' }
    for (const value of [
      '2026-1-05', '20261120', '2026/11/20', '2026-11-20T00:00:00Z', ' 2026-11-20', '2026-11-20 ',
      'today', '2026-13-01', '2026-00-10', '2026-11-00', '2026-11-31', '2026-02-29', '1900-02-29',
      '0000-01-01',
    ]) {
      const result = configure(checkout, { env: { ...ci, POLARIS_RELEASE_DATE: value } })
      expect(result.status, `${JSON.stringify(value)} was accepted`).not.toBe(0)
      expect(result.stderr).toContain('POLARIS_RELEASE_DATE')
      expect(result.date).toBeUndefined()
    }
    // As a CMake variable, and with a date-style version that would not have used it.
    expect(configure(checkout, { env: ci, defines: { POLARIS_RELEASE_DATE: '2026-02-30' } }).status).not.toBe(0)
    expect(configure(checkout, {
      env: { BRANCH: 'staging', BUILD_VERSION: '2025.1203.1', POLARIS_RELEASE_DATE: '12/03/2025' },
    }).status).not.toBe(0)
  })

  it('ignores the SOURCE_DATE_EPOCH Nix sets, 1980-01-01, and warns at 1990-01-01', () => {
    const ci = { BRANCH: 'staging', BUILD_VERSION: '1.5.0' }
    const result = configured(plain, { env: { ...ci, SOURCE_DATE_EPOCH: '315532800' } })
    expect(result.date).toBe('1990-01-01')
    expect(result.stderr).toContain('CMake Warning')
    expect(result.stderr).toContain('SOURCE_DATE_EPOCH=315532800 was ignored')
    for (const epoch of ['0', '1577836800', 'yesterday']) {
      expect(configured(plain, { env: { ...ci, SOURCE_DATE_EPOCH: epoch } }).date).toBe('1990-01-01')
    }
  })

  it('uses a SOURCE_DATE_EPOCH after 2020 when there is no git, in UTC', () => {
    const ci = { BRANCH: 'staging', BUILD_VERSION: '1.5.0' }
    // 2025-12-31T23:59:59Z.
    const result = configured(plain, { env: { ...ci, SOURCE_DATE_EPOCH: '1767225599' } })
    expect(result).toMatchObject({ date: '2025-12-31', after: 'SOURCE_DATE_EPOCH=1767225599' })
    expect(result.stderr).not.toContain('CMake Warning')
    // The commit still comes first.
    expect(configured(checkout, { env: { ...ci, SOURCE_DATE_EPOCH: '1767225599' } }).date).toBe(COMMIT_DATE)
  })

  it('warns and falls back to 1990-01-01 with nothing to go on', () => {
    const result = configured(plain, { env: { BRANCH: 'staging', BUILD_VERSION: '1.5.0' } })
    expect(result.date).toBe('1990-01-01')
    expect(result.stderr).toContain('CMake Warning')
    expect(result.stderr).toContain('No release date for Polaris 1.5.0')
    expect(result.log).toContain('POLARIS_RELEASE_DATE_SOURCE: fallback')
  })

  it('keeps dating a date-style version by the version itself', () => {
    for (const [version, date] of [['2025.1203.1', '2025-12-03'], ['2025.703.2', '2025-07-03']]) {
      const ci = { BRANCH: 'staging', BUILD_VERSION: version }
      expect(configured(plain, { env: ci })).toMatchObject({ version, date })
      expect(configured(checkout, { env: { ...ci, POLARIS_RELEASE_DATE: '2026-11-20' } }))
        .toMatchObject({ version, date })
    }
  })
})
