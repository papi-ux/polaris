// NVENC that the host's encoder probe could not start, with the CPU encoder selected in its place,
// is named before a stream (#490, discussion #813). The helper is pure; Mission Control is mounted
// in jsdom with the real English messages. Doctor & Support's card is in
// doctor-idle-host-findings.test.js. No GPU, host or browser is involved.
import { mount, flushPromises } from '@vue/test-utils'
import { ref } from 'vue'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import { nvencCpuFallbackNotice } from './encoder-fallback-notice.js'
import DashboardView from './views/DashboardView.vue'

const shared = vi.hoisted(() => ({ stats: null, connected: null }))
vi.mock('./composables/useStreamStats', () => ({ useStreamStats: () => ({ stats: shared.stats, connected: shared.connected, receivedAt: ref(0) }) }))
vi.mock('./composables/useLiveTuning', () => ({ useLiveTuning: () => ({ label: ref('Off') }) }))
vi.mock('./composables/useSystemStats', () => ({ useSystemStats: () => ({ gpu: ref(null), displays: ref([]), audio: ref(null), sessionType: ref(''), readiness: ref({ displays: 'unavailable', audio: 'unavailable' }), refresh: vi.fn(async () => {}) }) }))
vi.mock('./composables/useSessionHistory', () => ({ useSessionHistory: () => ({ sessions: ref([]), clearHistory: vi.fn(), activeStartedAt: ref(null) }), formatDuration: String }))
vi.mock('./composables/useAiOptimizer', () => ({ useAiOptimizer: () => ({ status: ref(null), fetchStatus: vi.fn(), fetchDevices: vi.fn() }) }))
vi.mock('./composables/useFavicon', () => ({ useFavicon: vi.fn() }))
vi.mock('./client-embed.js', async importOriginal => ({ ...(await importOriginal()), followClientLink: vi.fn() }))
const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const copy = messages.troubleshooting

// encoder_selection as the host serves it after an NVIDIA Auto probe that ended on libx264.
const REASON = 'Preferred encoder [nvenc] did not satisfy this runtime; selected [software] instead. ' +
  'NVENC did not start on NVIDIA driver [580.95.05]; the libav error above names the nvenc API version it required, ' +
  'and a driver older than that is the usual cause. Auto detected NVIDIA; prefer NVENC.'
const cpuFallback = {
  mode: 'auto', gpu_driver: 'nvidia', policy: 'nvidia_nvenc', preferred_encoder: 'nvenc',
  fallback_encoder: 'next_available', selected_encoder: 'software', exact_live_probe_required: false,
  fallback_used: true, reason: REASON,
}
const t = key => key

describe('NVENC to CPU encoder fallback notice', () => {
  it('names the observed fallback with what to check, and keeps the host reason as evidence', () => {
    expect(nvencCpuFallbackNotice(cpuFallback, t)).toEqual({
      title: 'troubleshooting.nvenc_cpu_fallback_title',
      detail: 'troubleshooting.nvenc_cpu_fallback_detail',
      action: 'troubleshooting.nvenc_cpu_fallback_action',
      evidence: REASON,
    })
    // An explicit encoder = nvenc that a legacy non-strict probe replaced with libx264 is the same fact.
    expect(nvencCpuFallbackNotice({ ...cpuFallback, mode: 'explicit', policy: 'explicit' }, t)).not.toBeNull()
    expect(nvencCpuFallbackNotice({ ...cpuFallback, reason: undefined }, t).evidence).toBe('')
  })

  it.each([
    ['NVENC started', { ...cpuFallback, selected_encoder: 'nvenc', fallback_used: false }],
    ['NVENC replaced by another hardware encoder', { ...cpuFallback, selected_encoder: 'vulkan' }],
    ['a VA-API host on the CPU encoder', { ...cpuFallback, gpu_driver: 'amdgpu', preferred_encoder: 'vaapi' }],
    ['an availability probe that chose the CPU encoder', { ...cpuFallback, preferred_encoder: 'automatic', fallback_used: false }],
    ['no probe has completed', { ...cpuFallback, selected_encoder: 'unknown', fallback_used: false }],
    ['a fallback flag that is not a boolean', { ...cpuFallback, fallback_used: 'true' }],
    ['PyroWave', { ...cpuFallback, preferred_encoder: 'pyrowave', selected_encoder: 'pyrowave', fallback_used: false }],
  ])('says nothing for %s', (_label, selection) => {
    expect(nvencCpuFallbackNotice(selection, t)).toBeNull()
  })

  it.each([[undefined], [null], ['nvenc'], [{}]])('says nothing without an encoder selection (%j)', (selection) => {
    expect(nvencCpuFallbackNotice(selection, t)).toBeNull()
  })

  it('has plain English copy that names the cause and the three things to check', () => {
    expect(copy.nvenc_cpu_fallback_title).toBe('NVENC is unavailable on this GPU with this build, so video is encoded on the CPU.')
    expect(copy.nvenc_cpu_fallback_action).toMatch(/NVIDIA driver/)
    expect(copy.nvenc_cpu_fallback_action).toMatch(/CUDA build covers this GPU's generation/)
    expect(copy.nvenc_cpu_fallback_action).toMatch(/Build features: cuda=enabled or cuda=disabled/)
    // No hardcoded architecture floor or GPU family verdict.
    for (const text of [copy.nvenc_cpu_fallback_title, copy.nvenc_cpu_fallback_detail, copy.nvenc_cpu_fallback_action]) {
      expect(text).not.toMatch(/sm_|compute capability|\b75\b|GTX|Pascal|not supported/i)
    }
  })
})

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

const report = (streaming, selection) => ({
  streaming, fps: streaming ? 60 : 0, bitrate_kbps: streaming ? 20000 : 0, encode_time_ms: streaming ? 4 : 0,
  latency_ms: 5, packet_loss: 0,
  doctor: { primary_issue: streaming ? 'none' : 'no_active_stream', status: 'unknown', severity: 'info',
    advanced_evidence: { encoder_selection: selection } },
})
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
const notices = () => wrapper.findAll('[data-dashboard-encoder-fallback]')

describe('Mission Control names an NVENC to CPU fallback', () => {
  it('says so before play, with what to check and a way to Doctor & Support', async () => {
    await render(report(false, cpuFallback))
    expect(notices()).toHaveLength(1)
    const notice = notices()[0]
    expect(notice.text()).toContain(copy.nvenc_cpu_fallback_title)
    expect(notice.text()).toContain(copy.nvenc_cpu_fallback_action)
    expect(notice.get('a').attributes('href')).toBe('/troubleshooting')
    expect(notice.get('a').text()).toBe(messages.navbar.troubleshoot)
  })

  it('keeps it in the Host vitals diagnostics while a stream runs', async () => {
    await render(report(true, cpuFallback))
    expect(notices()).toHaveLength(1)
    expect(notices()[0].element.closest('[data-mission-diagnostics-body]')).not.toBeNull()
    expect(notices()[0].text()).toContain(copy.nvenc_cpu_fallback_title)
  })

  it('shows nothing for a healthy NVENC selection, an unprobed host or another hardware fallback', async () => {
    await render(report(false, { ...cpuFallback, selected_encoder: 'nvenc', fallback_used: false }))
    expect(notices()).toHaveLength(0)
    shared.stats.value = report(false, { ...cpuFallback, selected_encoder: 'unknown', fallback_used: false })
    await flushPromises()
    expect(notices()).toHaveLength(0)
    shared.stats.value = report(false, { ...cpuFallback, selected_encoder: 'vulkan' })
    await flushPromises()
    expect(notices()).toHaveLength(0)
    shared.stats.value = report(false, cpuFallback)
    await flushPromises()
    expect(notices()).toHaveLength(1)
  })

  it('drops it with the other readings once the stream status is unavailable', async () => {
    await render(report(false, cpuFallback))
    expect(notices()).toHaveLength(1)
    shared.connected.value = false
    await flushPromises()
    vi.advanceTimersByTime(6000)
    await flushPromises()
    expect(wrapper.find('[data-dashboard-doctor-waiting]').exists()).toBe(true)
    expect(notices()).toHaveLength(0)
  })
})
