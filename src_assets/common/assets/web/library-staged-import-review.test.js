import { shallowMount } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'

import { forgetInstallJobs } from './composables/useRomSources'
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

// The console's English, so an assertion reads what a player sees and a missing key shows as itself.
const enLocale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))
const i18n = {
  t(key, params = {}) {
    const message = key.split('.').reduce((node, part) => node?.[part], enLocale)
    if (typeof message !== 'string') return key
    return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
  },
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

describe('AppsView ROM folder emulator install', () => {
  afterEach(() => {
    forgetInstallJobs()
    vi.useRealTimers()
    vi.restoreAllMocks()
    delete global.fetch
    document.body.innerHTML = ''
  })

  it('installs a missing emulator from Flathub, shows it running, then what the emulator needs next', async () => {
    resetScannerState()
    const wrapper = mountAppsView()
    await flushAppsViewLoad()

    const hostFetch = global.fetch
    let job = null
    let installed = false
    const installRequests = []
    global.fetch = vi.fn((url, options = {}) => {
      if (url === './api/library/sources') {
        const install = installed ? { kind: 'flatpak', location: 'dev.eden_emu.eden' } : { kind: 'missing', location: '' }
        const prerequisites = installed
          ? [{ id: 'eden_keys_missing', severity: 'warning', message: 'Eden has no prod.keys, so no game will boot.', action: 'Copy prod.keys into the keys folder' }]
          : []
        return Promise.resolve({
          ok: true,
          json: () => Promise.resolve({
            status: true,
            presets: [{ id: 'eden', label: 'Eden', platform: 'Nintendo Switch', install, installable: true, install_job: job, prerequisites }],
            sources: [{
              id: 'folder-1',
              emulator: 'eden',
              label: 'Eden',
              path: '/roms/switch',
              install,
              installable: true,
              install_job: job,
              warning: installed ? '' : 'Eden is not installed on this host, so games from this folder will not start until it is.',
              prerequisites,
            }],
          }),
        })
      }
      if (url === './api/library/emulators/install') {
        installRequests.push({ method: options.method, body: JSON.parse(options.body) })
        job = { state: 'installing', message: 'Installing Eden from Flathub.', started_at: 1, finished_at: 0 }
        return Promise.resolve({ ok: true, status: 202, json: () => Promise.resolve({ status: true, install_job: job }) })
      }
      return hostFetch(url, options)
    })
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })

    await wrapper.find('button').trigger('click')
    await flushAppsViewLoad()
    await flushAppsViewLoad()

    const card = () => wrapper.find('[data-rom-folder]')
    expect(card().text()).toContain('Eden not found')
    expect(card().text()).toContain('will not start until it is')
    expect(wrapper.find('[data-rom-folder-install]').text()).toBe('Install From Flathub')

    await wrapper.find('[data-rom-folder-install]').trigger('click')
    await flushAppsViewLoad()
    await flushAppsViewLoad()
    expect(installRequests).toEqual([{ method: 'POST', body: { emulator: 'eden' } }])
    expect(wrapper.find('[data-rom-folder-installing]').text()).toContain('Installing Eden from Flathub.')
    expect(wrapper.find('[data-rom-folder-install]').text()).toBe('Installing')
    expect(wrapper.find('[data-rom-folder-install]').attributes('disabled')).toBeDefined()

    const scansBefore = scannerState.scan.mock.calls.length
    job = { state: 'installed', message: 'Eden is installed.', started_at: 1, finished_at: 2 }
    installed = true
    await vi.advanceTimersByTimeAsync(2000)
    await flushAppsViewLoad()
    await flushAppsViewLoad()

    expect(card().text()).toContain('Eden via Flatpak')
    expect(wrapper.find('[data-rom-folder-install]').exists()).toBe(false)
    expect(wrapper.find('[data-rom-folder-installing]').exists()).toBe(false)
    expect(wrapper.find('[data-rom-folder-check]').text()).toContain('prod.keys')
    expect(scannerState.scan.mock.calls.length).toBe(scansBefore + 1)
  })

  it('keeps Flatpak\'s reason on the card when an install fails', async () => {
    resetScannerState()
    const wrapper = mountAppsView()
    await flushAppsViewLoad()

    const hostFetch = global.fetch
    global.fetch = vi.fn((url, options) => {
      if (url === './api/library/sources') {
        const failed = { state: 'failed', message: 'Installing DuckStation from Flathub failed: Nothing matches org.duckstation.DuckStation in remote flathub', started_at: 1, finished_at: 2 }
        return Promise.resolve({
          ok: true,
          json: () => Promise.resolve({
            status: true,
            presets: [{ id: 'duckstation', label: 'DuckStation', platform: 'PlayStation', install: { kind: 'missing', location: '' }, installable: true, install_job: failed }],
            sources: [{ id: 'folder-2', emulator: 'duckstation', label: 'DuckStation', path: '/roms/psx', install: { kind: 'missing', location: '' }, installable: true, install_job: failed, prerequisites: [] }],
          }),
        })
      }
      return hostFetch(url, options)
    })

    await wrapper.find('button').trigger('click')
    await flushAppsViewLoad()
    await flushAppsViewLoad()

    expect(wrapper.find('[data-rom-folder-install-failed]').text()).toBe('Installing DuckStation from Flathub failed: Nothing matches org.duckstation.DuckStation in remote flathub')
    expect(wrapper.find('[data-rom-folder-install]').text()).toBe('Install From Flathub')
  })
})
