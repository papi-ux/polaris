import { mount, flushPromises } from '@vue/test-utils'
import { ref } from 'vue'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import DashboardView from './DashboardView.vue'

const shared = vi.hoisted(() => ({ stats: null }))
vi.mock('../composables/useStreamStats', () => ({ useStreamStats: () => ({ stats: shared.stats }) }))
vi.mock('../composables/useLiveTuning', () => ({ useLiveTuning: () => ({ label: ref('Off') }) }))
vi.mock('../composables/useSystemStats', () => ({ useSystemStats: () => ({ gpu: ref(null), displays: ref([]), audio: ref(null), sessionType: ref('') }) }))
vi.mock('../composables/useSessionHistory', () => ({ useSessionHistory: () => ({ sessions: ref([]), clearHistory: vi.fn(), activeStartedAt: ref(null) }), formatDuration: String }))
vi.mock('../composables/useAiOptimizer', () => ({ useAiOptimizer: () => ({ status: ref(null), fetchStatus: vi.fn(), fetchDevices: vi.fn() }) }))
vi.mock('../composables/useFavicon', () => ({ useFavicon: vi.fn() }))
const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const snapshot = extra => ({ enabled: true, available: true, changing: false, failed: false,
  profiles: [{ id: 'a', name: 'Gaming Space', clients: ['device'], family: 'steam' }], activity: [], ...extra })
const active = state => snapshot({ activity: [{ profile_id: 'a', client_id: 'device', state }] })
const reply = data => ({ ok: true, json: async () => data })
let wrapper, spaces, platform
beforeEach(() => {
  vi.useFakeTimers()
  shared.stats = ref(null)
  spaces = snapshot(); platform = 'linux'
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} })
  vi.stubGlobal('matchMedia', () => ({ matches: true, addEventListener() {}, removeEventListener() {} }))
  vi.stubGlobal('fetch', vi.fn(async url => {
    if (url === './api/multiseat/profiles') return reply(spaces)
    if (url === './api/config') return reply({ platform, linux_stream_mode: 'desktop_display' })
    if (url === './api/clients/list') return reply({ named_certs: [{ uuid: 'device', name: 'Handheld' }] })
    if (url === './api/apps') return reply({ apps: [] })
    return reply([])
  }))
})
afterEach(() => { wrapper?.unmount(); wrapper = null; vi.unstubAllGlobals(); vi.restoreAllMocks(); vi.useRealTimers() })
async function render(streaming = true) {
  const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
  wrapper = mount(DashboardView, { global: { plugins: [i18n], provide: { i18n: i18n.global },
    stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' },
      GaugeArc: true, QuickControls: true, ConfirmActionDialog: true } } })
  await flushPromises()
  shared.stats.value = { streaming, fps: 60, bitrate_kbps: 10000, encode_time_ms: 2, latency_ms: 5, packet_loss: 0 }
  await flushPromises()
}
const spaceReads = () => fetch.mock.calls.filter(([url]) => url === './api/multiseat/profiles')

describe('Mission Control with Space sessions', () => {
  it('shows worker activity even when the host stream is idle, without the idle launch hero', async () => {
    spaces = active('running')
    await render(false)
    expect(wrapper.get('[data-dashboard-spaces]').text()).toContain('Gaming Space')
    expect(wrapper.get('[data-space-session]').text()).toContain('Handheld')
    expect(wrapper.find('[data-dashboard-idle-hero]').exists()).toBe(false)
    expect(wrapper.find('.dashboard-preview-image').exists()).toBe(false)
  })
  it('preserves normal desktop preview and stops background Spaces polling when disabled', async () => {
    spaces = snapshot({ enabled: false, profiles: [] })
    await render()
    expect(wrapper.find('[data-dashboard-spaces]').exists()).toBe(false)
    await wrapper.get('[data-start-preview]').trigger('click'); await flushPromises()
    expect(wrapper.get('.dashboard-preview-image').attributes('src')).toContain('/api/display/screenshot')
    expect(spaceReads()).toHaveLength(2) // initial state and fresh preview admission
    await vi.advanceTimersByTimeAsync(30000)
    expect(spaceReads()).toHaveLength(2)
  })
  it.each(['windows', 'macos'])('does not query Linux Spaces on %s', async value => {
    platform = value
    await render()
    await wrapper.get('[data-start-preview]').trigger('click'); await flushPromises()
    expect(wrapper.find('.dashboard-preview-image').exists()).toBe(true)
    expect(spaceReads()).toHaveLength(0)
  })
  it('rechecks activity before opening preview and rejects a newly starting Space', async () => {
    await render()
    spaces = active('starting')
    await wrapper.get('[data-start-preview]').trigger('click'); await flushPromises()
    expect(wrapper.find('.dashboard-preview-image').exists()).toBe(false)
    expect(wrapper.get('[data-space-session]').text()).toContain('Starting')
    expect(wrapper.get('[data-start-preview]').attributes('disabled')).toBeDefined()
  })
  it('retires an existing image and late image callbacks when Space activity arrives', async () => {
    await render()
    await wrapper.get('[data-start-preview]').trigger('click'); await flushPromises()
    wrapper.vm.previewMode = 'mjpeg'
    wrapper.vm.refreshPreview()
    await flushPromises()
    const oldImage = wrapper.get('.dashboard-preview-image')
    expect(oldImage.attributes('src')).toContain('/api/display/stream')
    spaces = active('running')
    await vi.advanceTimersByTimeAsync(10000); await flushPromises()
    expect(wrapper.find('.dashboard-preview-image').exists()).toBe(false)
    expect(wrapper.vm.previewUrl).toBe('')
    await oldImage.trigger('error')
    wrapper.vm.handlePreviewError()
    wrapper.vm.handlePreviewLoad()
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(30000); await flushPromises()
    expect(wrapper.vm.previewUrl).toBe('')
    expect(wrapper.find('.dashboard-preview-image').exists()).toBe(false)
  })
  it('permits a fresh preview after a Space ends without reopening the old image', async () => {
    spaces = active('stopping'); await render()
    spaces = snapshot()
    await wrapper.get('[data-spaces-refresh]').trigger('click'); await flushPromises()
    expect(wrapper.find('.dashboard-preview-image').exists()).toBe(false)
    expect(wrapper.get('[data-start-preview]').attributes('disabled')).toBeUndefined()
    await wrapper.get('[data-start-preview]').trigger('click'); await flushPromises()
    expect(wrapper.get('.dashboard-preview-image').attributes('src')).toContain('/api/display/screenshot')
  })
  it('keeps a stale activity list explicitly labeled and blocks preview after a failed poll', async () => {
    spaces = active('running'); await render()
    fetch.mockImplementation(async url => {
      if (url === './api/multiseat/profiles') throw new Error('network down')
      return reply([])
    })
    await vi.advanceTimersByTimeAsync(10000); await flushPromises()
    expect(wrapper.get('[data-space-session]').text()).toContain('Last reported: running')
    expect(wrapper.get('[data-spaces-attention]').text()).toContain('Needs attention')
    expect(wrapper.get('[data-start-preview]').attributes('disabled')).toBeDefined()
  })
  it('never reopens preview after leaving the page while its preflight read is pending', async () => {
    await render()
    let resolve, signal
    fetch.mockImplementation((_url, options) => { signal = options.signal; return new Promise(done => { resolve = done }) })
    await wrapper.get('[data-start-preview]').trigger('click')
    const vm = wrapper.vm
    wrapper.unmount(); wrapper = null
    expect(signal.aborted).toBe(true)
    resolve(reply(snapshot()))
    await flushPromises()
    expect(vm.showPreview).toBe(false)
    expect(vm.previewUrl).toBe('')
  })
})
