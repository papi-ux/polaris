import { beforeEach, describe, expect, it, vi } from 'vitest'

const cache = vi.hoisted(() => ({
  clearCachedConfig: vi.fn(),
  primeCachedConfig: vi.fn(),
}))

vi.mock('./config-cache.js', () => cache)

import {
  initializeWebUiAuthState,
  isWebUiAuthenticated,
  markWebUiAuthenticated,
  markWebUiUnauthenticated,
  webUiAuthenticated,
} from './auth-state.js'

describe('web UI auth state', () => {
  beforeEach(() => {
    cache.clearCachedConfig.mockReset()
    cache.primeCachedConfig.mockReset()
    initializeWebUiAuthState()
  })

  it('initializes fail-closed and clears any cached config', () => {
    expect(isWebUiAuthenticated()).toBe(false)
    expect(webUiAuthenticated.value).toBe(false)
    expect(window.__POLARIS_AUTHENTICATED__).toBe(false)
    expect(cache.clearCachedConfig).toHaveBeenCalledOnce()
  })

  it('primes config before publishing authenticated state', () => {
    const config = { status: true, platform: 'linux' }

    markWebUiAuthenticated(config)

    expect(cache.primeCachedConfig).toHaveBeenCalledWith(config)
    expect(isWebUiAuthenticated()).toBe(true)
    expect(webUiAuthenticated.value).toBe(true)
    expect(window.__POLARIS_AUTHENTICATED__).toBe(true)
  })

  it('clears cached and published state on authoritative rejection', () => {
    markWebUiAuthenticated({ status: true })
    cache.clearCachedConfig.mockClear()

    markWebUiUnauthenticated()

    expect(cache.clearCachedConfig).toHaveBeenCalledOnce()
    expect(isWebUiAuthenticated()).toBe(false)
    expect(window.__POLARIS_AUTHENTICATED__).toBe(false)
  })
})
