import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { initializeWebUiAuthState, isWebUiAuthenticated } from '../auth-state.js'
import ReconnectingView from './ReconnectingView.vue'

const routerHarness = vi.hoisted(() => ({
  query: { redirect: '/apps' },
  replace: vi.fn(),
}))

vi.mock('vue-router', () => ({
  useRoute: () => ({ query: routerHarness.query }),
  useRouter: () => ({ replace: routerHarness.replace }),
}))

function response(status, body = { status: true }) {
  return {
    headers: new Headers({ 'content-type': 'application/json' }),
    json: vi.fn(async () => body),
    ok: status >= 200 && status < 300,
    redirected: false,
    status,
    url: 'https://polaris.test/api/config',
  }
}

describe('ReconnectingView', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn())
    routerHarness.query = { redirect: '/apps' }
    routerHarness.replace.mockReset()
    initializeWebUiAuthState()
    window.history.replaceState(null, '', '/?polarisReload=123#/reconnecting')
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.unstubAllGlobals()
  })

  it('retries a transient failure and returns to the requested route', async () => {
    fetch
      .mockRejectedValueOnce(new TypeError('Failed to fetch'))
      .mockResolvedValueOnce(response(200, { status: true, platform: 'linux' }))

    const wrapper = mount(ReconnectingView)
    await flushPromises()

    expect(wrapper.get('[role="status"]').text()).toContain('Your session has not been cleared')
    expect(routerHarness.replace).not.toHaveBeenCalled()

    await vi.advanceTimersByTimeAsync(500)
    await flushPromises()

    expect(fetch).toHaveBeenCalledTimes(2)
    expect(routerHarness.replace).toHaveBeenCalledWith('/apps')
    expect(window.__POLARIS_AUTHENTICATED__).toBe(true)
    expect(window.location.search).toBe('')
    wrapper.unmount()
  })

  it('uses login only when the server explicitly rejects the session', async () => {
    routerHarness.query = { redirect: '/config' }
    window.__POLARIS_AUTHENTICATED__ = true
    fetch.mockResolvedValueOnce(response(401))

    const wrapper = mount(ReconnectingView)
    await flushPromises()

    expect(routerHarness.replace).toHaveBeenCalledWith({
      path: '/login',
      query: { redirect: '/config' },
    })
    expect(window.__POLARIS_AUTHENTICATED__).toBe(false)
    wrapper.unmount()
  })

  it('clears stale authenticated state before entering first-run setup', async () => {
    window.__POLARIS_AUTHENTICATED__ = true
    fetch.mockResolvedValueOnce({
      ...response(200),
      headers: new Headers({ 'content-type': 'text/html' }),
      redirected: true,
      url: 'https://polaris.test/welcome',
    })

    const wrapper = mount(ReconnectingView)
    await flushPromises()

    expect(routerHarness.replace).toHaveBeenCalledWith('/welcome')
    expect(window.__POLARIS_AUTHENTICATED__).toBe(false)
    wrapper.unmount()
  })

  it('does not navigate to an unsafe return target', async () => {
    routerHarness.query = { redirect: '//example.com/steal' }
    fetch.mockResolvedValueOnce(response(200, { status: true, platform: 'linux' }))

    const wrapper = mount(ReconnectingView)
    await flushPromises()

    expect(routerHarness.replace).toHaveBeenCalledWith('/')
    wrapper.unmount()
  })

  it('times out a hung probe before retrying', async () => {
    fetch
      .mockImplementationOnce((_url, options) => new Promise((_resolve, reject) => {
        options.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')))
      }))
      .mockResolvedValueOnce(response(200, { status: true, platform: 'linux' }))

    const wrapper = mount(ReconnectingView)
    await flushPromises()
    await vi.advanceTimersByTimeAsync(5500)
    await flushPromises()

    const requestCount = fetch.mock.calls.length
    wrapper.unmount()

    expect(requestCount).toBe(2)
  })


  it('caps retry backoff at eight seconds', async () => {
    fetch.mockRejectedValue(new TypeError('Failed to fetch'))

    const wrapper = mount(ReconnectingView)
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(1)

    for (const [index, delay] of [500, 1000, 2000, 4000, 8000, 8000].entries()) {
      await vi.advanceTimersByTimeAsync(delay)
      await flushPromises()
      expect(fetch).toHaveBeenCalledTimes(index + 2)
    }

    wrapper.unmount()
  })

  it('cancels a scheduled retry when the view unmounts', async () => {
    fetch.mockRejectedValueOnce(new TypeError('Failed to fetch'))

    const wrapper = mount(ReconnectingView)
    await flushPromises()
    wrapper.unmount()
    await vi.advanceTimersByTimeAsync(8000)

    expect(fetch).toHaveBeenCalledOnce()
  })

  it('aborts an in-flight probe when the view unmounts', async () => {
    let signal
    fetch.mockImplementationOnce((_url, options) => {
      signal = options.signal
      return new Promise(() => {})
    })

    const wrapper = mount(ReconnectingView)
    await flushPromises()
    wrapper.unmount()

    expect(signal).toBeInstanceOf(AbortSignal)
    expect(signal.aborted).toBe(true)
    expect(routerHarness.replace).not.toHaveBeenCalled()
  })

  it('ignores a late authenticated response after unmount when fetch ignores abort', async () => {
    let resolveFetch
    let signal
    fetch.mockImplementationOnce((_url, options) => {
      signal = options.signal
      return new Promise((resolve) => {
        resolveFetch = resolve
      })
    })

    const wrapper = mount(ReconnectingView)
    await flushPromises()
    wrapper.unmount()

    expect(signal.aborted).toBe(true)
    resolveFetch(response(200, { status: true, platform: 'linux' }))
    await flushPromises()
    await vi.advanceTimersByTimeAsync(8000)

    expect(fetch).toHaveBeenCalledOnce()
    expect(isWebUiAuthenticated()).toBe(false)
    expect(routerHarness.replace).not.toHaveBeenCalled()
  })

})
