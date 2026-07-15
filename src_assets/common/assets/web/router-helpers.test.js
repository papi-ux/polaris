import {
  AUTH_PROBE_STATE,
  clearPolarisReloadParam,
  getAuthRedirectPath,
  getIpv4LoopbackUrl,
  getSafeAuthReturnTarget,
  isDynamicImportError,
  isPublicRoute,
  probeWebUiAuth,
  redirectToIpv4Loopback,
  wrapFetchWithCsrfToken,
} from './router-helpers.js'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'

describe('router helpers', () => {
  it('keeps Browser Stream primary route and WebRTC compatibility redirect', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/main.js'), 'utf8')

    expect(source).toContain("path: '/browser-stream'")
    expect(source).toContain("path: '/webrtc', redirect: '/browser-stream'")
  })

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
    expect(isPublicRoute('/recover')).toBe(true)
    expect(isPublicRoute('/reconnecting')).toBe(true)
    expect(isPublicRoute('/config')).toBe(false)
  })

  it('maps auth redirects from API probes to SPA routes', () => {
    expect(getAuthRedirectPath({
      redirected: true,
      url: 'https://127.0.0.1:47990/welcome',
    })).toBe('/welcome')
    expect(getAuthRedirectPath({
      redirected: true,
      url: 'https://127.0.0.1:47990/login',
    })).toBe('/login')
    expect(getAuthRedirectPath({
      redirected: true,
      url: 'https://127.0.0.1:47990/WELCOME/',
    })).toBe('/welcome')
    expect(getAuthRedirectPath({
      redirected: true,
      url: 'https://127.0.0.1:47990/LOGIN/',
    })).toBe('/login')
  })

  it('ignores non-auth API probe responses', () => {
    expect(getAuthRedirectPath({
      redirected: false,
      url: 'https://127.0.0.1:47990/welcome',
    })).toBe('')
    expect(getAuthRedirectPath({
      redirected: true,
      url: 'https://127.0.0.1:47990/config',
    })).toBe('')
    expect(getAuthRedirectPath({
      redirected: true,
      url: 'not a url',
    })).toBe('')
  })

  it('adds a CSRF header only to mutating fetch requests', async () => {
    const calls = []
    const wrappedFetch = wrapFetchWithCsrfToken(function mockFetch(url, options) {
      calls.push({ url, options })
      return Promise.resolve({ ok: true, status: 200 })
    }, 'csrf-123')

    await wrappedFetch('/api/config', { method: 'POST' })
    await wrappedFetch('/api/config', { method: 'GET' })

    expect(calls[0].options.headers['X-CSRF-Token']).toBe('csrf-123')
    expect(calls[1].options.headers).toBeUndefined()
  })

  it('refreshes the CSRF token and retries mutating requests after a restart-style 403', async () => {
    document.head.innerHTML = '<meta name="csrf-token" content="csrf-123">'

    const calls = []
    const wrappedFetch = wrapFetchWithCsrfToken(function mockFetch(url, options) {
      calls.push({ url, options })

      if (calls.length === 1) {
        return Promise.resolve({ ok: false, status: 403 })
      }

      if (calls.length === 2) {
        return Promise.resolve({
          ok: true,
          status: 200,
          text: async () => '<html><head><meta name="csrf-token" content="csrf-456"></head></html>',
        })
      }

      return Promise.resolve({ ok: true, status: 200 })
    }, 'csrf-123', () => '/')

    await wrappedFetch('/api/config', { method: 'POST' })

    expect(calls).toHaveLength(3)
    expect(calls[0].options.headers['X-CSRF-Token']).toBe('csrf-123')
    expect(calls[1].url).toBe('/')
    expect(calls[2].options.headers['X-CSRF-Token']).toBe('csrf-456')
    expect(document.querySelector('meta[name="csrf-token"]').content).toBe('csrf-456')
  })

  it('cancels an indeterminate response body before retrying later', async () => {
    const cancel = vi.fn(async () => {})
    const result = await probeWebUiAuth(async () => ({
      body: { cancel },
      headers: new Headers({ 'content-type': 'text/plain' }),
      ok: false,
      redirected: false,
      status: 503,
      url: 'https://127.0.0.1:47990/api/config',
    }))

    expect(result).toEqual({ state: AUTH_PROBE_STATE.unavailable })
    expect(cancel).toHaveBeenCalledOnce()
  })


  it('requires the authenticated config response contract', async () => {
    const response = (body) => ({
      body: { cancel: vi.fn(async () => {}) },
      headers: new Headers({ 'content-type': 'application/json' }),
      json: vi.fn(async () => body),
      ok: true,
      redirected: false,
      status: 200,
      url: 'https://127.0.0.1:47990/api/config',
    })

    for (const body of [null, [], {}, { status: false }, { status: 'true' }]) {
      await expect(probeWebUiAuth(async () => response(body))).resolves.toEqual({
        state: AUTH_PROBE_STATE.unavailable,
      })
    }

    await expect(probeWebUiAuth(async () => ({
      ...response({ status: true }),
      json: vi.fn(async () => { throw new SyntaxError('malformed JSON') }),
    }))).resolves.toEqual({ state: AUTH_PROBE_STATE.unavailable })

    const config = { status: true, platform: 'linux' }
    await expect(probeWebUiAuth(async () => response(config))).resolves.toEqual({
      state: AUTH_PROBE_STATE.authenticated,
      config,
    })
  })

  it('accepts only the application/json media type for authenticated config', async () => {
    const response = (contentType) => ({
      body: { cancel: vi.fn(async () => {}) },
      headers: new Headers({ 'content-type': contentType }),
      json: vi.fn(async () => ({ status: true, platform: 'linux' })),
      ok: true,
      redirected: false,
      status: 200,
      url: 'https://127.0.0.1:47990/api/config',
    })

    for (const contentType of ['application/json', 'Application/JSON ; charset=UTF-8']) {
      await expect(probeWebUiAuth(async () => response(contentType))).resolves.toMatchObject({
        state: AUTH_PROBE_STATE.authenticated,
      })
    }

    for (const contentType of ['application/jsonp', 'application/json-patch+json', 'text/application/json']) {
      await expect(probeWebUiAuth(async () => response(contentType))).resolves.toEqual({
        state: AUTH_PROBE_STATE.unavailable,
      })
    }
  })

  it('rejects case and trailing-slash variants of public return targets', () => {
    for (const target of ['/LOGIN', '/login/', '/Reconnecting', '/reconnecting/', '/WELCOME/', '/Recover/']) {
      expect(getSafeAuthReturnTarget(target)).toBe('/')
    }
    expect(getSafeAuthReturnTarget('//attacker.example')).toBe('/')
    expect(getSafeAuthReturnTarget('/apps?filter=recent')).toBe('/apps?filter=recent')
  })

  it('preserves Vue Router history state while removing polarisReload', () => {
    const replaceState = vi.fn()
    const routerState = { back: '/login', current: '/apps', forward: null, position: 4 }

    expect(clearPolarisReloadParam(
      { href: 'https://polaris.test/?foo=1&polarisReload=123#/apps' },
      replaceState,
      routerState
    )).toBe(true)

    expect(replaceState).toHaveBeenCalledWith(routerState, '', '/?foo=1#/apps')
  })

})
