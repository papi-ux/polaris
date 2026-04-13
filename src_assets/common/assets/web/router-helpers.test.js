import {
  getIpv4LoopbackUrl,
  isDynamicImportError,
  isPublicRoute,
  redirectToIpv4Loopback,
  wrapFetchWithCsrfToken,
} from './router-helpers.js'

describe('router helpers', () => {
  it('builds an IPv4 loopback URL from the current location', () => {
    expect(getIpv4LoopbackUrl({
      protocol: 'https:',
      port: '47990',
      pathname: '/ui/',
      hash: '#/login',
    })).toBe('https://127.0.0.1:47990/ui/#/login')
  })

  it('redirects localhost sessions to 127.0.0.1', () => {
    const calls = []
    const redirected = redirectToIpv4Loopback(
      {
        hostname: 'localhost',
        protocol: 'https:',
        port: '47990',
        pathname: '/ui/',
        hash: '#/login',
      },
      (url) => calls.push(url)
    )

    expect(redirected).toBe(true)
    expect(calls).toEqual(['https://127.0.0.1:47990/ui/#/login'])
  })

  it('leaves non-localhost sessions alone', () => {
    const redirected = redirectToIpv4Loopback(
      {
        hostname: '192.168.1.20',
        protocol: 'https:',
        port: '47990',
        pathname: '/ui/',
        hash: '#/',
      },
      () => {
        throw new Error('redirect should not be attempted')
      }
    )

    expect(redirected).toBe(false)
  })

  it('recognizes dynamic import fetch failures', () => {
    expect(isDynamicImportError(new Error('Failed to fetch dynamically imported module'))).toBe(true)
    expect(isDynamicImportError(new Error('Importing a module script failed'))).toBe(true)
    expect(isDynamicImportError(new Error('Different error'))).toBe(false)
  })

  it('tracks public auth routes explicitly', () => {
    expect(isPublicRoute('/login')).toBe(true)
    expect(isPublicRoute('/welcome')).toBe(true)
    expect(isPublicRoute('/config')).toBe(false)
  })

  it('adds a CSRF header only to mutating fetch requests', async () => {
    const calls = []
    const wrappedFetch = wrapFetchWithCsrfToken(function mockFetch(url, options) {
      calls.push({ url, options })
      return Promise.resolve({ ok: true })
    }, 'csrf-123')

    await wrappedFetch('/api/config', { method: 'POST' })
    await wrappedFetch('/api/config', { method: 'GET' })

    expect(calls[0].options.headers['X-CSRF-Token']).toBe('csrf-123')
    expect(calls[1].options.headers).toBeUndefined()
  })
})
