import { flushPromises, mount } from '@vue/test-utils'
import { defineComponent, h } from 'vue'
import { createMemoryHistory, createRouter, RouterView } from 'vue-router'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { initializeWebUiAuthState, isWebUiAuthenticated } from './auth-state.js'
import { createWebUiAuthGuard } from './router-auth.js'
import ReconnectingView from './views/ReconnectingView.vue'

const EmptyView = defineComponent({ render: () => h('div') })
const RouterHarness = defineComponent({ render: () => h(RouterView) })

function response(status, body = { status: true }, overrides = {}) {
  return {
    body: { cancel: vi.fn(async () => {}) },
    headers: new Headers({ 'content-type': 'application/json' }),
    json: vi.fn(async () => body),
    ok: status >= 200 && status < 300,
    redirected: false,
    status,
    url: 'https://polaris.test/api/config',
    ...overrides,
  }
}

function makeRouter() {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/', component: EmptyView },
      { path: '/apps', component: EmptyView },
      { path: '/login', component: EmptyView },
      { path: '/welcome', component: EmptyView },
      { path: '/recover', component: EmptyView },
      { path: '/reconnecting', component: ReconnectingView },
    ],
  })
  router.beforeEach(createWebUiAuthGuard())
  return router
}

async function mountRoute(router, path) {
  await router.push(path)
  await router.isReady()
  const wrapper = mount(RouterHarness, { global: { plugins: [router] } })
  await flushPromises()
  return wrapper
}

describe('real router auth recovery', () => {
  beforeEach(() => {
    initializeWebUiAuthState()
    vi.stubGlobal('fetch', vi.fn())
    window.history.replaceState(null, '', '/#/')
  })

  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('hands reconnect success to the protected target without a second probe', async () => {
    fetch
      .mockResolvedValueOnce(response(200, { status: true, platform: 'linux' }))
      .mockResolvedValueOnce(response(503))
    const router = makeRouter()

    const wrapper = await mountRoute(router, '/reconnecting?redirect=/apps')

    expect(router.currentRoute.value.fullPath).toBe('/apps')
    expect(fetch).toHaveBeenCalledOnce()
    expect(isWebUiAuthenticated()).toBe(true)
    wrapper.unmount()
  })

  it('clears shared auth state before routing explicit rejection to login', async () => {
    fetch.mockResolvedValue(response(401))
    const router = makeRouter()

    const wrapper = await mountRoute(router, '/reconnecting?redirect=/apps')

    expect(router.currentRoute.value.path).toBe('/login')
    expect(isWebUiAuthenticated()).toBe(false)
    expect(window.__POLARIS_AUTHENTICATED__).toBe(false)
    wrapper.unmount()
  })

  it('escapes a case-variant stale login route to its safe target', async () => {
    fetch.mockResolvedValue(response(200, { status: true, platform: 'linux' }))
    const router = makeRouter()

    const wrapper = await mountRoute(router, '/LOGIN/?redirect=/apps')

    expect(router.currentRoute.value.fullPath).toBe('/apps')
    expect(fetch).toHaveBeenCalledOnce()
    wrapper.unmount()
  })
})
