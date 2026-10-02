import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'

import ConfigView from './views/ConfigView.vue'
import { requestHostRestart } from './restart-host.js'
import { reportSettingsReadable, settingsUnreadable } from './settings-unreadable.js'

const mockToast = vi.fn()

vi.mock('./composables/useToast', () => ({
  useToast: () => ({ toast: mockToast }),
}))

vi.mock('./restart-host.js', () => ({
  requestHostRestart: vi.fn(() => Promise.resolve()),
}))

const messages = {
  'navbar.settings': 'Settings',
  'config.configuration_desc': 'Tune settings',
  'config.visible_tabs': ({ count, total }) => `${count} of ${total} sections visible`,
  'config.unsaved_changes': 'Unsaved changes',
  'config.all_changes_saved': 'All changes saved',
  'config.action_center': 'Action Center',
  'config.search_placeholder': 'Search settings',
  'config.pending_badge': 'Pending save',
  'config.synced_badge': 'Synced',
  'config.reset_local': 'Reset Local Changes',
  '_common.save': 'Save',
  '_common.apply': 'Apply',
  'config.settings_map_kicker': 'Settings Map',
  'config.settings_map_title': 'Tune Polaris by area',
  'config.settings_map_desc': 'Move between sections',
  'config.unsaved_banner': 'You have pending host changes.',
  'config.saved_banner': 'No pending host changes.',
  'config.command_unsaved_note': 'Save stages your current edits.',
  'config.command_saved_note': 'Browse by area or search for a setting.',
  'config.pending_changes_kicker': 'Pending changes',
  'config.pending_changes_title': 'Review local edits before saving',
  'config.pending_changes_desc': ({ count }) => `${count} local edit${count === 1 ? '' : 's'} ready to review.`,
  'config.pending_changes_empty_title': 'No local edits yet',
  'config.pending_changes_empty_desc': 'Changed settings will appear here before you save.',
  'config.pending_changes_before': 'Before',
  'config.pending_changes_after': 'After',
  'config.pending_changes_jump': 'Jump to setting',
  'config.pending_changes_reset_one': 'Reset this change',
  'config.pending_changes_apply_required': 'Save + Apply restart required',
  'config.pending_changes_live_apply': 'Applies after Save',
  'config.search_results': ({ query, count }) => `Showing ${query} in ${count}`,
  'config.sunshine_name': 'Polaris Name',
  'config.max_bitrate': 'Maximum Bitrate',
}

const i18n = {
  t(key, params = {}) {
    const value = messages[key]
    if (typeof value === 'function') return value(params)
    return value || key
  },
}

function flushConfigLoad() {
  return Promise.resolve().then(() => Promise.resolve()).then(() => nextTick())
}

function mountConfigView(config = {}, firstResponse = null) {
  global.fetch = vi.fn(() => Promise.resolve({
    status: 200,
    json: () => Promise.resolve({
      platform: 'linux',
      status: {},
      version: 'test',
      vdisplayStatus: '1',
      sunshine_name: 'Old Host',
      max_bitrate: 0,
      client_settings_live_fields: ['max_bitrate'],
      client_settings_restart_fields: ['sunshine_name'],
      ...config,
    }),
  }))
  if (firstResponse) global.fetch.mockResolvedValueOnce(firstResponse)

  return shallowMount(ConfigView, {
    attachTo: document.body,
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t.bind(i18n) },
      stubs: {
        Skeleton: { template: '<div />' },
        General: { props: ['config'], template: '<div><input id="sunshine_name" data-setting-key="sunshine_name" v-model="config.sunshine_name"></div>' },
        AudioVideo: { name: 'AudioVideo', props: ['config'], template: '<div><input id="max_bitrate" data-setting-key="max_bitrate" v-model="config.max_bitrate"></div>' },
        Inputs: { template: '<div />' },
        Network: { template: '<div />' },
        Files: { template: '<div />' },
        Advanced: { template: '<div />' },
        AiOptimizer: { template: '<div />' },
        ContainerEncoders: { template: '<div />' },
      },
    },
  })
}

describe('ConfigView pending changes review', () => {
  it.each(['onReady', 'onTimeout'])('refreshes host capabilities only after restart readiness (%s)', async (outcome) => {
    const wrapper = mountConfigView()
    await flushConfigLoad()
    wrapper.vm.currentTab = 'av'
    wrapper.vm.config.linux_streaming_output = 'DP-2'
    global.fetch.mockResolvedValueOnce({ status: 200, json: async () => ({ status: true, configuration_revision: 'b'.repeat(64) }) })
    let restartCallbacks
    requestHostRestart.mockImplementationOnce((callbacks) => {
      restartCallbacks = callbacks
      return Promise.resolve()
    })
    wrapper.vm.apply()
    await flushPromises()
    expect(restartCallbacks).toBeDefined()
    expect(wrapper.vm.hostGeneration).toBe(0)
    wrapper.vm.config.max_bitrate = 42000
    restartCallbacks[outcome]()
    await nextTick()
    expect(wrapper.vm.hostGeneration).toBe(outcome === 'onReady' ? 1 : 0)
    expect(wrapper.vm.config.max_bitrate).toBe(42000)
    const tab = wrapper.findComponent({ name: 'AudioVideo' })
    expect(Number(tab.attributes('host-generation'))).toBe(outcome === 'onReady' ? 1 : 0)
    wrapper.unmount()
  })
  it('refreshes legacy capabilities with A/V closed and retains them through Reset Changes', async () => {
    const oldOptions = [{ value: 'host_virtual_display', available: false }]
    const readyOptions = [{ value: 'host_virtual_display', available: true }]
    const wrapper = mountConfigView({ stream_display_mode_options: oldOptions })
    await flushConfigLoad()
    let callbacks
    requestHostRestart.mockImplementationOnce((value) => { callbacks = value; return Promise.resolve() })
    global.fetch.mockResolvedValueOnce({ status: 200, json: async () => ({ status: true }) })
    wrapper.vm.apply()
    await flushPromises()
    expect(wrapper.findComponent({ name: 'AudioVideo' }).exists()).toBe(false)
    wrapper.vm.config.max_bitrate = 42000
    global.fetch.mockResolvedValueOnce({ ok: true, json: async () => ({ stream_display_mode_options: readyOptions, max_bitrate: 1 }) })
    callbacks.onReady()
    await flushPromises()
    expect(wrapper.vm.config.max_bitrate).toBe(42000)
    expect(wrapper.vm.config.stream_display_mode_options).toEqual(readyOptions)
    wrapper.vm.resetLocalChanges()
    await nextTick()
    expect(wrapper.vm.config.max_bitrate).toBe(0)
    expect(wrapper.vm.config.stream_display_mode_options).toEqual(readyOptions)
    wrapper.vm.currentTab = 'av'
    await nextTick()
    expect(wrapper.findComponent({ name: 'AudioVideo' }).props('config').stream_display_mode_options).toEqual(readyOptions)
    wrapper.unmount()
  })

  it.each(['newer restart', 'unmount'])('ignores a retired legacy response after %s', async (reason) => {
    const wrapper = mountConfigView({ stream_display_mode_options: [] })
    await flushConfigLoad()
    const config = wrapper.vm.config
    let callbacks
    requestHostRestart.mockImplementation((value) => { callbacks = value; return Promise.resolve() })
    global.fetch.mockResolvedValueOnce({ status: 200, json: async () => ({ status: true }) })
    wrapper.vm.apply()
    await flushPromises()
    let resolveOld
    global.fetch.mockImplementationOnce(() => new Promise((resolve) => { resolveOld = resolve }))
    callbacks.onReady()
    const readyOptions = [{ value: 'host_virtual_display', available: true }]
    if (reason === 'newer restart') {
      global.fetch.mockResolvedValueOnce({ status: 200, json: async () => ({ status: true }) })
      wrapper.vm.apply()
      await flushPromises()
      global.fetch.mockResolvedValueOnce({ ok: true, json: async () => ({ stream_display_mode_options: readyOptions }) })
      callbacks.onReady()
      await flushPromises()
    } else {
      wrapper.unmount()
    }
    resolveOld({ ok: true, json: async () => ({ stream_display_mode_options: [{ value: 'host_virtual_display', available: false }] }) })
    await flushPromises()
    expect(config.stream_display_mode_options).toEqual(reason === 'newer restart' ? readyOptions : [])
    if (reason === 'newer restart') wrapper.unmount()
  })

  it('re-reads host capabilities when the operator returns to the tab after an outside restart (#633)', async () => {
    const wrapper = mountConfigView({
      vdisplayBackend: 'None',
      vdisplayAvailable: false,
      stream_display_mode_options: [{ value: 'host_virtual_display', available: false }],
    })
    await flushConfigLoad()
    wrapper.vm.config.max_bitrate = 42000
    const readyOptions = [{ value: 'host_virtual_display', available: true }]
    global.fetch.mockResolvedValueOnce({
      ok: true,
      json: async () => ({ stream_display_mode_options: readyOptions, vdisplayBackend: 'kscreen-doctor', vdisplayAvailable: true, max_bitrate: 1 }),
    })
    const now = vi.spyOn(Date, 'now').mockReturnValue(1_000_000)

    document.dispatchEvent(new Event('visibilitychange'))
    await flushPromises()
    expect(wrapper.vm.hostGeneration).toBe(1)
    expect(wrapper.vm.config.stream_display_mode_options).toEqual(readyOptions)
    expect(wrapper.vm.config.vdisplayBackend).toBe('kscreen-doctor')
    expect(wrapper.vm.config.vdisplayAvailable).toBe(true)
    expect(wrapper.vm.config.max_bitrate).toBe(42000)

    // A focus right after the tab switch is the same return, not a second one.
    window.dispatchEvent(new Event('focus'))
    await flushPromises()
    expect(wrapper.vm.hostGeneration).toBe(1)
    now.mockReturnValue(1_010_000)
    window.dispatchEvent(new Event('focus'))
    await flushPromises()
    expect(wrapper.vm.hostGeneration).toBe(2)

    wrapper.unmount()
    now.mockReturnValue(1_100_000)
    document.dispatchEvent(new Event('visibilitychange'))
    await flushPromises()
    expect(wrapper.vm.hostGeneration).toBe(2)
  })

  afterEach(() => {
    document.body.innerHTML = ''
    mockToast.mockClear()
    vi.restoreAllMocks()
    delete global.fetch
  })

  it('keeps the pending review out of the way until a setting changes', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()

    expect(wrapper.find('.settings-pending-review').exists()).toBe(false)
    expect(wrapper.text()).not.toContain('No local edits yet')

    wrapper.vm.config.sunshine_name = 'Changed Host'
    await nextTick()

    expect(wrapper.find('.settings-pending-review').exists()).toBe(true)
  })

  it('shows the host cursor when the setting is absent', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()

    expect(wrapper.vm.config.mouse_cursor_visible).toBe('enabled')
  })

  it('preserves an explicitly disabled host cursor', async () => {
    const wrapper = mountConfigView({ mouse_cursor_visible: 'disabled' })
    await flushConfigLoad()

    expect(wrapper.vm.config.mouse_cursor_visible).toBe('disabled')
  })

  it('summarizes dirty settings with before/after values and impact', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()

    wrapper.vm.config.sunshine_name = 'Nova Host'
    wrapper.vm.config.max_bitrate = 50000
    await nextTick()

    const text = wrapper.text()
    expect(text).toContain('Review local edits before saving')
    expect(text).toContain('2 local edits ready to review.')
    expect(text).toContain('Polaris Name')
    expect(text).toContain('Old Host')
    expect(text).toContain('Nova Host')
    expect(text).toContain('Save + Apply restart required')
    expect(text).toContain('Maximum Bitrate')
    expect(text).toContain('0')
    expect(text).toContain('50000')
    expect(text).toContain('Applies after Save')
  })

  it('can jump to a changed setting and reset one local edit', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()

    wrapper.vm.config.sunshine_name = 'Nova Host'
    wrapper.vm.config.max_bitrate = 50000
    await nextTick()

    await wrapper.find('[data-pending-change-jump="max_bitrate"]').trigger('click')
    await nextTick()
    expect(wrapper.vm.currentTab).toBe('av')

    await wrapper.find('[data-pending-change-reset="sunshine_name"]').trigger('click')
    await nextTick()

    expect(wrapper.vm.config.sunshine_name).toBe('Old Host')
    expect(wrapper.text()).not.toContain('Polaris Name')
    expect(wrapper.text()).toContain('Maximum Bitrate')
  })

  it('strips response-only stream path metadata from config saves', async () => {
    const wrapper = mountConfigView({
      stream_path_id: 'headless_stream',
      stream_path_label: 'Private Stream',
      runtime_backend: 'Labwc',
      vdisplayAvailable: true,
    })
    await flushConfigLoad()

    wrapper.vm.config.sunshine_name = 'Fixed Host'
    await nextTick()

    global.fetch.mockResolvedValueOnce({ status: 200, json: async () => ({ status: true, configuration_revision: 'a'.repeat(64) }) })
    const result = await wrapper.vm.save()

    expect(result).toBe(true)
    const saveRequest = global.fetch.mock.calls.at(-1)
    const body = JSON.parse(saveRequest[1].body)
    expect(body).toHaveProperty('sunshine_name', 'Fixed Host')
    expect(body).not.toHaveProperty('stream_path_id')
    expect(body).not.toHaveProperty('stream_path_label')
    expect(body).not.toHaveProperty('runtime_backend')
    expect(body).not.toHaveProperty('vdisplayAvailable')
  })

  it('preserves credential-presence metadata when resetting local changes', async () => {
    const wrapper = mountConfigView({
      has_ai_api_key: true,
      has_steamgriddb_api_key: true,
      has_api_key: true,
    })
    await flushConfigLoad()

    wrapper.vm.config.sunshine_name = 'Temporary Name'
    await nextTick()
    wrapper.vm.resetLocalChanges()
    await nextTick()

    expect(wrapper.vm.config.has_ai_api_key).toBe(true)
    expect(wrapper.vm.config.has_steamgriddb_api_key).toBe(true)
    expect(wrapper.vm.config.has_api_key).toBe(true)
  })

  it('shows backend validation details when saving configuration fails', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()

    wrapper.vm.config.sunshine_name = 'Broken Host'
    await nextTick()

    global.fetch.mockResolvedValueOnce({
      status: 400,
      text: () => Promise.resolve('Unsupported config key: container_encoders'),
    })

    const result = await wrapper.vm.save()

    expect(result).toBe(false)
    expect(mockToast).toHaveBeenCalledWith(
      'Failed to save configuration: Unsupported config key: container_encoders',
      'error'
    )
  })

  it('uses the generic save failure when the backend response body is empty', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()

    wrapper.vm.config.sunshine_name = 'Broken Host'
    await nextTick()

    global.fetch.mockResolvedValueOnce({
      status: 500,
      text: () => Promise.resolve('   '),
    })

    const result = await wrapper.vm.save()

    expect(result).toBe(false)
    expect(mockToast).toHaveBeenCalledWith('Failed to save configuration', 'error')
  })

  it('does not treat a cancelled SteamGridDB clear as a pending settings change', async () => {
    const wrapper = mountConfigView({ has_steamgriddb_api_key: true })
    await flushConfigLoad()

    wrapper.vm.config.clear_steamgriddb_api_key = false
    await nextTick()

    expect(wrapper.vm.hasUnsavedChanges).toBe(false)
    expect(wrapper.vm.pendingChanges.map((change) => change.key)).not.toContain('clear_steamgriddb_api_key')
  })

  it('posts an explicit SteamGridDB secret clear flag', async () => {
    const wrapper = mountConfigView({ has_steamgriddb_api_key: true })
    await flushConfigLoad()

    wrapper.vm.config.clear_steamgriddb_api_key = true
    wrapper.vm.config.steamgriddb_api_key = ''
    await nextTick()

    global.fetch.mockResolvedValueOnce({ status: 200, json: async () => ({ status: true, configuration_revision: 'a'.repeat(64) }) })

    const result = await wrapper.vm.save()

    expect(result).toBe(true)
    const saveRequest = global.fetch.mock.calls.at(-1)
    expect(saveRequest[0]).toBe('./api/config')
    const body = JSON.parse(saveRequest[1].body)
    expect(body).toHaveProperty('clear_steamgriddb_api_key', true)
    expect(body).toHaveProperty('steamgriddb_api_key', '')
  })
})

// #782: a refused settings file left Settings on its loading skeleton forever.
describe('ConfigView when the host refuses its settings file', () => {
  const refusal = {
    path: '/srv/polaris/polaris.conf',
    reason: 'It is writable by its group (mode 0664), and the settings store refuses a file another user can change.',
    fix: 'Restrict it with "chmod go-w /srv/polaris/polaris.conf".',
  }
  const refused = () => ({
    ok: false,
    status: 503,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: async () => ({ status: false, error: 'config_unreadable', ...refusal }),
  })

  afterEach(() => {
    document.body.innerHTML = ''
    mockToast.mockClear()
    vi.restoreAllMocks()
    delete global.fetch
    reportSettingsReadable()
  })

  // A save on a refused file answered 412 "Settings changed" with If-Match, or
  // 400 "Failed to write config file" without it, and Settings toasted either.
  it('names the reason and the fix when the host refuses to save, and keeps the edits', async () => {
    const wrapper = mountConfigView({ configuration_revision: 'a'.repeat(64) })
    await flushConfigLoad()
    wrapper.vm.config.max_bitrate = 42000
    global.fetch.mockResolvedValueOnce(refused())

    await expect(wrapper.vm.save()).resolves.toBe(false)
    await flushPromises()

    const [url, request] = global.fetch.mock.calls.at(-1)
    expect(url).toBe('./api/config')
    expect(request.method).toBe('POST')
    expect(request.headers['If-Match']).toBe(`"${'a'.repeat(64)}"`)
    expect(mockToast).toHaveBeenCalledTimes(1)
    expect(mockToast).toHaveBeenCalledWith(`config.save_unreadable ${refusal.reason} ${refusal.fix}`, 'error', 12000)
    expect(wrapper.find('[data-settings-unreadable-reason]').text()).toBe(refusal.reason)
    expect(wrapper.find('[data-settings-unreadable-fix]').text()).toBe(refusal.fix)
    expect(wrapper.find('[data-settings-unreadable-desc]').text()).toBe('config.unreadable_desc_loaded')
    expect(wrapper.vm.config.max_bitrate).toBe(42000)
    expect(settingsUnreadable.value).toEqual(refusal)
    wrapper.unmount()
  })

  it('names the file, the reason and the fix instead of loading forever, and loads once it is fixed', async () => {
    const wrapper = mountConfigView({}, refused())
    await flushPromises()

    const panel = wrapper.find('[data-settings-unreadable]')
    expect(panel.exists()).toBe(true)
    expect(panel.attributes('role')).toBe('alert')
    expect(wrapper.find('[data-settings-unreadable-path]').text()).toBe(refusal.path)
    expect(wrapper.find('[data-settings-unreadable-reason]').text()).toBe(refusal.reason)
    expect(wrapper.find('[data-settings-unreadable-fix]').text()).toBe(refusal.fix)
    expect(wrapper.find('[data-settings-unreadable-desc]').text()).toBe('config.unreadable_desc')
    expect(wrapper.find('[data-settings-loading]').exists()).toBe(false)
    expect(wrapper.find('.settings-workspace').exists()).toBe(false)
    expect(wrapper.vm.config).toBe(null)

    await wrapper.find('[data-settings-unreadable-retry]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-settings-unreadable]').exists()).toBe(false)
    expect(wrapper.find('.settings-workspace').exists()).toBe(true)
    expect(wrapper.vm.config.sunshine_name).toBe('Old Host')
    wrapper.unmount()
  })

  it('says so when the host comes back from a restart unable to read its settings, and keeps local edits', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()
    let callbacks
    requestHostRestart.mockImplementationOnce((value) => { callbacks = value; return Promise.resolve() })
    global.fetch.mockResolvedValueOnce({ status: 200, json: async () => ({ status: true }) })
    wrapper.vm.apply()
    await flushPromises()
    wrapper.vm.config.max_bitrate = 42000
    global.fetch.mockResolvedValueOnce(refused())
    callbacks.onReady({ ready: true, attempts: 1, settingsUnreadable: refusal })
    await flushPromises()

    expect(wrapper.find('[data-settings-unreadable-reason]').text()).toBe(refusal.reason)
    expect(wrapper.find('[data-settings-unreadable-fix]').text()).toBe(refusal.fix)
    expect(wrapper.vm.config.max_bitrate).toBe(42000)
    expect(mockToast).toHaveBeenCalledWith('config.restart_ready_unreadable', 'error', 8000)
    expect(mockToast).not.toHaveBeenCalledWith('config.restart_ready', 'success', 5000)
    wrapper.unmount()
  })

  it('says edits cannot be saved while settings stay on screen, and clears once a save goes through', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()
    // Settings are on screen when the file turns unreadable, and the form stays.
    global.fetch.mockResolvedValueOnce(refused())
    wrapper.vm.retrySettings()
    await flushPromises()
    expect(wrapper.find('[data-settings-unreadable]').exists()).toBe(true)
    expect(wrapper.find('.settings-workspace').exists()).toBe(true)
    expect(wrapper.find('[data-settings-unreadable-desc]').text()).toBe('config.unreadable_desc_loaded')

    // The file was fixed and a save went through, which means the host read it.
    wrapper.vm.config.max_bitrate = 42000
    global.fetch.mockResolvedValueOnce({
      status: 200,
      json: async () => ({ status: true, configuration_revision: 'c'.repeat(64), restart_required: false }),
    })
    await expect(wrapper.vm.save()).resolves.toBe(true)
    await flushPromises()
    expect(wrapper.find('[data-settings-unreadable]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('says a settings request failed when the failure is not a refusal', async () => {
    const logged = vi.spyOn(console, 'error').mockImplementation(() => {})
    const wrapper = mountConfigView({}, {
      ok: false,
      status: 500,
      headers: new Headers({ 'content-type': 'text/plain' }),
      json: async () => { throw new SyntaxError('not JSON') },
    })
    await flushPromises()

    expect(wrapper.find('[data-settings-load-failed]').exists()).toBe(true)
    expect(wrapper.find('[data-settings-unreadable]').exists()).toBe(false)
    expect(wrapper.find('[data-settings-loading]').exists()).toBe(false)
    expect(logged).toHaveBeenCalledWith(new Error('Config request failed with status 500'))
    wrapper.unmount()
  })
})
