import { readFileSync } from 'node:fs'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const harness = vi.hoisted(() => {
  const app = {
    config: {},
    mount: vi.fn(),
    provide: vi.fn(),
    use: vi.fn(),
  }
  app.use.mockReturnValue(app)

  return {
    afterEach: vi.fn(),
    app,
    beforeEach: vi.fn(),
    onError: vi.fn(),
  }
})

vi.mock('vue', () => ({
  createApp: vi.fn(() => harness.app),
  readonly: vi.fn((value) => value),
  ref: vi.fn((value) => ({ value })),
}))

vi.mock('vue-router', () => ({
  createRouter: vi.fn(() => ({
    afterEach: harness.afterEach,
    beforeEach: harness.beforeEach,
    onError: harness.onError,
  })),
  createWebHashHistory: vi.fn(() => ({})),
}))

vi.mock('./App.vue', () => ({ default: {} }))
vi.mock('./views/DashboardView.vue', () => ({ default: {} }))
vi.mock('./locale', () => ({
  default: vi.fn(async () => ({ global: {} })),
}))
vi.mock('./config-cache.js', () => ({
  clearCachedConfig: vi.fn(),
  primeCachedConfig: vi.fn(),
}))

function response(status, body = { status: true }, overrides = {}) {
  return {
    headers: new Headers({ 'content-type': 'application/json' }),
    json: vi.fn(async () => body),
    ok: status >= 200 && status < 300,
    redirected: false,
    status,
    text: vi.fn(async () => JSON.stringify(body)),
    url: 'https://polaris.test/api/config',
    ...overrides,
  }
}

async function loadGuard(fetchImpl) {
  vi.resetModules()
  harness.beforeEach.mockClear()
  window.fetch = fetchImpl
  await import('./main.js')
  expect(harness.beforeEach).toHaveBeenCalledOnce()
  return harness.beforeEach.mock.calls[0][0]
}

describe('router authentication guard', () => {
  beforeEach(() => {
    document.head.innerHTML = '<meta name="csrf-token" content="csrf-test">'
    window.history.replaceState(null, '', '/#/')
    window.__POLARIS_AUTHENTICATED__ = false
  })

  it('keeps a transient network failure out of the login flow', async () => {
    const guard = await loadGuard(vi.fn().mockRejectedValue(new TypeError('Failed to fetch')))

    await expect(guard({ path: '/apps', fullPath: '/apps' }, {})).resolves.toEqual({
      path: '/reconnecting',
      query: { redirect: '/apps' },
    })
  })

  it('keeps a temporary server failure out of the login flow', async () => {
    const guard = await loadGuard(vi.fn().mockResolvedValue(response(503)))

    await expect(guard({ path: '/config', fullPath: '/config' }, {})).resolves.toEqual({
      path: '/reconnecting',
      query: { redirect: '/config' },
    })
  })

  it('fails closed to reconnecting for an unknown future probe state', async () => {
    vi.resetModules()
    const { createWebUiAuthGuard } = await import('./router-auth.js')
    const guard = createWebUiAuthGuard(vi.fn().mockResolvedValue({ state: 'future-state' }))

    await expect(guard({ path: '/apps', fullPath: '/apps' }, {})).resolves.toEqual({
      path: '/reconnecting',
      query: { redirect: '/apps' },
    })
    expect(window.__POLARIS_AUTHENTICATED__).toBe(false)
  })

  it('returns an authenticated login route to its internal target', async () => {
    const guard = await loadGuard(vi.fn().mockResolvedValue(response(200, { status: true, platform: 'linux' })))

    await expect(guard({
      path: '/login',
      fullPath: '/login?redirect=/apps',
      query: { redirect: '/apps' },
    }, {})).resolves.toBe('/apps')
  })

  it('removes the dynamic-import reload marker after authentication succeeds', async () => {
    window.history.replaceState(null, '', '/?polarisReload=123#/apps')
    const guard = await loadGuard(vi.fn().mockResolvedValue(response(200, { status: true, platform: 'linux' })))

    await guard({ path: '/apps', fullPath: '/apps' }, {})

    expect(window.location.search).toBe('')
    expect(window.location.hash).toBe('#/apps')
  })

  it('keeps authenticated cockpit chrome hidden on the reconnect route', () => {
    const appSource = readFileSync('src_assets/common/assets/web/App.vue', 'utf8')

    expect(appSource).toContain("return webUiAuthenticated.value && !isPublicRoute(route.path)")
  })


  it('uses login for an explicit 401 response', async () => {
    const guard = await loadGuard(vi.fn().mockResolvedValue(response(401)))

    await expect(guard({ path: '/apps', fullPath: '/apps' }, {})).resolves.toEqual({
      path: '/login',
      query: { redirect: '/apps' },
    })
  })

  it('uses login for a confirmed server redirect to login', async () => {
    const guard = await loadGuard(vi.fn().mockResolvedValue(response(200, {}, {
      headers: new Headers({ 'content-type': 'text/html' }),
      redirected: true,
      url: 'https://polaris.test/login',
    })))

    await expect(guard({ path: '/config', fullPath: '/config' }, {})).resolves.toEqual({
      path: '/login',
      query: { redirect: '/config' },
    })
  })

  it('preserves first-run routing after a confirmed welcome redirect', async () => {
    const guard = await loadGuard(vi.fn().mockResolvedValue(response(200, {}, {
      headers: new Headers({ 'content-type': 'text/html' }),
      redirected: true,
      url: 'https://polaris.test/welcome',
    })))

    await expect(guard({ path: '/apps', fullPath: '/apps' }, {})).resolves.toBe('/welcome')
  })

  it('treats an unexpected successful HTML response as unavailable', async () => {
    const guard = await loadGuard(vi.fn().mockResolvedValue(response(200, {}, {
      headers: new Headers({ 'content-type': 'text/html' }),
    })))

    await expect(guard({ path: '/apps', fullPath: '/apps' }, {})).resolves.toEqual({
      path: '/reconnecting',
      query: { redirect: '/apps' },
    })
  })


  it('times out a hung initial auth probe and enters reconnecting', async () => {
    vi.useFakeTimers()
    const fetchImpl = vi.fn((_url, options) => new Promise((_resolve, reject) => {
      options.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')))
    }))

    try {
      const guard = await loadGuard(fetchImpl)
      const decision = guard({ path: '/apps', fullPath: '/apps' }, {})
      await Promise.resolve()

      expect(fetchImpl.mock.calls[0][1].signal).toBeInstanceOf(AbortSignal)
      await vi.advanceTimersByTimeAsync(5000)
      await expect(decision).resolves.toEqual({
        path: '/reconnecting',
        query: { redirect: '/apps' },
      })
    } finally {
      vi.useRealTimers()
    }
  })

  it('does not clear authenticated state when a public-route probe returns 403', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response(200, { status: true, platform: 'linux' }))
      .mockResolvedValueOnce(response(403))
    const guard = await loadGuard(fetchImpl)

    await guard({ path: '/apps', fullPath: '/apps' }, {})
    expect(window.__POLARIS_AUTHENTICATED__).toBe(true)

    await expect(guard({ path: '/login', fullPath: '/login', query: {} }, {})).resolves.toBeUndefined()
    expect(window.__POLARIS_AUTHENTICATED__).toBe(true)
    expect(fetchImpl).toHaveBeenCalledTimes(2)
  })

  it('clears stale authenticated state after an explicit rejection on login', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response(200, { status: true, platform: 'linux' }))
      .mockResolvedValueOnce(response(401))
      .mockResolvedValueOnce(response(401))
    const guard = await loadGuard(fetchImpl)

    await guard({ path: '/apps', fullPath: '/apps' }, {})
    expect(window.__POLARIS_AUTHENTICATED__).toBe(true)

    await guard({ path: '/login', fullPath: '/login', query: {} }, {})
    expect(window.__POLARIS_AUTHENTICATED__).toBe(false)

    await expect(guard({ path: '/config', fullPath: '/config' }, {})).resolves.toEqual({
      path: '/login',
      query: { redirect: '/config' },
    })
    expect(fetchImpl).toHaveBeenCalledTimes(3)
  })


  it('does not start App config requests while auth or reconnect routes are mounted', () => {
    const appSource = readFileSync('src_assets/common/assets/web/App.vue', 'utf8')
    const mountedBlock = appSource.match(/onMounted\(\(\) => \{([\s\S]*?)\n\}\)/)?.[1] || ''

    expect(mountedBlock).not.toMatch(/loadAppVersion\(\)[\s\S]*if \(showNav\.value\)/)
    expect(mountedBlock).toMatch(/if \(showNav\.value\) \{[\s\S]*loadAppVersion\(\)[\s\S]*refreshSidebarUpdateStatus\(\)/)
  })

})
