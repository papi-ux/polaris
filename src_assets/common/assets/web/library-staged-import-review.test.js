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
  toggleAll: vi.fn((value, source) => {
    const lists = {
      steam: scannerState.steamGames,
      lutris: scannerState.lutrisGames,
      heroic: scannerState.heroicGames,
    }
    const targets = source ? [lists[source].value] : Object.values(lists).map((list) => list.value)
    targets.forEach((games) => games.forEach((game) => {
      if (!game.already_imported) game.selected = value
    }))
  }),
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

function mountAppsView() {
  global.fetch = vi.fn((url) => {
    if (String(url).includes('./api/apps')) {
      return Promise.resolve({
        json: () => Promise.resolve({ apps: [], current_app: '', host_name: 'Test Host', host_uuid: 'host-1' }),
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

function resetScannerState() {
  scannerState.scanning.value = false
  scannerState.importing.value = false
  scannerState.error.value = null
  scannerState.steamGames.value = [
    { name: 'ARC Raiders', source: 'steam', appid: '1808500', selected: true, already_imported: false },
    { name: 'Hades', source: 'steam', appid: '1145360', selected: false, already_imported: true },
  ]
  scannerState.lutrisGames.value = []
  scannerState.heroicGames.value = [
    { name: 'Alan Wake 2', source: 'heroic', slug: 'alan-wake-2', runner: 'legendary', selected: true, already_imported: false },
  ]
  scannerState.scan.mockClear()
  scannerState.importSelected.mockClear()
  scannerState.toggleAll.mockClear()
}

describe('AppsView staged import review', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    delete global.fetch
    document.body.innerHTML = ''
  })

  it('keeps a sticky staged summary with source counts and primary import action', async () => {
    resetScannerState()
    const wrapper = mountAppsView()
    await flushAppsViewLoad()

    await wrapper.find('button').trigger('click')
    await nextTick()

    const summary = wrapper.find('.library-import-staged-summary')
    expect(summary.exists()).toBe(true)
    expect(summary.text()).toContain('2 games staged')
    expect(summary.text()).toContain('Steam 1')
    expect(summary.text()).toContain('Heroic 1')
    expect(summary.text()).toContain('Import 2 staged games')
  })

  it('reviews staged games and can remove one item or clear all staged entries', async () => {
    resetScannerState()
    const wrapper = mountAppsView()
    await flushAppsViewLoad()

    await wrapper.find('button').trigger('click')
    await nextTick()
    await wrapper.find('[data-import-review-open]').trigger('click')
    await nextTick()

    const drawer = wrapper.find('.library-import-review-drawer')
    expect(drawer.exists()).toBe(true)
    expect(drawer.text()).toContain('ARC Raiders')
    expect(drawer.text()).toContain('Alan Wake 2')
    expect(drawer.text()).toContain('Hades')
    expect(drawer.text()).toContain('Already imported')

    await wrapper.find('[data-import-stage-remove="1808500"]').trigger('click')
    await nextTick()
    expect(scannerState.steamGames.value[0].selected).toBe(false)
    expect(wrapper.find('.library-import-staged-summary').text()).toContain('1 game staged')

    await wrapper.find('[data-import-stage-clear-all]').trigger('click')
    await nextTick()
    expect(scannerState.heroicGames.value[0].selected).toBe(false)
    expect(wrapper.find('.library-import-staged-summary').text()).toContain('No games staged')
  })
})
