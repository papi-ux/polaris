import { mount, flushPromises } from '@vue/test-utils'
import { ref } from 'vue'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import DashboardView from './DashboardView.vue'
import { followClientLink, initClientEmbed } from '../client-embed.js'

const shared = vi.hoisted(() => ({ stats: null, gpu: null, connected: undefined, receivedAt: undefined }))
// connected and receivedAt stay undefined unless a case models the stream-status channel itself.
vi.mock('../composables/useStreamStats', () => ({ useStreamStats: () => ({ stats: shared.stats, connected: shared.connected, receivedAt: shared.receivedAt }) }))
vi.mock('../composables/useLiveTuning', () => ({ useLiveTuning: () => ({ label: ref('Off') }) }))
vi.mock('../composables/useSystemStats', () => ({ useSystemStats: () => ({ gpu: shared.gpu, displays: ref([]), audio: ref(null), sessionType: ref(''), readiness: ref({ displays: 'unavailable', audio: 'unavailable' }), refresh: vi.fn(async () => {}) }) }))
vi.mock('../composables/useSessionHistory', () => ({ useSessionHistory: () => ({ sessions: ref([]), clearHistory: vi.fn(), activeStartedAt: ref(null) }), formatDuration: String }))
vi.mock('../composables/useAiOptimizer', () => ({ useAiOptimizer: () => ({ status: ref(null), fetchStatus: vi.fn(), fetchDevices: vi.fn() }) }))
vi.mock('../composables/useFavicon', () => ({ useFavicon: vi.fn() }))
// Nova takes its launch link before the page goes anywhere; jsdom cannot follow it.
vi.mock('../client-embed.js', async importOriginal => ({ ...(await importOriginal()), followClientLink: vi.fn() }))
const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const snapshot = extra => ({ enabled: true, available: true, changing: false, failed: false,
  profiles: [{ id: 'a', name: 'Gaming Space', clients: ['device'], family: 'steam' }], activity: [], ...extra })
const active = state => snapshot({ activity: [{ profile_id: 'a', client_id: 'device', state }] })
const reply = data => ({ ok: true, json: async () => data })
let wrapper, spaces, platform, appPayload, pairedCerts
beforeEach(() => {
  vi.useFakeTimers()
  shared.stats = ref(null)
  shared.gpu = ref(null)
  shared.connected = undefined
  shared.receivedAt = undefined
  pairedCerts = [{ uuid: 'device', name: 'Handheld' }]
  spaces = snapshot(); platform = 'linux'
  appPayload = { apps: [], current_app: '' }
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} })
  vi.stubGlobal('matchMedia', () => ({ matches: true, addEventListener() {}, removeEventListener() {} }))
  vi.stubGlobal('fetch', vi.fn(async url => {
    if (url === './api/multiseat/profiles') return reply(spaces)
    if (url === './api/config') return reply({ platform, linux_stream_mode: 'desktop_display' })
    if (url === './api/clients/list') return reply({ named_certs: pairedCerts })
    if (url === './api/apps') return reply(appPayload)
    return reply([])
  }))
})
afterEach(() => { wrapper?.unmount(); wrapper = null; vi.unstubAllGlobals(); vi.restoreAllMocks(); vi.useRealTimers() })
async function render(streaming = true, { attach = false, dialogs = false, stats = {} } = {}) {
  const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
  wrapper = mount(DashboardView, { attachTo: attach ? document.body : undefined, global: { plugins: [i18n], provide: { i18n: i18n.global },
    stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' },
      GaugeArc: true, QuickControls: true, ConfirmActionDialog: !dialogs } } })
  await flushPromises()
  shared.stats.value = { streaming, fps: 60, bitrate_kbps: 10000, encode_time_ms: 2, latency_ms: 5, packet_loss: 0, ...stats }
  await flushPromises()
}
const spaceReads = () => fetch.mock.calls.filter(([url]) => url === './api/multiseat/profiles')

describe('Mission Control with Space sessions', () => {
  it('puts idle launches before collapsible vitals, preserving zero and unknown readings', async () => {
    shared.gpu.value = { name: 'Fixture GPU', temperature_c: 0, utilization_pct: 0 }
    appPayload = { apps: [{ uuid: 'game', name: 'Control', cmd: 'game' }], current_app: '' }
    await render(false)
    const rail = wrapper.get('[data-dashboard-play-rail]')
    const vitals = wrapper.get('[data-dashboard-idle-vitals]')
    expect(rail.element.compareDocumentPosition(vitals.element) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
    expect(vitals.element.open).toBe(false)
    expect(wrapper.get('[data-dashboard-vitals-summary]').text()).toContain('0°C')
    expect(wrapper.get('[data-dashboard-vitals-summary]').text()).toContain('0% GPU')
    expect(wrapper.get('[data-dashboard-vitals-summary]').text()).toContain('encoder unknown')
    expect(wrapper.get('[data-dashboard-launch-app="game"]').attributes('disabled')).toBeUndefined()
    shared.gpu.value = { name: 'Fixture GPU', temperature_c: 85, utilization_pct: 0, encoder_pct: 85, vram_used_mb: 7373, vram_total_mb: 8192 }
    await flushPromises()
    expect(vitals.element.open).toBe(false)
    expect(wrapper.get('[data-vitals-temperature]').text()).toBe('85°C temperature · high')
    expect(wrapper.get('[data-vitals-temperature]').classes()).toContain('text-danger-bright')
    expect(wrapper.get('[data-vitals-encoder]').classes()).toContain('text-danger-bright')
    expect(wrapper.get('[data-vitals-vram]').text()).toBe('90% VRAM · high')
    shared.stats.value = { streaming: true, fps: 60 }; await flushPromises()
    expect(wrapper.find('[data-dashboard-idle-vitals]').exists()).toBe(false)
    expect(wrapper.find('[data-dashboard-play-rail]').exists()).toBe(false)
  })

  it('blocks recent launches while app identity is unavailable and restores the current catalog', async () => {
    appPayload = { apps: [{ uuid: 'app-a', name: 'Control A', cmd: 'run-a' }], current_app: 'app-a' }
    await render(false)
    appPayload = { ...appPayload, current_app: '', current_app_available: false }
    await wrapper.vm.refreshApps(); await flushPromises()
    const before = fetch.mock.calls.length
    await wrapper.vm.launchRecentApp(appPayload.apps[0])
    expect(fetch.mock.calls).toHaveLength(before)
    expect(wrapper.get('[data-dashboard-launch-app="app-a"]').attributes('disabled')).toBeDefined()
    expect(wrapper.get('[data-dashboard-play-rail]').text()).toContain('Waiting for current app status.')
    let resolveLate
    const lateBody = new Promise(resolve => { resolveLate = resolve })
    fetch.mockResolvedValueOnce({ ok: true, json: () => lateBody })
    const staleRead = wrapper.vm.refreshApps()
    await wrapper.vm.refreshApps()
    resolveLate({ apps: [{ uuid: 'app-a', name: 'Control A' }], current_app: 'app-a' })
    await staleRead
    expect(wrapper.vm.currentAppAvailable).toBe(false)
    fetch.mockRejectedValueOnce(new Error('fixture read failed'))
    await wrapper.vm.refreshApps()
    expect(wrapper.vm.currentAppAvailable).toBe(false)
    appPayload = { apps: [{ uuid: 'app-b', name: 'Control B', cmd: 'run-b' }], current_app: '', current_app_available: true }
    await wrapper.vm.refreshApps(); await flushPromises()
    const launch = wrapper.get('[data-dashboard-launch-app="app-b"]')
    expect(launch.attributes('disabled')).toBeUndefined()
    await launch.trigger('click'); await flushPromises()
    expect(fetch.mock.calls.some(([url, options]) => url === './api/apps/launch' && JSON.parse(options.body).uuid === 'app-b')).toBe(true)
  })
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
  it('links a blocked Show display button to the reason it is disabled, and drops the link once allowed', async () => {
    spaces = active('running'); await render()
    const blocked = wrapper.get('[data-start-preview]')
    expect(blocked.attributes('disabled')).toBeDefined()
    const reasonId = blocked.attributes('aria-describedby')
    expect(reasonId).toBeTruthy()
    expect(wrapper.get(`#${reasonId}`).text()).toBe(messages.dashboard_spaces.no_preview)
    spaces = snapshot()
    await wrapper.get('[data-spaces-refresh]').trigger('click'); await flushPromises()
    const allowed = wrapper.get('[data-start-preview]')
    expect(allowed.attributes('disabled')).toBeUndefined()
    expect(allowed.attributes('aria-describedby')).toBeUndefined()
    // Once allowed, no reason (and so no orphaned description id) remains.
    expect(wrapper.find('[data-preview-hidden-reason]').exists()).toBe(false)
  })

})

describe('Mission Control header, Spaces and shelf', () => {
  const shelf = [
    { uuid: 'a', name: 'Alpha', 'image-path': 'a.png', source: 'steam', 'last-launched': 30 },
    { uuid: 'b', name: 'Bravo', 'image-path': 'b.png', 'last-launched': 20 },
    { uuid: 'c', name: 'Charlie', 'last-launched': 0 },
  ]
  const launches = () => fetch.mock.calls.filter(([url]) => url === './api/apps/launch')
  const stage = () => wrapper.get('[data-dashboard-idle-hero]')

  it('keeps the header to the title and guide, announcing the state only to screen readers', async () => {
    await render(false)
    expect(wrapper.find('.page-subtitle').exists()).toBe(false)
    const status = wrapper.get('[data-mission-status]')
    expect(status.attributes('role')).toBe('status')
    expect(status.classes()).toContain('sr-only')
    expect(status.text()).toBe(messages.dashboard.standby)
    const guide = wrapper.get('[data-tab-docs-link]')
    expect(guide.attributes('href')).toBe('https://papi-ux.com/docs/mission-control/')
    expect(guide.attributes('aria-label')).toBe(messages.dashboard.docs_link)
    shared.stats.value = { streaming: true, fps: 60 }; await flushPromises()
    expect(status.text()).toBe(messages.dashboard.live)
  })

  it('leaves healthy idle Spaces off the page and brings them back for a problem or a session', async () => {
    await render(false)
    expect(wrapper.find('[data-dashboard-spaces]').exists()).toBe(false)
    spaces = snapshot({ failed: true })
    await vi.advanceTimersByTimeAsync(10000); await flushPromises()
    expect(wrapper.get('[data-spaces-attention]').text()).toBe(messages.dashboard_spaces.attention)
    expect(wrapper.find('[data-dashboard-idle-hero]').exists()).toBe(true)
    spaces = active('running')
    await vi.advanceTimersByTimeAsync(10000); await flushPromises()
    expect(wrapper.get('[data-space-session]').text()).toContain('Gaming Space')
    expect(wrapper.find('[data-dashboard-idle-hero]').exists()).toBe(false)
  })

  it('features a poster on the stage without launching it, and launches only from Play', async () => {
    appPayload = { apps: shelf, current_app: '' }
    await render(false)
    expect(stage().get('.mc-stage-title').text()).toBe('Alpha')
    expect(wrapper.get('[data-dashboard-feature-app="a"]').attributes('aria-pressed')).toBe('true')

    await wrapper.get('[data-dashboard-feature-app="b"]').trigger('click')
    expect(stage().get('.mc-stage-title').text()).toBe('Bravo')
    expect(stage().get('[data-stage-artwork] img').attributes('src')).toBe('./api/covers/image?name=Bravo')
    expect(stage().get('[data-stage-launch]').attributes('aria-label')).toBe('Launch Bravo')
    expect(wrapper.get('[data-dashboard-feature-app="b"]').attributes('aria-pressed')).toBe('true')
    expect(wrapper.get('[data-dashboard-feature-app="a"]').attributes('aria-pressed')).toBe('false')
    expect(launches()).toHaveLength(0)

    // A pick with no launch record is still next up; without a cover or a Steam background the
    // stage is typographic.
    await wrapper.get('[data-dashboard-feature-app="c"]').trigger('click')
    expect(stage().get('.mc-kicker').text()).toBe(messages.mission_control.next_up)
    expect(stage().find('[data-stage-artwork]').exists()).toBe(false)
    await stage().get('[data-stage-hero]').trigger('error')
    expect(stage().find('.mc-stage-backdrop').exists()).toBe(false)

    // A refresh that drops the pick returns the stage to the most recent title.
    appPayload = { apps: shelf.slice(0, 2), current_app: '' }
    await wrapper.vm.refreshApps(); await flushPromises()
    expect(stage().get('.mc-stage-title').text()).toBe('Alpha')
    expect(launches()).toHaveLength(0)

    // Play launches its own title; afterwards the stage follows the launch record, not the old pick.
    const base = fetch.getMockImplementation()
    fetch.mockImplementation(async (url, options) => (url === './api/apps/launch' ? reply({ status: true }) : base(url, options)))
    appPayload = { apps: shelf, current_app: '' }
    await wrapper.vm.refreshApps(); await flushPromises()
    await wrapper.get('[data-dashboard-feature-app="b"]').trigger('click')
    appPayload = { apps: [{ ...shelf[2], 'last-launched': 40 }, ...shelf.slice(0, 2)], current_app: '' }
    await wrapper.get('[data-dashboard-launch-app="c"]').trigger('click'); await flushPromises()
    expect(launches().map(([, options]) => JSON.parse(options.body).uuid)).toEqual(['c'])
    expect(stage().get('.mc-stage-title').text()).toBe('Charlie')
    expect(wrapper.get('[data-dashboard-feature-app="c"]').attributes('aria-pressed')).toBe('true')
  })

  it('features the poster for a tap that began while Play was hidden, even when its click lands on Play', async () => {
    appPayload = { apps: shelf, current_app: '' }
    await render(false)
    // The finger went down on the poster; the browser then delivers the click to the revealed Play.
    // test-utils cannot set a click's detail, so these pointer clicks are dispatched directly.
    const pointerClick = async selector => { wrapper.get(selector).element.dispatchEvent(new MouseEvent('click', { bubbles: true, detail: 1 })); await flushPromises() }
    await wrapper.get('[data-dashboard-feature-app="b"]').trigger('pointerdown')
    await pointerClick('[data-dashboard-launch-app="b"]')
    expect(launches()).toHaveLength(0)
    expect(stage().get('.mc-stage-title').text()).toBe('Bravo')
    // A press that began on the shown Play launches, and so does keyboard activation (detail 0).
    const base = fetch.getMockImplementation()
    fetch.mockImplementation(async (url, options) => (url === './api/apps/launch' ? reply({ status: true }) : base(url, options)))
    await wrapper.get('[data-dashboard-launch-app="b"]').trigger('pointerdown')
    await pointerClick('[data-dashboard-launch-app="b"]')
    await wrapper.get('[data-dashboard-launch-app="c"]').trigger('click'); await flushPromises()
    expect(launches().map(([, options]) => JSON.parse(options.body).uuid)).toEqual(['b', 'c'])
  })

  it('inside Nova\'s Host Console hands Play to Nova, hides the guide link and starts nothing on the host', async () => {
    initClientEmbed({ search: '?client=nova&theme=director', storage: null, themes: ['polaris', 'director'] })
    try {
      appPayload = { apps: shelf, current_app: '', host_uuid: 'host-1', host_name: 'Living Room' }
      await render(false)
      expect(wrapper.find('[data-tab-docs-link]').exists()).toBe(false)
      await wrapper.get('[data-dashboard-launch-app="b"]').trigger('click')
      expect(followClientLink).toHaveBeenLastCalledWith('art://launch?host_uuid=host-1&host_name=Living%20Room&app_uuid=b&app_name=Bravo')
      await stage().get('[data-stage-launch]').trigger('click')
      expect(followClientLink).toHaveBeenLastCalledWith('art://launch?host_uuid=host-1&host_name=Living%20Room&app_uuid=a&app_name=Alpha')
      expect(launches()).toHaveLength(0)
      // Without the host's id Nova could not take the link: Play and Launch wait, say why, and
      // nothing starts on the host behind the player's back.
      followClientLink.mockClear()
      appPayload = { apps: shelf, current_app: '' }
      await wrapper.vm.refreshApps(); await flushPromises()
      expect(wrapper.get('[data-dashboard-launch-app="b"]').attributes('disabled')).toBeDefined()
      expect(stage().get('[data-stage-launch]').attributes('aria-disabled')).toBe('true')
      expect(wrapper.get('[data-nova-launch-reason]').text()).toBe(messages.mission_control.nova_launch_waiting)
      await wrapper.get('[data-dashboard-launch-app="b"]').trigger('click'); await flushPromises()
      await stage().get('[data-stage-launch]').trigger('click'); await flushPromises()
      expect(followClientLink).not.toHaveBeenCalled()
      expect(launches()).toHaveLength(0)
    } finally {
      initClientEmbed({ search: '', storage: null, themes: [] })
      followClientLink.mockClear()
    }
  })

  it('in a browser keeps the guide link and starts Play on the host', async () => {
    appPayload = { apps: shelf, current_app: '', host_uuid: 'host-1', host_name: 'Living Room' }
    await render(false)
    expect(wrapper.get('[data-tab-docs-link]').attributes('href')).toBe('https://papi-ux.com/docs/mission-control/')
    await wrapper.get('[data-dashboard-launch-app="b"]').trigger('click'); await flushPromises()
    expect(followClientLink).not.toHaveBeenCalled()
    expect(launches()).toHaveLength(1)
  })

  it('offers Library from the shelf, and from the stage only when the library is empty', async () => {
    appPayload = { apps: shelf, current_app: '' }
    await render(false)
    expect(stage().find('a[href="/apps"]').exists()).toBe(false)
    expect(wrapper.findAll('[data-dashboard-play-rail] a[href="/apps"]')).toHaveLength(1)
    expect(wrapper.find('[data-diagnostics-host] [data-mission-route]').exists()).toBe(true)
    appPayload = { apps: [], current_app: '' }
    await wrapper.vm.refreshApps(); await flushPromises()
    expect(wrapper.find('[data-dashboard-play-rail]').exists()).toBe(false)
    expect(stage().get('[data-stage-library]').attributes('href')).toBe('/apps')
  })
})

describe('Mission Control stage', () => {
  const displayReads = () => fetch.mock.calls.filter(([url]) => /\/api\/display\/(stream|screenshot)/.test(String(url)))
  const live = { streaming: true, fps: 60, bitrate_kbps: 10000, encode_time_ms: 2, latency_ms: 5,
    network_verdict: { loss_pct: 0.2, loss_state: 'light', window_seconds: 20 } }

  it('never captures the host display before Show display, even without reduced motion', async () => {
    vi.stubGlobal('matchMedia', () => ({ matches: false, addEventListener() {}, removeEventListener() {} }))
    spaces = snapshot({ enabled: false, profiles: [] })
    await render()
    for (let tick = 0; tick < 3; tick++) { shared.stats.value = { ...live, fps: 60 + tick }; await flushPromises() }
    await vi.advanceTimersByTimeAsync(5000)
    expect(wrapper.vm.showPreview).toBe(false)
    expect(wrapper.vm.previewUrl).toBe('')
    expect(wrapper.find('.dashboard-preview-image').exists()).toBe(false)
    expect(displayReads()).toHaveLength(0)
    await wrapper.get('[data-start-preview]').trigger('click'); await flushPromises()
    expect(wrapper.get('.dashboard-preview-image').attributes('src')).toContain('/api/display/stream')
    // Hide display retires the capture; a late image callback cannot bring it back.
    const image = wrapper.get('.dashboard-preview-image')
    await wrapper.get('[data-stop-preview]').trigger('click'); await flushPromises()
    await image.trigger('load')
    expect(wrapper.vm.showPreview).toBe(false)
    expect(wrapper.vm.previewUrl).toBe('')
    expect(wrapper.find('[data-start-preview]').exists()).toBe(true)
  })

  it('shows a disconnected live stream as unavailable with no numeric readings, then recovers', async () => {
    shared.connected = ref(true)
    shared.receivedAt = ref(Date.now())
    await render()
    expect(wrapper.get('[data-mission-stage]').attributes('data-stage-mode')).toBe('live')
    expect(wrapper.findAll('[data-live-summary-metric]')).toHaveLength(6)
    shared.connected.value = false
    shared.receivedAt.value = 0
    await vi.advanceTimersByTimeAsync(4000); await flushPromises()
    // Inside the grace period a reconnect gap is not shown as an outage.
    expect(wrapper.get('[data-mission-stage]').attributes('data-stage-mode')).toBe('live')
    await vi.advanceTimersByTimeAsync(2000); await flushPromises()
    const stage = wrapper.get('[data-mission-stage]')
    expect(stage.attributes('data-stage-mode')).toBe('unavailable')
    expect(stage.text()).toContain('Waiting for host')
    expect(stage.text()).toMatch(/Last report \d+ s ago/)
    expect(wrapper.findAll('[data-live-summary-metric]')).toHaveLength(0)
    const readings = wrapper.findAll('[data-unavailable-signal] .mc-signal-number').map(node => node.text())
    expect(readings).toEqual(['—', '—', '—', '—', '—', '—'])
    expect(stage.text()).not.toMatch(/\d+\.\d/)
    expect(wrapper.find('[data-start-preview]').exists()).toBe(false)
    expect(wrapper.find('[data-dashboard-doctor-waiting]').exists()).toBe(true)
    expect(wrapper.find('[data-dashboard-doctor]').exists()).toBe(false)
    shared.connected.value = true
    shared.receivedAt.value = Date.now()
    shared.stats.value = { ...live }
    await flushPromises()
    expect(wrapper.get('[data-mission-stage]').attributes('data-stage-mode')).toBe('live')
  })

  it('reports a host that never answered as unavailable without inventing a report age', async () => {
    shared.connected = ref(false)
    shared.receivedAt = ref(0)
    const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
    wrapper = mount(DashboardView, { global: { plugins: [i18n], provide: { i18n: i18n.global },
      stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' }, GaugeArc: true, QuickControls: true, ConfirmActionDialog: true } } })
    await flushPromises()
    expect(wrapper.get('[data-mission-stage]').attributes('data-stage-mode')).toBe('loading')
    await vi.advanceTimersByTimeAsync(6000); await flushPromises()
    const stage = wrapper.get('[data-mission-stage]')
    expect(stage.attributes('data-stage-mode')).toBe('unavailable')
    expect(stage.text()).toContain('No report received yet')
    expect(stage.text()).not.toMatch(/Last report/)
  })

  it('features the running app with its own library cover, whether or not it is recent', async () => {
    const apps = Array.from({ length: 7 }, (_, index) => ({ uuid: `app-${index}`, name: `Game ${index}`, 'last-launched': 100 - index }))
    apps[6] = { ...apps[6], name: 'Control Ultimate Edition', 'image-path': 'control.png', source: 'steam' }
    appPayload = { apps, current_app: 'app-6' }
    await render()
    const stage = wrapper.get('[data-mission-stage]')
    expect(stage.get('.mc-stage-title').text()).toBe('Control Ultimate Edition')
    expect(stage.get('[data-stage-artwork] img').attributes('src')).toBe('./api/covers/image?name=Control%20Ultimate%20Edition')
    expect(stage.get('[data-stage-hero]').attributes('src')).toBe('./api/apps/app-6/artwork/hero')
    expect(stage.get('[data-stage-logo]').attributes('src')).toBe('./api/apps/app-6/artwork/logo')
    expect(stage.get('.mc-stage-title').attributes('data-title-step')).toBe('medium')
    // Without a Steam background the softened cover stands in; a cover that fails as well leaves a
    // typographic stage: no backdrop, no key art.
    await stage.get('[data-stage-hero]').trigger('error')
    expect(stage.get('.mc-stage-backdrop').attributes('src')).toBe('./api/covers/image?name=Control%20Ultimate%20Edition')
    await stage.get('.mc-stage-backdrop').trigger('error')
    expect(stage.find('.mc-stage-backdrop').exists()).toBe(false)
    expect(stage.find('[data-stage-artwork]').exists()).toBe(false)
  })

  it('renders an unreported GPU temperature and power as dashes in the idle reading', async () => {
    shared.gpu.value = { vendor: 'amd', name: 'AMD Radeon RX 7900 XTX', utilization_pct: 55, vram_used_mb: 8192, vram_total_mb: 24576 }
    await render(false)
    const reading = wrapper.get('[data-dashboard-idle-hero] [data-idle-signal="gpu"]').text()
    expect(reading).toContain('--°C')
    expect(reading).toContain('55% · --W')
    expect(reading).not.toMatch(/(^|\D)0W|NaN|undefined/)
  })

  it('keeps a title without a cover typographic', async () => {
    appPayload = { apps: [{ uuid: 'plain', name: 'Plain Game', 'last-launched': 5 }], current_app: '' }
    await render(false)
    const stage = wrapper.get('[data-dashboard-idle-hero]')
    expect(stage.get('.mc-stage-title').text()).toBe('Plain Game')
    // The host has no Steam background for it either.
    await stage.get('[data-stage-hero]').trigger('error')
    expect(stage.find('.mc-stage-backdrop').exists()).toBe(false)
    expect(stage.find('[data-stage-artwork]').exists()).toBe(false)
  })

  it('promotes Open priority fix over Launch while a launch check needs attention', async () => {
    appPayload = { apps: [{ uuid: 'game', name: 'Control', 'last-launched': 5 }], current_app: '' }
    pairedCerts = []
    await render(false)
    const actions = wrapper.get('[data-dashboard-idle-hero] .mc-stage-actions')
    const fix = actions.get('[data-stage-priority-fix]')
    const launch = actions.get('[data-stage-launch]')
    expect(fix.attributes('href')).toBe('/pin')
    expect(fix.classes()).toContain('dashboard-action-button-primary')
    expect(launch.classes()).toContain('dashboard-action-button-ghost')
    expect(fix.element.compareDocumentPosition(launch.element) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
    pairedCerts = [{ uuid: 'device', name: 'Handheld' }]
    await wrapper.vm.refreshLaunchChecks(); await flushPromises()
    expect(wrapper.find('[data-stage-priority-fix]').exists()).toBe(false)
    expect(wrapper.get('[data-stage-launch]').classes()).toContain('dashboard-action-button-primary')
  })

  it('greets Director with its exact theme name and leaves other themes on the summary', async () => {
    document.documentElement.setAttribute('data-theme', 'director')
    try {
      await render(false)
      expect(wrapper.get('[data-director-greeting]').element.textContent).toBe('< Congratulations, Director >')
    } finally {
      document.documentElement.removeAttribute('data-theme')
    }
    wrapper.unmount(); wrapper = null
    await render(false)
    expect(wrapper.find('[data-director-greeting]').exists()).toBe(false)
  })

  it('keeps Diagnostics open and focus where it was across telemetry updates', async () => {
    await render(true, { attach: true })
    const toggle = wrapper.get('[data-mission-diagnostics-toggle]')
    expect(toggle.attributes('aria-expanded')).toBe('false')
    await toggle.trigger('click')
    expect(toggle.attributes('aria-expanded')).toBe('true')
    const show = wrapper.get('[data-start-preview]').element
    show.focus()
    for (let tick = 0; tick < 4; tick++) { shared.stats.value = { ...live, fps: 50 + tick }; await flushPromises() }
    await vi.advanceTimersByTimeAsync(3000)
    expect(wrapper.get('[data-mission-diagnostics-toggle]').attributes('aria-expanded')).toBe('true')
    expect(document.activeElement).toBe(show)
    // The stage's Session diagnostics action is the one deliberate move to the disclosure.
    await wrapper.get('[data-open-diagnostics]').trigger('click'); await flushPromises()
    expect(document.activeElement).toBe(wrapper.get('[data-mission-diagnostics-toggle]').element)
  })

})

describe('Mission Control Disconnect', () => {
  const posts = () => fetch.mock.calls.filter(([, options]) => options?.method === 'POST')
  afterEach(() => { document.body.innerHTML = '' })

  it('resolves the client from the real list response and posts its UUID only after confirmation', async () => {
    pairedCerts = [{ uuid: 'other', name: 'Tablet' }, { uuid: 'device', name: 'Handheld' }]
    await render(true, { attach: true, dialogs: true, stats: { client_name: 'Handheld', stream_instance_id: 'stream-1' } })
    const disconnect = wrapper.get('[data-disconnect-client]')
    await disconnect.trigger('click'); await flushPromises()
    expect(document.body.textContent).toContain('Handheld will be removed from the active stream.')
    document.querySelector('[data-confirm-cancel]').click(); await flushPromises()
    expect(document.querySelector('[data-confirm-confirm]')).toBeNull()
    expect(posts()).toHaveLength(0)
    await disconnect.trigger('click'); await flushPromises()
    document.querySelector('[data-confirm-confirm]').click(); await flushPromises()
    expect(posts()).toHaveLength(1)
    const [url, options] = posts()[0]
    expect(url).toBe('./api/clients/disconnect')
    expect(JSON.parse(options.body)).toEqual({ uuid: 'device' })
  })

  it.each([
    ['two pairing records share the name', [{ uuid: 'a', name: 'Handheld' }, { uuid: 'b', name: 'Handheld' }]],
    ['the matching record has no UUID', [{ name: 'Handheld' }]],
    ['the matching record has an empty UUID', [{ uuid: '', name: 'Handheld' }]],
    ['the matching record has a non-text UUID', [{ uuid: 7, name: 'Handheld' }]],
    ['no record has the name', [{ uuid: 'other', name: 'Tablet' }]],
    ['the list is malformed', 'not-a-list'],
    ['a valid record has a malformed twin with the same name', [{ uuid: 'device', name: 'Handheld' }, { name: 'Handheld' }]],
    ['a valid record has a blank-UUID twin with the same name', [{ uuid: 'device', name: 'Handheld' }, { uuid: '', name: 'Handheld' }]],
    ['the only record has a whitespace UUID', [{ uuid: '   ', name: 'Handheld' }]],
  ])('offers no Disconnect when %s', async (_case, certs) => {
    pairedCerts = certs
    await render(true, { stats: { client_name: 'Handheld' } })
    expect(wrapper.vm.connectedClientUuid).toBeNull()
    expect(wrapper.find('[data-disconnect-client]').exists()).toBe(false)
  })

  it('resolves the client when the page opens on a stream that is already live', async () => {
    pairedCerts = [{ uuid: 'device', name: 'Handheld' }]
    // The shared stats channel already carries the live stream before Mission Control mounts.
    shared.stats.value = { streaming: true, client_name: 'Handheld', stream_instance_id: 'stream-1', fps: 60 }
    const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
    wrapper = mount(DashboardView, { global: { plugins: [i18n], provide: { i18n: i18n.global },
      stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' }, GaugeArc: true, QuickControls: true, ConfirmActionDialog: true } } })
    await flushPromises()
    // Later ticks keep the same stream and client, so nothing else would trigger the lookup.
    shared.stats.value = { ...shared.stats.value, fps: 59 }
    await flushPromises()
    expect(wrapper.vm.connectedClientUuid).toBe('device')
    expect(wrapper.find('[data-disconnect-client]').exists()).toBe(true)
  })

  it('offers no Disconnect when the stream names no client', async () => {
    await render(true)
    expect(wrapper.find('[data-disconnect-client]').exists()).toBe(false)
    expect(fetch.mock.calls.filter(([url]) => url === './api/clients/list')).toHaveLength(1) // readiness only
  })

  it('discards a lookup that finishes after the stream changed client, and never retargets', async () => {
    let releaseStale
    const staleBody = new Promise(resolve => { releaseStale = resolve })
    let calls = 0
    fetch.mockImplementation(async url => {
      if (url === './api/clients/list') {
        calls += 1
        // The first live lookup is slow; the readiness read at mount answers at once.
        if (calls === 2) return { ok: true, json: () => staleBody }
        return reply({ named_certs: [{ uuid: 'device', name: 'Handheld' }, { uuid: 'tablet', name: 'Tablet' }] })
      }
      if (url === './api/multiseat/profiles') return reply(spaces)
      if (url === './api/config') return reply({ platform, linux_stream_mode: 'desktop_display' })
      if (url === './api/apps') return reply(appPayload)
      return reply([])
    })
    await render(true, { stats: { client_name: 'Handheld', stream_instance_id: 'stream-1' } })
    expect(wrapper.vm.connectedClientUuid).toBeNull()
    shared.stats.value = { ...shared.stats.value, client_name: 'Tablet' }
    await flushPromises()
    expect(wrapper.vm.connectedClientUuid).toBe('tablet')
    releaseStale({ named_certs: [{ uuid: 'device', name: 'Handheld' }] })
    await flushPromises()
    expect(wrapper.vm.connectedClientUuid).toBe('tablet')
    // The stream ending clears it, and a late reply cannot bring it back.
    shared.stats.value = { streaming: false }
    await flushPromises()
    expect(wrapper.vm.connectedClientUuid).toBeNull()
  })

  it('closes an open confirmation when its client is no longer the resolved one', async () => {
    pairedCerts = [{ uuid: 'device', name: 'Handheld' }]
    await render(true, { attach: true, dialogs: true, stats: { client_name: 'Handheld', stream_instance_id: 'stream-1' } })
    await wrapper.get('[data-disconnect-client]').trigger('click'); await flushPromises()
    expect(document.querySelector('[data-confirm-confirm]')).not.toBeNull()
    shared.stats.value = { ...shared.stats.value, stream_instance_id: 'stream-2', client_name: 'Someone else' }
    await flushPromises()
    expect(document.querySelector('[data-confirm-confirm]')).toBeNull()
    expect(posts()).toHaveLength(0)
  })
})

