import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { flushPromises, mount } from '@vue/test-utils'
import { nextTick } from 'vue'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import SettingsUnreadableBanner from './components/SettingsUnreadableBanner.vue'
import { initializeWebUiAuthState } from './auth-state.js'
import { clearCachedConfig, getCachedConfig } from './config-cache.js'
import { waitForHostReady } from './restart-host.js'
import {
  reportSettingsReadable,
  reportSettingsUnreadable,
  settingsRefusalSentence,
  settingsUnreadable,
  showsSettingsBanner,
} from './settings-unreadable.js'

// #782: the console said a settings file was refused only on the Settings page.
// probeWebUiAuth found it and the router dropped it, and Dashboard, System,
// Apps and the app shell read the refusal as missing data. One banner in
// App.vue now says it wherever the operator is.

const refusal = {
  path: '/srv/polaris/polaris.conf',
  reason: 'It is writable by its group (mode 0664), and the settings store refuses a file another user can change.',
  fix: 'Restrict it with "chmod go-w /srv/polaris/polaris.conf".',
}

function refused(overrides = {}) {
  return {
    ok: false,
    status: 503,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: async () => ({ status: false, error: 'config_unreadable', ...refusal, ...overrides }),
  }
}

function settings(body = { status: true, version: '1.4.14', platform: 'linux' }) {
  return {
    ok: true,
    status: 200,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: async () => body,
  }
}

const $t = (key) => key

describe('the settings refusal the console shares', () => {
  beforeEach(() => {
    initializeWebUiAuthState()
    clearCachedConfig()
  })

  afterEach(() => {
    vi.unstubAllGlobals()
    initializeWebUiAuthState()
  })

  it('is set by a refused settings read and cleared by one that reads', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(refused()).mockResolvedValueOnce(settings()))

    await expect(getCachedConfig()).rejects.toMatchObject({ name: 'SettingsUnreadableError' })
    expect(settingsUnreadable.value).toEqual(refusal)

    await expect(getCachedConfig()).resolves.toMatchObject({ status: true })
    expect(settingsUnreadable.value).toBe(null)
  })

  it('is set by a restart from any page that comes back to a refused file', async () => {
    const fetchImpl = vi.fn().mockResolvedValueOnce(refused()).mockResolvedValueOnce(settings())
    const options = { fetchImpl, sleep: async () => {}, readyPollAttempts: 1, readyPollInitialDelayMs: 0 }

    await expect(waitForHostReady(options)).resolves.toMatchObject({ ready: true, settingsUnreadable: refusal })
    expect(settingsUnreadable.value).toEqual(refusal)

    await expect(waitForHostReady(options)).resolves.toEqual({ ready: true, attempts: 1 })
    expect(settingsUnreadable.value).toBe(null)
  })

  it('is forgotten when the session ends', () => {
    reportSettingsUnreadable(refusal)
    initializeWebUiAuthState()
    expect(settingsUnreadable.value).toBe(null)
  })

  it('words a refused save with the reason and the fix after its own lead', () => {
    expect(settingsRefusalSentence('Settings were not saved.', refusal))
      .toBe(`Settings were not saved. ${refusal.reason} ${refusal.fix}`)
    expect(settingsRefusalSentence('Settings were not saved.', { reason: '', fix: '' }))
      .toBe('Settings were not saved.')
  })
})

describe('the settings unreadable banner', () => {
  beforeEach(() => {
    initializeWebUiAuthState()
  })

  afterEach(() => {
    vi.unstubAllGlobals()
    initializeWebUiAuthState()
  })

  it('names the file, the reason and the fix, and goes once the file reads again', async () => {
    const wrapper = mount(SettingsUnreadableBanner, { global: { mocks: { $t } } })
    expect(wrapper.find('[data-settings-unreadable-banner]').exists()).toBe(false)

    reportSettingsUnreadable(refusal)
    await nextTick()
    const banner = wrapper.find('[data-settings-unreadable-banner]')
    expect(banner.exists()).toBe(true)
    expect(banner.attributes('role')).toBe('alert')
    expect(banner.text()).toContain('config.unreadable_banner_desc')
    expect(wrapper.find('[data-settings-unreadable-banner-path]').text()).toBe(refusal.path)
    expect(wrapper.find('[data-settings-unreadable-banner-reason]').text()).toBe(refusal.reason)
    expect(wrapper.find('[data-settings-unreadable-banner-fix]').text()).toBe(refusal.fix)

    reportSettingsReadable()
    await nextTick()
    expect(wrapper.find('[data-settings-unreadable-banner]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('asks the host again, and keeps the new reason while the file is still refused', async () => {
    const reason = 'It is writable by every user on this machine (mode 0666), and the settings store refuses a file another user can change.'
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(refused({ reason }))
      .mockResolvedValueOnce(settings()))
    reportSettingsUnreadable(refusal)
    const wrapper = mount(SettingsUnreadableBanner, { global: { mocks: { $t } } })
    await nextTick()

    await wrapper.find('[data-settings-unreadable-banner-retry]').trigger('click')
    await flushPromises()
    expect(fetch).toHaveBeenCalledWith('./api/config', { credentials: 'include' })
    expect(wrapper.find('[data-settings-unreadable-banner-reason]').text()).toBe(reason)

    await wrapper.find('[data-settings-unreadable-banner-retry]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-settings-unreadable-banner]').exists()).toBe(false)
    wrapper.unmount()
  })
})

describe('the app shell', () => {
  it('shows the banner on every signed-in page but Settings, which explains the file itself', () => {
    expect(showsSettingsBanner(refusal, '/')).toBe(true)
    expect(showsSettingsBanner(refusal, '/apps')).toBe(true)
    expect(showsSettingsBanner(refusal, '/troubleshooting')).toBe(true)
    expect(showsSettingsBanner(refusal, '/config')).toBe(false)
    expect(showsSettingsBanner(null, '/')).toBe(false)
  })

  it('says settings changes cannot be saved, not every change made in the console', () => {
    // Apps, pairing and the console password are kept in their own files and still save while the
    // settings file is refused, and the banner stands above the Apps page too.
    const locale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))
    const text = locale.config.unreadable_banner_desc
    expect(text).not.toMatch(/no change made in this console/)
    expect(text).toContain('no settings change can be saved')
    expect(text).toContain('Apps, pairing and the console password')
  })

  it('renders the one banner inside the signed-in shell, above the page, by that rule', () => {
    // App.vue imports its wordmark by absolute URL, which vitest cannot resolve,
    // so its wiring is held here by source.
    const app = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/App.vue'), 'utf8')
    const shell = app.slice(app.indexOf('<main id="polaris-main"'), app.indexOf('</main>'))
    expect(app.match(/<SettingsUnreadableBanner\b/g)).toHaveLength(1)
    expect(shell).toContain('<SettingsUnreadableBanner v-if="settingsBannerVisible" />')
    expect(shell.indexOf('<SettingsUnreadableBanner')).toBeLessThan(shell.indexOf('<router-view'))
    expect(app).toContain('const settingsBannerVisible = computed(() => showsSettingsBanner(settingsUnreadable.value, normalizeRoutePath(route.path)))')
  })
})
