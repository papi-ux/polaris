import { describe, expect, it } from 'vitest'
import PolarisVersion from './polaris_version.js'

const newer = (left, right, incremental = true) => new PolarisVersion(null, left).isGreater(right, incremental)

describe('release channel ordering shared with Nova', () => {
  it.each([
    ['1.4.13-beta.4', '1.4.13-beta.3'],
    ['1.4.13-beta.10', '1.4.13-beta.9'],
    ['1.4.13-rc.1', '1.4.13-beta.49'],
    ['1.4.13-rc.10', '1.4.13-rc.9'],
    ['1.4.13', '1.4.13-rc.49'],
    ['1.4.14-beta.1', '1.4.13-rc.49'],
    ['1.4.13-beta.1', '1.4.13-alpha.49'],
    ['1.4.13-alpha.1', '1.4.13-pre'],
    ['1.4.13-beta.65536', '1.4.13-beta.65535'],
  ])('%s follows %s, never the reverse', (left, right) => {
    expect(newer(left, right)).toBe(true)
    expect(newer(right, left)).toBe(false)
    expect(newer(left, left)).toBe(false)
  })

  it('ignores build metadata and accepts the release tag prefix', () => {
    expect(newer('v1.4.13-rc.2+build.7', '1.4.13-rc.1')).toBe(true)
    expect(newer('1.4.13+build.8', 'v1.4.13+build.7')).toBe(false)
  })

  it('keeps explicit release-number-only and unknown development comparisons', () => {
    expect(newer('1.4.13', '1.4.13-beta.3', false)).toBe(false)
    expect(newer('1.4.14-beta.1', '1.4.13', false)).toBe(true)
    expect(newer('1.4.13', '1.4.13-17-gabcdef')).toBe(false)
    expect(newer('1.4.13-rc.1', '1.4.13-custom')).toBe(false)
  })

  it.each(['garbage', '1.4', '1x.4.13', '9007199254740992.0.0'])('does not order an invalid version %s', version => {
    expect(newer(version, '1.4.13')).toBe(false)
    expect(newer('1.4.13', version)).toBe(false)
  })
})
