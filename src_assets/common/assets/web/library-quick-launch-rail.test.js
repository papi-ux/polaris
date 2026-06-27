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

function mountAppsView({ apps, currentApp = '' }) {
  global.fetch = vi.fn((url) => {
    if (String(url).includes('./api/apps')) {
      return Promise.resolve({
        json: () => Promise.resolve({ apps, current_app: currentApp, host_name: 'Test Host', host_uuid: 'host-1' }),
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

  it('renders a Moonlight-style quick launch rail before the published app list', async () => {
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
    expect(wrapper.find('.library-quick-launch-rail').text()).toContain('Moonlight-style shortcuts')

    expect(wrapper.findAll('[data-quick-launch-app]').map((node) => node.attributes('data-quick-launch-app'))).toEqual([
      'favorite-recent',
      'recent-ready',
      'ready-basic',
      'broken-favorite',
    ])
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
