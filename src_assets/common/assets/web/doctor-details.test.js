import { mount, flushPromises } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { ref } from 'vue'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { doctorDetails } from './doctor-details.js'
import DashboardView from './views/DashboardView.vue'

const shared = vi.hoisted(() => ({ stats: null }))
vi.mock('./composables/useStreamStats', () => ({ useStreamStats: () => ({ stats: shared.stats }) }))
vi.mock('./composables/useLiveTuning', () => ({ useLiveTuning: () => ({ label: ref('Off') }) }))
vi.mock('./composables/useSystemStats', () => ({ useSystemStats: () => ({ gpu: ref(null), displays: ref([]), audio: ref(null), sessionType: ref(''), readiness: ref({ displays: 'unavailable', audio: 'unavailable' }), refresh: vi.fn(async () => {}) }) }))
vi.mock('./composables/useSessionHistory', () => ({ useSessionHistory: () => ({ sessions: ref([]), clearHistory: vi.fn(), activeStartedAt: ref(null) }), formatDuration: String }))
vi.mock('./composables/useAiOptimizer', () => ({ useAiOptimizer: () => ({ status: ref({ enabled: true }), fetchStatus: vi.fn(), fetchDevices: vi.fn() }) }))
vi.mock('./composables/useFavicon', () => ({ useFavicon: vi.fn() }))

// The host's frame pacing finding, as the RP6 received it and with this release's wording.
const pacing = {
  summary: 'Frames are not leaving the host at a steady pace, so motion may look uneven.',
  primary_issue: 'frame_pacing',
  traffic_light: 'amber',
  confidence: { level: 'high', score: 0.9, basis: 'video_window' },
  recommendation: {
    title: 'Try this first',
    body: 'Cap the game\'s frame rate at the stream\'s frame rate, or lower its graphics settings, then check again.',
    next_step_label: 'Cap the game\'s frame rate',
    expected_effect: 'Motion should even out, and Doctor\'s next reading should come back steady.',
  },
  safe_recovery_action: {
    id: 'recheck_pacing', label: 'Recheck', kind: 'verification', endpoint: '/api/doctor/action',
    payload_preview: { action_id: 'recheck_pacing', app_session_id: 'session', session_generation: 3 },
  },
  evidence: [
    { id: 'frame_pacing', label: 'Mean target interval error', value: 8.3333, unit: 'ms', status: 'watch', source: 'stream_stats', detail: 'Source-frame timing against the requested interval.' },
    { id: 'streaming', label: 'Active stream', value: true, unit: '', status: 'pass', detail: 'A stream is active.' },
    { id: 'packet_loss', label: 'Video frame loss', value: null, unit: '%', status: 'unknown', detail: 'Not judged yet.' },
  ],
}

describe('doctorDetails', () => {
  it('collects what the verdict rests on', () => {
    const view = doctorDetails(pacing)
    expect(view.expected).toContain('Motion should even out')
    expect(view.confidence).toBe('high')
    expect(view.evidence.map(row => row.id)).toEqual(['frame_pacing', 'streaming', 'packet_loss'])
  })

  it('never reads a missing reading as a zero, and prints only plain readings', () => {
    const [interval, active, loss] = doctorDetails(pacing).evidence
    expect(interval.value).toBe('8.3 ms')
    expect(active.value).toBe('')
    expect(loss.value).toBe('')
    expect(loss.status).toBe('unknown')
  })

  it('stays empty for no report', () => {
    expect(doctorDetails(null)).toEqual({ expected: '', actionNote: '', confidence: '', evidence: [] })
  })

  it('keeps a reason nothing can be pressed only when it says more than the step', () => {
    const same = { recommendation: { body: 'Do this.' }, safe_recovery_action: { unavailable_reason: 'Do this.' } }
    expect(doctorDetails(same).actionNote).toBe('')
    const more = { recommendation: { body: 'Do this.' }, safe_recovery_action: { unavailable_reason: 'Steam profiles are not changed.' } }
    expect(doctorDetails(more).actionNote).toBe('Steam profiles are not changed.')
  })

  it('gives an unknown evidence status the neutral word rather than a grade', () => {
    const [row] = doctorDetails({ evidence: [{ label: 'New row', status: 'surprising' }, { status: 'pass' }, null] }).evidence
    expect(row.status).toBe('unknown')
    expect(doctorDetails({ evidence: 'not a list' }).evidence).toEqual([])
  })
})

describe('Mission Control Doctor', () => {
  const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
  const reply = data => ({ ok: true, json: async () => data })
  let wrapper
  beforeEach(() => {
    shared.stats = ref(null)
    vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} })
    vi.stubGlobal('matchMedia', () => ({ matches: true, addEventListener() {}, removeEventListener() {} }))
    vi.stubGlobal('fetch', vi.fn(async url => reply(url === './api/apps' ? { apps: [], current_app: '' } : url === './api/clients/list' ? { named_certs: [] } : [])))
  })
  afterEach(() => { wrapper?.unmount(); wrapper = null; vi.unstubAllGlobals() })
  async function doctorCard(doctor) {
    const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
    wrapper = mount(DashboardView, { global: { plugins: [i18n], provide: { i18n: i18n.global },
      stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' }, GaugeArc: true, QuickControls: true, ConfirmActionDialog: true } } })
    await flushPromises()
    shared.stats.value = { streaming: true, fps: 60, bitrate_kbps: 10000, encode_time_ms: 2, latency_ms: 5, packet_loss: 0, doctor }
    await flushPromises()
    return wrapper.get('[data-dashboard-doctor]')
  }
  async function openMore(card) {
    const more = card.get('[data-doctor-more]')
    more.element.open = true
    await more.trigger('toggle')
    await vi.dynamicImportSettled()
    await flushPromises()
    return more
  }

  it('leads with the finding, the step and one button', async () => {
    const card = await doctorCard(pacing)
    expect(card.get('.mc-doctor-headline').text()).toBe(pacing.summary)
    expect(card.get('[data-doctor-try]').text()).toBe(`Try this first: ${pacing.recommendation.body}`)
    expect(card.get('[data-doctor-action]').findAll('button').map(button => button.text())).toEqual(['Recheck'])
  })

  it('keeps confidence, evidence and Explain out of sight until asked for', async () => {
    const card = await doctorCard(pacing)
    expect(card.text()).not.toContain('Confidence')
    expect(card.text()).not.toContain('Mean target interval error')
    expect(card.text()).not.toContain('Explain')
    const more = await openMore(card)
    expect(more.get('summary').text()).toBe('Why Doctor says this')
    expect(more.text()).toContain('Confidence: high')
    expect(more.get('[data-doctor-expected]').text()).toContain('Motion should even out')
    expect(more.get('[data-doctor-evidence]').text()).toContain('Mean target interval error · 8.3 ms · Watching')
    expect(more.get('[data-doctor-evidence]').text()).toContain('Video frame loss · Not measured')
    expect(more.get('[data-doctor-explain]').text()).toBe('Explain')
  })

  it('does not suggest a button that is not there, and says why nothing can be pressed', async () => {
    const card = await doctorCard({ ...pacing, safe_recovery_action: { id: 'none', label: 'Manual', kind: 'manual_guidance', unavailable_reason: 'Automatic Steam profile changes are disabled in this release.' } })
    expect(card.find('[data-doctor-action]').exists()).toBe(false)
    expect(card.text()).not.toContain('Suggested')
    const more = await openMore(card)
    expect(more.get('[data-doctor-action-note]').text()).toContain('Automatic Steam profile changes')
  })

  it('still suggests a step it can name but cannot run from this page', async () => {
    const card = await doctorCard({ ...pacing, safe_recovery_action: { id: 'recheck_pacing', label: 'Recheck', kind: 'verification', endpoint: '' } })
    expect(card.get('[data-doctor-action]').text()).toBe('Suggested: Recheck')
  })

  it('drops a step that only repeats the headline, and reads an older host\'s string', async () => {
    const same = await doctorCard({ ...pacing, recommendation: { title: 'Try this first', body: pacing.summary } })
    expect(same.find('[data-doctor-try]').exists()).toBe(false)
    wrapper.unmount()
    const older = await doctorCard({ ...pacing, recommendation: 'Lower the bitrate.' })
    expect(older.get('[data-doctor-try]').text()).toBe('Lower the bitrate.')
  })
})
