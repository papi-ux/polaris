import { shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'

import AppsView from './views/AppsView.vue'

const scannerState = {
  scanning: ref(false),
  importing: ref(false),
  steamGames: ref([]),
  lutrisGames: ref([]),
  heroicGames: ref([]),
  emulatorGames: ref([]),
  librarySources: ref([]),
  error: ref(null),
  scan: vi.fn(),
  importSelected: vi.fn(() => Promise.resolve(0)),
  toggleAll: vi.fn(),
}

vi.mock('./composables/useGameScanner', () => ({
  useGameScanner: () => scannerState,
}))

const i18n = {
  t(key) { return key },
}

function flushAppsViewLoad() {
  return Promise.resolve().then(() => Promise.resolve()).then(() => nextTick())
}

function mountAppsView({ apps, currentApp = '', currentAppAvailable }) {
  global.fetch = vi.fn((url) => {
    if (String(url).includes('./api/apps')) {
      return Promise.resolve({
        json: () => Promise.resolve({ apps, current_app: currentApp, current_app_available: currentAppAvailable, host_name: 'Test Host', host_uuid: 'host-1' }),
      })
    }

    if (String(url).includes('./api/config')) {
      return Promise.resolve({
        json: () => Promise.resolve({ platform: 'linux' }),
      })
    }

    return Promise.resolve({ json: () => Promise.resolve({ status: true }) })
  })

  return shallowMount(AppsView, {
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t.bind(i18n) },
      stubs: {
        Button: { props: ['disabled'], emits: ['click'], template: '<button :disabled="disabled" @click="$emit(\'click\')"><slot /></button>' },
        InfoHint: { template: '<span><slot /></span>' },
        Checkbox: { template: '<label />' },
      },
    },
  })
}

describe('AppsView Library Quick Launch rail', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    delete global.fetch
    document.body.innerHTML = ''
  })

  it('blocks launch and stop during unavailable app identity and recovers with the new app', async () => {
    const appA = { uuid: 'app-a', name: 'Control A', cmd: 'run-a' }
    const appB = { uuid: 'app-b', name: 'Control B', cmd: 'run-b' }
    const wrapper = mountAppsView({ apps: [appA], currentApp: appA.uuid })
    await flushAppsViewLoad()
    wrapper.vm.requestCloseApp(appA)
    await nextTick()
    expect(wrapper.text()).toContain('Confirm stop')
    global.fetch.mockResolvedValueOnce({ json: async () => ({ apps: [appA], current_app: '', current_app_available: false }) })
    await wrapper.vm.loadApps()
    await nextTick()
    const before = global.fetch.mock.calls.length
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(true)
    wrapper.vm.launchApp({ target: { href: '' } }, appA)
    wrapper.vm.requestCloseApp(appA)
    wrapper.vm.closeApp({ confirmFirst: false })
    await flushAppsViewLoad()
    expect(global.fetch.mock.calls).toHaveLength(before)
    expect(confirm).not.toHaveBeenCalled()
    expect(wrapper.text()).not.toContain('Confirm stop')
    expect(wrapper.find('[data-quick-launch-app="app-a"] .control-chip').text()).toBe('Checking status')
    expect(wrapper.find('[data-quick-launch-app="app-a"] a').attributes('aria-disabled')).toBe('true')

    let resolveLate
    const lateBody = new Promise(resolve => { resolveLate = resolve })
    global.fetch.mockResolvedValueOnce({ json: () => lateBody })
    const staleRead = wrapper.vm.loadApps()
    global.fetch.mockResolvedValueOnce({ json: async () => ({ apps: [appA], current_app: '', current_app_available: false }) })
    await wrapper.vm.loadApps()
    resolveLate({ apps: [appA], current_app: appA.uuid })
    await staleRead
    expect(wrapper.vm.currentAppAvailable).toBe(false)
    expect(wrapper.vm.currentApp).toBe('')

    global.fetch.mockRejectedValueOnce(new Error('fixture read failed'))
    await wrapper.vm.loadApps()
    expect(wrapper.vm.currentAppAvailable).toBe(false)
    global.fetch.mockResolvedValueOnce({ json: async () => ({ apps: [appB], current_app: '', current_app_available: true }) })
    await wrapper.vm.loadApps()
    await nextTick()
    expect(wrapper.find('[data-quick-launch-app="app-a"]').exists()).toBe(false)
    expect(wrapper.find('[data-quick-launch-app="app-b"] .control-chip').text()).toBe('Ready')
    await wrapper.find('[data-quick-launch-app="app-b"] a').trigger('click')
    expect(global.fetch.mock.calls.some(([url, options]) => url === './api/apps/launch' && JSON.parse(options.body).uuid === appB.uuid)).toBe(true)
    wrapper.unmount()
  })

  it('renders a compact quick launch rail before the published app list', async () => {
    const wrapper = mountAppsView({
      apps: [
        { uuid: 'broken-favorite', name: 'Broken Favorite', favorite: true },
        { uuid: 'ready-basic', name: 'Ready Basic', cmd: 'run-basic' },
        { uuid: 'recent-ready', name: 'Recent Ready', cmd: 'run-recent', 'last-launched': 1780000000 },
        { uuid: 'favorite-recent', name: 'Favorite Recent', favorite: true, cmd: 'run-favorite', 'last-launched': 1781000000 },
      ],
    })
    await flushAppsViewLoad()

    const source = wrapper.html()
    expect(source.indexOf('library-quick-launch-rail')).toBeLessThan(source.indexOf('Library Surface'))
    expect(wrapper.find('.library-quick-launch-rail').text()).toContain('3 ready / 4 shown')
    expect(wrapper.find('.library-quick-launch-rail').text()).toContain('Favorites and recently played games, ready to launch.')

    expect(wrapper.findAll('[data-quick-launch-app]').map((node) => node.attributes('data-quick-launch-app'))).toEqual([
      'favorite-recent',
      'recent-ready',
      'ready-basic',
      'broken-favorite',
    ])

    const recentCard = wrapper.find('[data-quick-launch-app="recent-ready"]')
    expect(recentCard.find('.control-chip').text()).toBe('Ready')
    expect(recentCard.findAll('.meta-pill').filter((pill) => pill.text() === 'Recent')).toHaveLength(1)
    expect(recentCard.find('.library-quick-launch-action').classes()).not.toContain('w-full')
  })

  it('pins the running app and routes broken entries to repair copy instead of launch', async () => {
    const wrapper = mountAppsView({
      currentApp: 'ready-basic',
      apps: [
        { uuid: 'broken-entry', name: 'Broken Entry' },
        { uuid: 'ready-basic', name: 'Ready Basic', cmd: 'run-basic' },
        { uuid: 'recent-ready', name: 'Recent Ready', cmd: 'run-recent', 'last-launched': 1780000000 },
      ],
    })
    await flushAppsViewLoad()

    const cards = wrapper.findAll('[data-quick-launch-app]')
    expect(cards.map((node) => node.attributes('data-quick-launch-app'))).toEqual([
      'ready-basic',
      'recent-ready',
      'broken-entry',
    ])
    expect(cards[0].text()).toContain('Live')
    expect(cards[0].text()).toContain('Stop stream')
    expect(cards[2].text()).toContain('Needs fix')
    expect(cards[2].text()).toContain('Fix entry')
  })

  it("uses inline confirmation for Stop stream and reloads after confirmed close", async () => {
    const confirmSpy = vi.spyOn(window, "confirm").mockReturnValue(true)
    const wrapper = mountAppsView({
      currentApp: "ready-basic",
      apps: [
        { uuid: "ready-basic", name: "Ready Basic", cmd: "run-basic" },
      ],
    })
    await flushAppsViewLoad()

    const stopButton = wrapper.find("[data-quick-launch-app=ready-basic] button")
    await stopButton.trigger("click")
    await nextTick()

    expect(confirmSpy).not.toHaveBeenCalled()
    expect(global.fetch.mock.calls.some(([url]) => String(url).includes("./api/apps/close"))).toBe(false)
    expect(wrapper.find("[data-quick-launch-app=ready-basic] button").text()).toContain("Confirm stop")

    await wrapper.find("[data-quick-launch-app=ready-basic] button").trigger("click")
    await flushAppsViewLoad()

    const closeCall = global.fetch.mock.calls.find(([url]) => String(url).includes("./api/apps/close"))
    expect(closeCall).toBeTruthy()
    expect(closeCall[1]).toMatchObject({ credentials: "include", method: "POST" })
    expect(global.fetch.mock.calls.filter(([url]) => String(url).includes("./api/apps")).length).toBeGreaterThanOrEqual(3)
  })

})
