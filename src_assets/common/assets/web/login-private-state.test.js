import { flushPromises, mount } from '@vue/test-utils'
import { createMemoryHistory, createRouter } from 'vue-router'
import { afterEach, describe, expect, it, vi } from 'vitest'

import LoginView from './views/LoginView.vue'
import SettingsUnreadableBanner from './components/SettingsUnreadableBanner.vue'
import { readConfigOrNull } from './config-cache.js'
import { initializeWebUiAuthState } from './auth-state.js'
import { probeWebUiAuth } from './router-helpers.js'

const parent = '/home/ubuntu/.config'
const reason = `A folder on the way to it, ${parent}, is writable by group or other users (mode 0775), so Polaris will not keep private state there.`
const fix = `Restrict it with "chmod 700 ${parent}"; a umask of 002 is enough to leave it group writable.`
const error = `The Web UI session could not be persisted. ${reason} ${fix}`
const $t = key => key

function reply(body) {
  return {
    ok: false,
    status: 503,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: async () => body,
  }
}

afterEach(() => {
  vi.unstubAllGlobals()
  initializeWebUiAuthState()
  localStorage.clear()
})

describe('private state refused by a parent directory (#550)', () => {
  it('renders the login refusal and exact remedy without navigating to the dashboard', async () => {
    const router = createRouter({ history: createMemoryHistory(), routes: [{ path: '/login', component: LoginView }] })
    await router.push('/login')
    await router.isReady()
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, code: 'private_state_unavailable', error, reason, fix })))
    const wrapper = mount(LoginView, { global: { plugins: [router], mocks: { $t } } })
    await wrapper.find('#usernameInput').setValue('ubuntu')
    await wrapper.find('#passwordInput').setValue('fixture-password')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(wrapper.text()).toContain(error)
    expect(wrapper.text()).toContain(`chmod 700 ${parent}`)
    expect(router.currentRoute.value.path).toBe('/login')
    expect(localStorage.getItem('login')).toBe(null)
    wrapper.unmount()
  })

  it('keeps a session-storage refusal unavailable at the auth probe', async () => {
    // config_unreadable means authentication succeeded; private session state
    // must never be mislabeled as that authenticated settings-only refusal.
    const fetchImpl = vi.fn(async () => reply({ status: false, code: 'private_state_unavailable', error, reason, fix }))
    const result = await probeWebUiAuth(fetchImpl, 'https://polaris.test/')
    expect(fetchImpl).toHaveBeenCalledOnce()
    expect(result.state).toBe('unavailable')
  })

  it('shows the same parent remedy through Mission Control’s existing settings-readiness banner', async () => {
    const wrapper = mount(SettingsUnreadableBanner, { global: { mocks: { $t } } })
    await expect(readConfigOrNull(reply({ status: false, error: 'config_unreadable', path: `${parent}/polaris/polaris.conf`, reason, fix }))).resolves.toBe(null)
    await flushPromises()
    expect(wrapper.find('[data-settings-unreadable-banner-reason]').text()).toBe(reason)
    expect(wrapper.find('[data-settings-unreadable-banner-fix]').text()).toBe(fix)
    wrapper.unmount()
  })
})
