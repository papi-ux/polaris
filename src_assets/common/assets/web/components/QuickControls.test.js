import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

import QuickControls from './QuickControls.vue'
import { reportSettingsReadable, settingsUnreadable } from '../settings-unreadable.js'

const mockToast = vi.fn()

vi.mock('../composables/useToast', () => ({
  useToast: () => ({ toast: mockToast }),
}))

// #782: a toggle on a refused settings file said only "Unable to update that
// setting.", whether the refusal came on the read the toggle makes first or on
// the save itself.
const refusal = {
  path: '/srv/polaris/polaris.conf',
  reason: 'It is writable by its group (mode 0664), and the settings store refuses a file another user can change.',
  fix: 'Restrict it with "chmod go-w /srv/polaris/polaris.conf".',
}
const revision = 'b'.repeat(64)

function refused() {
  return {
    ok: false,
    status: 503,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: async () => ({ status: false, error: 'config_unreadable', ...refusal }),
  }
}

function settings() {
  return {
    ok: true,
    status: 200,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: async () => ({ status: true, platform: 'linux', configuration_revision: revision, enable_discovery: 'enabled' }),
  }
}

function mountControls() {
  const i18n = { t: (key) => key }
  return shallowMount(QuickControls, { global: { provide: { i18n }, mocks: { $t: i18n.t } } })
}

function discoveryToggle(wrapper) {
  return wrapper.findAll('button').find((button) => button.text().includes('quick_controls.items.enable_discovery.label'))
}

describe('Quick Controls on a settings file the host refuses', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
    mockToast.mockClear()
    reportSettingsReadable()
  })

  it('names the reason and the fix when the host refuses the save, and keeps its own words for other failures', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => {})
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(settings())
      .mockResolvedValueOnce(settings())
      .mockResolvedValueOnce(refused())
      .mockResolvedValueOnce(settings())
      .mockResolvedValueOnce({
        ok: false,
        status: 500,
        headers: new Headers({ 'content-type': 'text/plain' }),
        json: async () => { throw new SyntaxError('not JSON') },
      }))
    const wrapper = mountControls()
    await flushPromises()
    const toggle = discoveryToggle(wrapper)
    expect(toggle.attributes('aria-pressed')).toBe('true')

    await toggle.trigger('click')
    await flushPromises()

    const [, request] = fetch.mock.calls[2]
    expect(request.method).toBe('PATCH')
    expect(request.headers['If-Match']).toBe(`"${revision}"`)
    expect(mockToast).toHaveBeenCalledTimes(1)
    expect(mockToast).toHaveBeenCalledWith(`quick_controls.save_unreadable ${refusal.reason} ${refusal.fix}`, 'error', 12000)
    expect(settingsUnreadable.value).toEqual(refusal)
    expect(discoveryToggle(wrapper).attributes('aria-pressed')).toBe('true')

    // Once the file reads again, a save that fails for another reason keeps the old words.
    mockToast.mockClear()
    await discoveryToggle(wrapper).trigger('click')
    await flushPromises()
    expect(mockToast).toHaveBeenCalledTimes(1)
    expect(mockToast).toHaveBeenCalledWith('quick_controls.save_failed', 'error')
    expect(settingsUnreadable.value).toBe(null)
    wrapper.unmount()
  })

  it('names them when the read the toggle makes first is refused, and saves nothing', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => {})
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(settings())
      .mockResolvedValueOnce(refused()))
    const wrapper = mountControls()
    await flushPromises()

    await discoveryToggle(wrapper).trigger('click')
    await flushPromises()

    expect(fetch).toHaveBeenCalledTimes(2)
    expect(mockToast).toHaveBeenCalledWith(`quick_controls.save_unreadable ${refusal.reason} ${refusal.fix}`, 'error', 12000)
    expect(settingsUnreadable.value).toEqual(refusal)
    wrapper.unmount()
  })
})
