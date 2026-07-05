import { shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'

import ConfigView from './views/ConfigView.vue'

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

function mountConfigView(config = {}) {
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

  return shallowMount(ConfigView, {
    attachTo: document.body,
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t.bind(i18n) },
      stubs: {
        Skeleton: { template: '<div />' },
        General: { props: ['config'], template: '<div><input id="sunshine_name" data-setting-key="sunshine_name" v-model="config.sunshine_name"></div>' },
        AudioVideo: { props: ['config'], template: '<div><input id="max_bitrate" data-setting-key="max_bitrate" v-model="config.max_bitrate"></div>' },
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
  afterEach(() => {
    document.body.innerHTML = ''
    mockToast.mockClear()
    vi.restoreAllMocks()
    delete global.fetch
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
})
