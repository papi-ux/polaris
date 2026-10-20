import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'
import TroubleshootingView from './views/TroubleshootingView.vue'
import { buildPostSessionStreamReport } from './diagnostics-export.js'

let channel
let wrapper
vi.mock('./composables/useStreamStats', () => ({ useStreamStats: () => channel }))
vi.mock('./composables/useToast', () => ({ useToast: () => ({ toast: vi.fn() }) }))

const t = (key) => key === 'troubleshooting.post_session_report' ? 'Post-session Stream Report' : key
const id = 'process-a:1'
const ended = (streamId = id) => ({ state: 'ended', stream_instance_id: streamId, ended_at: '2026-10-04T01:00:10Z' })
const live = (override = {}) => ({
  streaming: true,
  clients: [{ stream_instance_id: id, fps: 120 }],
  video_sample_revision: 20,
  video_policy_sample_count: 20,
  media_loss_sample_revision: 10,
  media_loss_last_received_age_ms: 100,
  network_sample_revision: 20,
  network_last_received_age_ms: 100,
  encode_time_ms: 4,
  encode_target_fps: 120,
  dropped_frame_ratio: 0,
  latency_ms: 7,
  network_verdict: {
    loss_basis: 'video_frames_lost_after_fec', loss_state: 'clean', loss_pct: 0,
    frames_expected: 2400, frames_lost: 0, media_samples: 20, window_seconds: 20,
    rtt_state: 'clean', rtt_median_ms: 7, rtt_samples: 20,
  },
  ...override,
})

async function mountView(stats = { streaming: false }, connected = true) {
  channel = { stats: ref(stats), connected: ref(connected) }
  wrapper = shallowMount(TroubleshootingView, {
    global: { provide: { i18n: { t } }, mocks: { $t: t }, directives: { tooltip: {} } },
  })
  await flushPromises()
}
const card = () => wrapper.findAll('[data-readonly]').find((entry) => entry.text().includes('Post-session Stream Report'))
async function receive(stats, connected = true) {
  channel.stats.value = stats
  channel.connected.value = connected
  await nextTick()
}

describe('post-session report through the production view', () => {
  beforeEach(() => {
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({}), text: async () => '' })))
  })
  afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.useRealTimers() })

  it('requires explicit completion evidence at the public report boundary', () => {
    for (const input of [undefined, {}, { stats: live() }, { snapshot: { origin: 'live-sample', stats: live() } }]) {
      const report = buildPostSessionStreamReport(input)
      expect(report.status).toBe('info')
      expect(report.issueOwner).toBe('unknown')
      expect(report.origin).toBe('unavailable')
    }
  })

  it('keeps a never-streamed idle host unavailable without client blame or zero measurements', async () => {
    await mountView({ streaming: false, packet_loss: 0, latency_ms: 0, encode_time_ms: 0, dropped_frame_ratio: 0 })
    expect(card().text()).toContain('No completed session telemetry is available.')
    expect(card().text()).toContain('Issue owner: unknown')
    expect(card().text()).not.toContain('0.0 ms')
    expect(card().text()).not.toContain('Lower bitrate')
  })

  it('does not archive a disconnected cached stream when an idle payload arrives later', async () => {
    await mountView(live(), false)
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('Issue owner: unknown')
    expect(card().text()).not.toContain('7.0 ms latency')
  })

  it('captures measured zero, detaches the snapshot and keeps its historical quality as time passes', async () => {
    vi.useFakeTimers({ toFake: ['Date'] })
    vi.setSystemTime(new Date('2026-10-04T01:00:00Z'))
    const sample = live()
    await mountView(sample)
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('Historical session telemetry captured at 2026-10-04T01:00:00.000Z')
    expect(card().text()).toContain('7.0 ms latency / 0.0% of video frames lost after FEC / 4.0 ms encode / 0.00% of frames dropped on the host.')
    sample.encode_time_ms = 90
    sample.network_verdict.loss_pct = 90
    vi.setSystemTime(new Date('2026-10-07T01:00:00Z'))
    await receive({ streaming: false, encode_time_ms: 0, last_session: ended() })
    expect(card().text()).toContain('4.0 ms encode')
    expect(card().text()).not.toContain('90.0')
    expect(card().text()).toContain('0.0% of video frames lost after FEC')
  })

  it('retains session-owned capture facts without inventing quality from an ended-only record', async () => {
    await mountView({ streaming: false, encode_time_ms: 0, last_session: {
      ...ended(), capture: { preference: 'kms', requested: 'wlr', opened: 'wlroots', route: 'wlroots',
        transport: 'shm', residency: 'cpu', format: 'bgra8',
        mode_override_reason: 'private_compositor', route_fallback_reason: 'dmabuf_import_failed' },
    } })
    expect(card().text()).toContain('Capture preference: kms')
    expect(card().text()).toContain('Capture requested: wlr')
    expect(card().text()).toContain('Capture initialized: wlroots')
    expect(card().text()).toContain('Route fallback reason: dmabuf_import_failed')
    expect(card().text()).toContain('Historical media quality: unavailable')
    expect(card().text()).toContain('Issue owner: unknown')
    expect(card().text()).not.toContain('0.0 ms encode')
  })

  it('does not attach global quality from simultaneous clients to the latest-ended client', async () => {
    await mountView(live({ clients: [{ stream_instance_id: id }, { stream_instance_id: 'process-a:2' }] }))
    await receive({ streaming: false, last_session: ended('process-a:2') })
    expect(card().text()).toContain('Historical media quality: unavailable')
    expect(card().text()).not.toContain('7.0 ms latency')
  })

  it('does not keep an old report beside a different latest-ended session', async () => {
    await mountView(live())
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('4.0 ms encode')
    await receive({ streaming: false, last_session: ended('process-b:1') })
    expect(card().text()).toContain('Historical media quality: unavailable')
    expect(card().text()).not.toContain('4.0 ms encode')
  })

  it('rejects default, invalid and coerced measurements at completion', async () => {
    await mountView(live({ video_sample_revision: 0, encode_time_ms: '90', dropped_frame_ratio: null,
      media_loss_sample_revision: 0, network_sample_revision: 0,
      network_verdict: { loss_pct: 0, loss_state: 'clean', rtt_median_ms: '70' }, latency_ms: '70' }))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('unknown ms latency / unknown of video frames lost after FEC / unknown ms encode / unknown of frames dropped on the host.')
    expect(card().text()).toContain('Issue owner: unknown')
    expect(card().text()).not.toContain('Lower bitrate')
  })

  it('invalidates active evidence even when a disconnect and reconnect share one Vue update', async () => {
    await mountView(live())
    channel.connected.value = false
    channel.stats.value = { streaming: false, last_session: ended() }
    channel.connected.value = true
    await nextTick()
    expect(card().text()).not.toContain('4.0 ms encode')
  })

  it('does not grade media loss from stale reports beside fresh control pings', async () => {
    await mountView(live({ media_loss_last_received_age_ms: 6000 }))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('7.0 ms latency / unknown of video frames lost after FEC')
    expect(card().text()).toContain('Issue owner: unknown')
  })

  it('preserves the last judged window while a dropped client remains in it, then releases an empty window', async () => {
    const elevated = { ...live().network_verdict, loss_pct: 5, loss_state: 'elevated', frames_lost: 120 }
    await mountView(live({ network_verdict: elevated }))
    const stale = { ...elevated, loss_state: 'stale', loss_pct: null, rtt_state: 'stale', rtt_median_ms: null }
    for (let i = 0; i < 8; ++i) await receive(live({ network_verdict: stale, media_loss_last_received_age_ms: 6000 + i * 1000, latency_ms: 70 }))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain("network pressure when the client's media reports stopped: 5.0%")
    expect(card().text()).toContain('7.0 ms latency / 5.0%')
    await receive(live({ clients: [{ stream_instance_id: 'process-a:2', fps: 120 }], network_verdict: elevated }))
    await receive(live({ clients: [{ stream_instance_id: 'process-a:2', fps: 120 }], network_verdict: {
      ...stale, media_samples: 0, frames_expected: 0, frames_lost: 0,
    }, media_loss_last_received_age_ms: 40000 }))
    await receive({ streaming: false, last_session: ended('process-a:2') })
    expect(card().text()).toContain('unknown of video frames lost after FEC')
    expect(card().text()).toContain('Issue owner: unknown')
  })

  it('resets judged quality when a new session replaces the primary client without an idle payload', async () => {
    await mountView(live({ network_verdict: { ...live().network_verdict, loss_pct: 5, loss_state: 'elevated', frames_lost: 120 } }))
    await receive(live({ clients: [{ stream_instance_id: 'process-a:2', fps: 120 }], network_verdict: {
      ...live().network_verdict, loss_state: 'stale', loss_pct: null,
    }, media_loss_last_received_age_ms: 7000 }))
    await receive({ streaming: false, last_session: ended('process-a:2') })
    expect(card().text()).not.toContain('5.0%')
    expect(card().text()).toContain('unknown of video frames lost after FEC')
  })

  it.each([null, '0', '90', NaN, Infinity, -1])('rejects invalid metric value %s', async (value) => {
    await mountView(live({ encode_time_ms: value, dropped_frame_ratio: value, latency_ms: value,
      network_verdict: { ...live().network_verdict, loss_pct: value, rtt_median_ms: value } }))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('unknown ms latency / unknown of video frames lost after FEC / unknown ms encode / unknown of frames dropped on the host.')
    expect(card().text()).toContain('Issue owner: unknown')
  })

  it('preserves a genuine judged zero round trip while rejecting zero encoder timing', async () => {
    await mountView(live({ encode_time_ms: 0, latency_ms: 0,
      network_verdict: { ...live().network_verdict, rtt_median_ms: 0 } }))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('0.0 ms latency / 0.0% of video frames lost after FEC / unknown ms encode / 0.00% of frames dropped on the host.')
  })

  it('observes an in-place completion without turning Vue oldValue into the archived payload', async () => {
    await mountView(live())
    channel.stats.value.streaming = false
    channel.stats.value.last_session = ended()
    channel.stats.value.encode_time_ms = 0
    await nextTick()
    expect(card().text()).toContain('4.0 ms encode')
  })

  it('requires current client video evidence before grading a default zero host-drop ratio', async () => {
    await mountView(live({ clients: [{ stream_instance_id: id, fps: 0 }] }))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('unknown ms encode / unknown of frames dropped on the host.')
  })

  it.each([0, null, '20'])('does not use a revision surviving reset as a current video sample: %s', async (sampleCount) => {
    await mountView(live({ video_policy_sample_count: sampleCount }))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('unknown ms encode / unknown of frames dropped on the host.')
  })

  it.each([
    { media_loss_sample_revision: '10' },
    { media_loss_last_received_age_ms: '100' },
    { media_loss_last_received_age_ms: -1 },
    { media_loss_last_received_age_ms: Infinity },
    { network_verdict: { ...live().network_verdict, loss_basis: 'enet_reliable_channel' } },
    { network_verdict: { ...live().network_verdict, media_samples: 4 } },
    { network_verdict: { ...live().network_verdict, frames_expected: 0 } },
    { network_verdict: { ...live().network_verdict, frames_expected: '2400' } },
  ])('does not grade loss without its measurement provenance: %j', async (override) => {
    await mountView(live(override))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('unknown of video frames lost after FEC')
    expect(card().text()).not.toContain('Lower bitrate')
  })

  it('does not retain a judged verdict on invalid window counters', async () => {
    await mountView(live({ network_verdict: { ...live().network_verdict, loss_pct: 5, loss_state: 'elevated', frames_lost: 120 } }))
    await receive(live({ network_verdict: { ...live().network_verdict, loss_pct: null, loss_state: 'stale',
      media_samples: '20', frames_expected: Infinity }, media_loss_last_received_age_ms: 6000 }))
    await receive({ streaming: false, last_session: ended() })
    expect(card().text()).toContain('unknown of video frames lost after FEC')
    expect(card().text()).not.toContain('5.0%')
  })
})
