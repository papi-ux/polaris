import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'
import { readFileSync } from 'node:fs'

import ConfigView from './views/ConfigView.vue'

vi.mock('./composables/useToast', () => ({ useToast: () => ({ toast: vi.fn() }) }))
vi.mock('./restart-host.js', () => ({ requestHostRestart: vi.fn(() => Promise.resolve()) }))

// The real English strings, so a missing locale key fails here instead of rendering a raw key.
const en = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const i18n = {
  t(key, params = {}) {
    const value = key.split('.').reduce((node, part) => node?.[part], en)
    if (typeof value !== 'string') return key
    return value.replace(/\{(\w+)\}/g, (_, name) => String(params[name] ?? ''))
  },
}

let wrapper

function mountConfigView(config = {}) {
  global.fetch = vi.fn(() => Promise.resolve({
    status: 200,
    json: () => Promise.resolve({
      platform: 'linux',
      status: {},
      version: 'test',
      sunshine_name: 'Host',
      max_bitrate: 0,
      configuration_revision: 'a'.repeat(64),
      client_settings_live_fields: ['max_bitrate'],
      client_settings_restart_fields: ['sunshine_name'],
      ...config,
    }),
  }))
  wrapper = shallowMount(ConfigView, {
    attachTo: document.body,
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t.bind(i18n) },
      stubs: {
        Skeleton: { template: '<div />' },
        General: { template: '<div />' },
        AudioVideo: { template: '<div />' },
        Inputs: { template: '<div />' },
        Network: { template: '<div />' },
        Files: { template: '<div />' },
        Advanced: { template: '<div />' },
        AiOptimizer: { template: '<div />' },
        ContainerEncoders: { template: '<div />' },
        Recording: false,  // The real tab
      },
    },
  })
  return wrapper
}

async function settle() {
  for (let i = 0; i < 4; i += 1) {
    await Promise.resolve()
    await nextTick()
  }
  await flushPromises()
}

async function openRecordingTab(config) {
  mountConfigView(config)
  await settle()
  wrapper.vm.currentTab = 'recording'
  await settle()
}

function requestedUrls() {
  return global.fetch.mock.calls.map(([url]) => String(url))
}

function postedConfig() {
  const call = global.fetch.mock.calls.findLast(([url, init]) => url === './api/config' && init?.method === 'POST')
  return call ? JSON.parse(call[1].body) : null
}

describe('host recording settings', () => {
  afterEach(() => {
    wrapper?.unmount()
    wrapper = null
  })

  it('defaults to Off and states the raw-video limits in the real copy', async () => {
    await openRecordingTab()
    expect(wrapper.vm.config.recording_enabled).toBe('disabled')
    expect(wrapper.vm.config.recording_replay_buffer).toBe('disabled')
    expect(wrapper.get('#recording_policy_off').element.checked).toBe(true)
    expect(wrapper.get('[data-recording-format-notice]').text()).toBe(en.config.recording_format_notice)
    expect(en.config.recording_format_notice).toMatch(/no audio, no container, no timestamps/)
    expect(wrapper.get('[data-recording-policy-option="off"]').text()).toContain(en.config.recording_policy_off_desc)
    expect(wrapper.get('[data-recording-policy-option="manual"]').text()).toContain('does not start a file')
    expect(wrapper.get('#recording_replay_buffer_minutes').element.value).toBe('5')
  })

  it('shows a saved replay policy as Replay buffer', async () => {
    await openRecordingTab({ recording_enabled: 'enabled', recording_replay_buffer: 'enabled', recording_replay_buffer_minutes: 12 })
    expect(wrapper.get('#recording_policy_replay').element.checked).toBe(true)
    expect(wrapper.get('#recording_replay_buffer_minutes').element.value).toBe('12')
    expect(wrapper.vm.hasUnsavedChanges).toBe(false)
  })

  it('follows refreshed settings and edits the current configuration object', async () => {
    await openRecordingTab()
    const previous = wrapper.vm.config
    wrapper.vm.config = { ...previous, recording_enabled: 'enabled', recording_replay_buffer: 'enabled' }
    await settle()
    expect(wrapper.get('#recording_policy_replay').element.checked).toBe(true)
    await wrapper.get('#recording_policy_manual').setValue()
    await settle()
    expect(wrapper.vm.config.recording_replay_buffer).toBe('disabled')
    expect(previous.recording_enabled).toBe('disabled')
    expect(previous.recording_replay_buffer).toBe('disabled')
  })

  it('reset restores Off without starting a recorder and subsequent edits still save', async () => {
    await openRecordingTab()
    await wrapper.get('#recording_policy_replay').setValue()
    wrapper.vm.resetLocalChanges()
    await settle()
    expect(wrapper.get('#recording_policy_off').element.checked).toBe(true)
    expect(wrapper.vm.hasUnsavedChanges).toBe(false)
    await wrapper.get('#recording_policy_manual').setValue()
    await wrapper.vm.save()
    await settle()
    expect(postedConfig().recording_enabled).toBe('enabled')
    expect(requestedUrls().some((url) => url.includes('/api/recording'))).toBe(false)
  })

  it('maps Manual continuous to the existing keys as a pending, restart-needed change only', async () => {
    await openRecordingTab()
    await wrapper.get('#recording_policy_manual').setValue()
    await settle()
    expect(wrapper.vm.config.recording_enabled).toBe('enabled')
    expect(wrapper.vm.config.recording_replay_buffer).toBe('disabled')
    const change = wrapper.vm.pendingChanges.find((entry) => entry.key === 'recording_enabled')
    expect(change).toBeTruthy()
    expect(change.tabId).toBe('recording')
    expect(change.label).toBe(en.config.recording_enabled)
    expect(change.impact).toBe(en.config.pending_changes_restart_required)
    expect(wrapper.vm.pendingChanges.map((entry) => entry.key)).toEqual(['recording_enabled'])
  })

  it('saves only the changed recording keys and never calls the recorder', async () => {
    await openRecordingTab()
    await wrapper.get('#recording_policy_replay').setValue()
    await wrapper.get('#recording_replay_buffer_minutes').setValue('15')
    await wrapper.get('#recording_output_dir').setValue('/srv/clips')
    await wrapper.vm.save()
    await settle()
    const body = postedConfig()
    expect(body).toMatchObject({
      recording_enabled: 'enabled',
      recording_replay_buffer: 'enabled',
      recording_replay_buffer_minutes: 15,
      recording_output_dir: '/srv/clips',
    })
    expect(requestedUrls().some((url) => url.includes('/api/recording'))).toBe(false)
  })

  it('turning the policy back Off writes neither key, so the defaults apply', async () => {
    await openRecordingTab({ recording_enabled: 'enabled', recording_replay_buffer: 'enabled' })
    await wrapper.get('#recording_policy_off').setValue()
    await settle()
    expect(wrapper.vm.config.recording_enabled).toBe('disabled')
    expect(wrapper.vm.config.recording_replay_buffer).toBe('disabled')
    await wrapper.vm.save()
    await settle()
    const body = postedConfig()
    expect(body).not.toBeNull()
    expect(body).not.toHaveProperty('recording_enabled')
    expect(body).not.toHaveProperty('recording_replay_buffer')
  })

  it('opening and editing the tab sends nothing until Save, and never a recorder request', async () => {
    await openRecordingTab()
    const before = global.fetch.mock.calls.length
    await wrapper.get('#recording_policy_manual').setValue()
    await wrapper.get('#recording_policy_replay').setValue()
    await wrapper.get('#recording_output_dir').setValue('/tmp/x')
    await settle()
    expect(global.fetch.mock.calls.length).toBe(before)
    expect(requestedUrls().some((url) => url.includes('/api/recording'))).toBe(false)
    // The tab offers no start, stop or save action.
    expect(wrapper.get('#recording').findAll('button')).toHaveLength(0)
  })

  it('is found by search and focuses the policy', async () => {
    mountConfigView()
    await settle()
    wrapper.vm.searchQuery = 'replay buffer'
    await settle()
    expect(wrapper.vm.matchingTabs.map((tab) => tab.id)).toContain('recording')
    expect(wrapper.vm.currentTab).toBe('recording')
    expect(['recording_enabled', 'recording_replay_buffer', 'recording_replay_buffer_minutes'])
      .toContain(wrapper.vm.matchedOptionByTab.recording)
  })
})
