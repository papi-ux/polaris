import { mount, flushPromises } from '@vue/test-utils'
import { ref } from 'vue'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import DashboardView from './DashboardView.vue'

// Mission Control's frame-health pill read "target interval error" for every stream, including a
// source-paced one whose figure is encode handoff against the source's own intervals.
const shared = vi.hoisted(() => ({ stats: null, connected: null }))
vi.mock('../composables/useStreamStats', () => ({ useStreamStats: () => ({ stats: shared.stats, connected: shared.connected, receivedAt: ref(0) }) }))
vi.mock('../composables/useLiveTuning', () => ({ useLiveTuning: () => ({ label: ref('Off') }) }))
vi.mock('../composables/useSystemStats', () => ({ useSystemStats: () => ({ gpu: ref(null), displays: ref([]), audio: ref(null), sessionType: ref(''), readiness: ref({ displays: 'unavailable', audio: 'unavailable' }), refresh: vi.fn(async () => {}) }) }))
vi.mock('../composables/useSessionHistory', () => ({ useSessionHistory: () => ({ sessions: ref([]), clearHistory: vi.fn(), activeStartedAt: ref(null) }), formatDuration: String }))
vi.mock('../composables/useAiOptimizer', () => ({ useAiOptimizer: () => ({ status: ref(null), fetchStatus: vi.fn(), fetchDevices: vi.fn() }) }))
vi.mock('../composables/useFavicon', () => ({ useFavicon: vi.fn() }))
vi.mock('../client-embed.js', async importOriginal => ({ ...(await importOriginal()), followClientLink: vi.fn() }))
const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))

const live = extra => ({ streaming: true, clients: [{ stream_instance_id: 'process-a:4' }], fps: 120, bitrate_kbps: 20000,
  latency_ms: 6, encode_time_ms: 3, packet_loss: 0, dropped_frame_ratio: 0, duplicate_frame_ratio: 0,
  frame_interval_error_ms: 0.6, frame_jitter_ms: 0.6, ...extra })
let wrapper
beforeEach(() => {
  vi.useFakeTimers()
  shared.stats = ref(null)
  shared.connected = ref(true)
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} })
  vi.stubGlobal('matchMedia', () => ({ matches: true, addEventListener() {}, removeEventListener() {} }))
  vi.stubGlobal('fetch', vi.fn(async url => ({ ok: true, json: async () => (url === './api/apps' ? { apps: [], current_app: '' } : []) })))
})
afterEach(() => { wrapper?.unmount(); wrapper = null; vi.unstubAllGlobals(); vi.restoreAllMocks(); vi.useRealTimers() })

async function render(stats) {
  const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
  wrapper = mount(DashboardView, { attachTo: document.body, global: { plugins: [i18n], provide: { i18n: i18n.global },
    stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' }, GaugeArc: true, QuickControls: true, ConfirmActionDialog: true } } })
  await flushPromises()
  shared.stats.value = stats
  await flushPromises()
  await vi.dynamicImportSettled()
  await flushPromises()
}
const pill = () => wrapper.get('[data-dashboard-frame-health] [data-frame-interval-basis]')

describe('Mission Control frame-health basis', () => {
  it('names a fixed-rate target interval', async () => {
    await render(live())
    expect(pill().attributes('data-frame-interval-basis')).toBe('fixed_grid')
    expect(pill().text()).toBe('Fixed-rate · 0.6 ms target interval error')
    expect(pill().attributes('title')).toMatch(/^Fixed-rate: .*requested frame interval/)
  })

  it('names the source-relative encode handoff, and shows no figure without current timing', async () => {
    const measurement = { basis: 'source_relative_encode_handoff', available: true, receiver_presentation_verified: false }
    await render(live({ frame_pacing_measurement: measurement }))
    expect(pill().attributes('data-frame-interval-basis')).toBe('source_time')
    expect(pill().text()).toBe('Source-paced · 0.6 ms handoff error')
    expect(pill().attributes('title')).toMatch(/^Source-paced: .*encoder handoff intervals.*not verified/)
    expect(wrapper.get('[data-dashboard-frame-health]').text()).not.toMatch(/target interval|VRR/)

    shared.stats.value = live({ frame_interval_error_ms: 7, frame_pacing_measurement: { ...measurement, available: false } })
    await flushPromises()
    expect(pill().text()).toBe('Source-paced · Waiting for timing evidence')
    expect(pill().classes()).not.toContain('text-warning')
  })
})
