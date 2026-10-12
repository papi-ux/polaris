// DOM unit test of the production Doctor & Support view (jsdom, shallowMount): before a stream the
// checklist shows the host's current configuration findings and nothing else. This is not a browser
// run; tests/e2e/doctor-idle-host-findings.spec.js covers the same cases in the mocked lane.
import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'
import TroubleshootingView from './views/TroubleshootingView.vue'

let channel
let wrapper
vi.mock('./composables/useStreamStats', () => ({ useStreamStats: () => channel }))
vi.mock('./composables/useToast', () => ({ useToast: () => ({ toast: vi.fn() }) }))

const t = (key) => key
const waiting = { primary_issue: 'no_active_stream', status: 'unknown', severity: 'info', simple_state: 'Waiting for a stream' }
const setAside = {
  id: 'capture_set_aside_by_mode', severity: 'info',
  message: 'capture override: stream mode [windowed_stream] asks for [wlr], not the configured [kms], because a private labwc session can only be captured through wlroots.',
  action: 'No change is needed. Keep capture = kms if a later mode switch should use it, or leave capture unset to let each mode choose.',
}
const unavailable = {
  id: 'stream_mode_unavailable', severity: 'warning',
  message: 'The configured stream mode [windowed_stream] cannot be entered on this host: labwc binary not found on PATH.',
  action: 'Install what the reason names, or choose a stream mode this host can enter in Settings.',
}
const staleSubstitution = {
  id: 'capture_backend_substituted', severity: 'warning', stale: true,
  message: 'The capture backend this host is configured to use cannot capture anything in the current stream mode, so Polaris substituted another one (kms -> portal).',
  action: 'Either set capture to the substituted backend so the configuration matches what is running, or go back to a private-compositor stream mode if you want the configured one.',
}
const idle = (warnings) => ({
  streaming: false, fps: 0, encode_time_ms: 0, bitrate_kbps: 0, doctor: waiting,
  linux_gpu_profile: { configuration_warnings: warnings },
})
const live = () => ({
  streaming: true,
  clients: [{ stream_instance_id: 'process-a:1', fps: 120 }],
  video_sample_revision: 20, video_policy_sample_count: 20,
  media_loss_sample_revision: 10, media_loss_last_received_age_ms: 100,
  network_sample_revision: 20, network_last_received_age_ms: 100,
  encode_time_ms: 4, encode_target_fps: 120, dropped_frame_ratio: 0, latency_ms: 7,
  capture_gpu_native: true, capture_path: 'wlr_dmabuf',
  network_verdict: {
    loss_basis: 'video_frames_lost_after_fec', loss_state: 'clean', loss_pct: 0,
    frames_expected: 2400, frames_lost: 0, media_samples: 20, window_seconds: 20,
    rtt_state: 'clean', rtt_median_ms: 7, rtt_samples: 20,
  },
  doctor: { primary_issue: 'none', status: 'ok', severity: 'info', simple_state: 'Streaming ready' },
  linux_gpu_profile: { configuration_warnings: [setAside] },
})
const PLACEHOLDERS = [
  'Telemetry is connected, but no active stream is running.',
  'No current confirmed video frame loss measurement is available.',
  'No active connected stream is available to verify capture.',
  'No current encoder timing is available.',
]

async function mountView(stats, connected = true) {
  channel = { stats: ref(stats), connected: ref(connected), receivedAt: ref(connected ? Date.now() : 0) }
  wrapper = shallowMount(TroubleshootingView, {
    global: { provide: { i18n: { t } }, mocks: { $t: t }, directives: { tooltip: {} } },
  })
  await flushPromises()
}
async function receive(stats, connected = true) {
  channel.stats.value = stats
  channel.connected.value = connected
  await nextTick()
}
const diagnosis = () => wrapper.find('[data-doctor-diagnosis]')
const grid = () => wrapper.find('[data-doctor-checklist]')
const cards = () => wrapper.findAll('[data-doctor-checklist] [data-readonly]')

describe('Doctor host findings before a stream, through the production view', () => {
  beforeEach(() => {
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({}), text: async () => '' })))
  })
  afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })

  it('shows the current configured-host findings under a neutral waiting headline, and no live rows', async () => {
    await mountView(idle([setAside, unavailable]))
    expect(diagnosis().classes()).toContain('doctor-diagnosis-idle')
    expect(diagnosis().text()).toContain('troubleshooting.doctor_waiting_title')
    expect(wrapper.find('[data-ai-doctor-explanation]').exists()).toBe(false)
    expect(cards()).toHaveLength(2)
    expect(cards()[0].text()).toContain(unavailable.message)
    expect(cards()[0].text()).toContain('troubleshooting.fix_my_stream_status_warning')
    expect(cards()[1].text()).toContain(setAside.message)
    expect(cards()[1].text()).toContain('troubleshooting.fix_my_stream_status_info')
    const text = grid().text()
    for (const placeholder of PLACEHOLDERS) expect(text).not.toContain(placeholder)
    expect(text).not.toContain('No recent auth or pairing errors')
    expect(text).not.toContain('No recent warnings, fatals, or errors')
  })

  it('shows a capture finding the host has not verified as a note with its recheck step', async () => {
    await mountView(idle([staleSubstitution]))
    expect(cards()).toHaveLength(1)
    expect(cards()[0].text()).toContain('troubleshooting.fix_my_stream_status_info')
    expect(cards()[0].text()).toContain('This capture finding has not been verified for the current settings.')
    expect(cards()[0].text()).toContain('Start a stream to recheck capture with the current settings.')
    expect(cards()[0].text()).not.toContain('set capture to the substituted backend')
  })

  it('stays compact with nothing to show, and after the connection drops', async () => {
    await mountView(idle([]))
    expect(diagnosis().classes()).toContain('doctor-diagnosis-idle')
    expect(grid().exists()).toBe(false)

    await receive(idle([setAside, unavailable]))
    expect(cards()).toHaveLength(2)
    await receive(idle([setAside, unavailable]), false)
    expect(grid().exists()).toBe(false)
    expect(diagnosis().text()).toContain('troubleshooting.doctor_waiting_title')
  })

  it('keeps the whole checklist for a live stream and a failed start', async () => {
    await mountView(live())
    expect(diagnosis().classes()).not.toContain('doctor-diagnosis-idle')
    const liveText = cards().map((card) => card.text())
    expect(liveText.some((text) => text.includes('Live telemetry is connected and a stream is active.'))).toBe(true)
    expect(liveText.some((text) => text.includes(setAside.message))).toBe(true)
    expect(liveText.some((text) => text.includes('No recent auth or pairing errors'))).toBe(true)
    expect(cards().length).toBeGreaterThanOrEqual(7)

    await receive({
      streaming: false,
      doctor: { primary_issue: 'stream_failed_to_start', status: 'needs_action', severity: 'warning', simple_state: 'Needs attention',
        summary: 'The last stream, to Living Room TV, failed to start: the client left during video setup.' },
      linux_gpu_profile: { configuration_warnings: [] },
    })
    expect(diagnosis().classes()).not.toContain('doctor-diagnosis-idle')
    expect(cards()[0].text()).toContain('failed to start')
    expect(cards()[0].text()).toContain('troubleshooting.fix_my_stream_status_fail')
    expect(cards().length).toBeGreaterThanOrEqual(6)
  })
})
