import { mount, flushPromises } from '@vue/test-utils'
import { ref } from 'vue'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import DashboardView from './DashboardView.vue'
import { RECAP_HOLD_MS } from '../mission-control.js'

// The real session history and post-session report run here; only the transports are fixtures.
const shared = vi.hoisted(() => ({ stats: null, connected: null }))
vi.mock('../composables/useStreamStats', () => ({ useStreamStats: () => ({ stats: shared.stats, connected: shared.connected, receivedAt: ref(0) }) }))
vi.mock('../composables/useLiveTuning', () => ({ useLiveTuning: () => ({ label: ref('Off') }) }))
vi.mock('../composables/useSystemStats', () => ({ useSystemStats: () => ({ gpu: ref(null), displays: ref([]), audio: ref(null), sessionType: ref(''), readiness: ref({ displays: 'unavailable', audio: 'unavailable' }), refresh: vi.fn(async () => {}) }) }))
vi.mock('../composables/useAiOptimizer', () => ({ useAiOptimizer: () => ({ status: ref(null), fetchStatus: vi.fn(), fetchDevices: vi.fn() }) }))
vi.mock('../composables/useFavicon', () => ({ useFavicon: vi.fn() }))
vi.mock('../client-embed.js', async importOriginal => ({ ...(await importOriginal()), followClientLink: vi.fn() }))
const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))

const id = 'process-a:4'
const START = Date.parse('2026-10-26T20:00:00Z')
const live = () => ({ streaming: true, clients: [{ stream_instance_id: id }], fps: 120, bitrate_kbps: 20000, latency_ms: 6, encode_time_ms: 3, packet_loss: 0 })
const ended = (seconds, streamId = id) => ({ state: 'ended', stream_instance_id: streamId,
  started_at: new Date(START).toISOString().replace('.000', ''), ended_at: new Date(START + seconds * 1000).toISOString().replace('.000', '') })
let wrapper
beforeEach(() => {
  vi.useFakeTimers()
  vi.setSystemTime(START - 5000)
  localStorage.clear()
  shared.stats = ref(null)
  shared.connected = ref(true)
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} })
  vi.stubGlobal('matchMedia', () => ({ matches: true, addEventListener() {}, removeEventListener() {} }))
  vi.stubGlobal('fetch', vi.fn(async url => ({ ok: true, json: async () => (url === './api/apps' ? { apps: [], current_app: '' } : []) })))
})
afterEach(() => { wrapper?.unmount(); wrapper = null; localStorage.clear(); vi.unstubAllGlobals(); vi.restoreAllMocks(); vi.useRealTimers() })

async function render(first) {
  const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
  wrapper = mount(DashboardView, { attachTo: document.body, global: { plugins: [i18n], provide: { i18n: i18n.global },
    stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' }, GaugeArc: true, QuickControls: true, ConfirmActionDialog: true } } })
  await flushPromises()
  shared.stats.value = first
  await flushPromises()
}
async function receive(stats, { at, connected = true } = {}) {
  if (at !== undefined) vi.setSystemTime(START + at * 1000)
  shared.connected.value = connected
  shared.stats.value = stats
  await flushPromises()
}
const lastCell = () => wrapper.get('[data-idle-signal="last"]')

describe('Mission Control Last session recap', () => {
  it('stays silent on an idle first load, even beside a matching host record and saved history', async () => {
    localStorage.setItem('polaris_session_history', JSON.stringify([{ started_at: START, ended_at: START + 90000, duration_s: 90,
      client_name: 'Pixel', avg_fps: 118, avg_bitrate_kbps: 21000, quality_grade: 'A' }]))
    vi.setSystemTime(START + 91000)
    await render({ streaming: false, last_session: ended(90) })
    expect(lastCell().text()).toContain('Last session')
    expect(lastCell().classes()).not.toContain('is-recap')
    expect(lastCell().find('a').exists()).toBe(false)
  })

  it('recaps a stream it watched end from that stream’s history, then settles once focus leaves', async () => {
    await render({ streaming: false })
    await receive(live(), { at: 0 })
    await receive(live(), { at: 30 })
    await receive({ streaming: false, last_session: ended(61) }, { at: 61 })
    const cell = lastCell()
    expect(cell.classes()).toContain('is-recap')
    expect(cell.text()).toContain('Stream ended')
    expect(cell.text()).toContain('120 fps avg · 20.0 Mbps avg')
    expect(cell.text()).toContain('1m 1s')
    expect(cell.text()).not.toContain('Live Tuning')
    const link = cell.get('a')
    expect(link.attributes('href')).toBe('/troubleshooting')
    expect(link.attributes('aria-label')).toMatch(/^Stream ended/)
    // Focus holds the recap past its time; it settles to the ordinary cell after focus leaves.
    link.element.focus()
    await vi.advanceTimersByTimeAsync(RECAP_HOLD_MS); await flushPromises()
    expect(lastCell().text()).toContain('Stream ended')
    link.element.blur()
    await vi.advanceTimersByTimeAsync(RECAP_HOLD_MS); await flushPromises()
    expect(lastCell().classes()).not.toContain('is-recap')
    expect(lastCell().text()).toContain('Last session')
    expect(lastCell().find('a').exists()).toBe(false)
  })

  it('does not announce a stream whose feed dropped before the host reported it ended', async () => {
    await render({ streaming: false })
    await receive(live(), { at: 0 })
    await receive(live(), { at: 30 })
    await receive(live(), { at: 31, connected: false })
    await receive({ streaming: false, last_session: ended(61) }, { at: 61 })
    // This browser still records the session; nothing calls it a stream that just finished.
    expect(lastCell().text()).toContain('Last session')
    expect(lastCell().classes()).not.toContain('is-recap')
    expect(lastCell().find('a').exists()).toBe(false)
  })

  it('does not recap when the host record does not span this browser’s entry', async () => {
    await render({ streaming: false })
    await receive(live(), { at: 0 })
    await receive(live(), { at: 30 })
    // The host says the stream ran 15 minutes; this page saw one minute of it.
    await receive({ streaming: false, last_session: { ...ended(61), started_at: '2026-10-26T19:46:00Z' } }, { at: 61 })
    expect(lastCell().text()).toContain('Last session')
    expect(lastCell().classes()).not.toContain('is-recap')
  })
})
