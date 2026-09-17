import { flushPromises, shallowMount } from '@vue/test-utils'
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

const bundledDesktop = {
  name: 'Desktop',
  uuid: 'desktop-1',
  'image-path': 'desktop.png',
  'desktop-mirror': true,
  'allow-client-commands': false,
}

function mountAppsView({ platform = 'linux', apps = [] } = {}) {
  global.fetch = vi.fn((url, options = {}) => {
    if (String(url).includes('./api/apps') && options.method === 'POST') {
      return Promise.resolve({ json: () => Promise.resolve({ status: true }) })
    }
    if (String(url).includes('./api/apps')) {
      return Promise.resolve({
        json: () => Promise.resolve({ apps, current_app: '', host_name: 'Test Host', host_uuid: 'host-1' }),
      })
    }
    if (String(url).includes('./api/config')) {
      return Promise.resolve({ json: () => Promise.resolve({ platform }) })
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

function savedAppBody() {
  const call = global.fetch.mock.calls.find(([url, options]) => String(url).includes('./api/apps') && options?.method === 'POST')
  expect(call, 'the editor posts the app').toBeDefined()
  return JSON.parse(call[1].body)
}

describe('AppsView desktop mirroring setting (#715)', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    delete global.fetch
    document.body.innerHTML = ''
  })

  it('lets a Linux operator turn off mirroring on the bundled Desktop entry', async () => {
    const wrapper = mountAppsView({ apps: [bundledDesktop] })
    await flushPromises()

    wrapper.vm.editApp(bundledDesktop)
    await nextTick()
    expect(wrapper.find('#desktopMirror').exists()).toBe(true)
    expect(wrapper.vm.editForm['desktop-mirror']).toBe(true)

    wrapper.vm.editForm['desktop-mirror'] = false
    wrapper.vm.save()
    await flushPromises()
    expect(savedAppBody()['desktop-mirror']).toBe(false)
    wrapper.unmount()
  })

  it('saves a new entry with an explicit choice instead of leaving it to migration inference', async () => {
    const wrapper = mountAppsView()
    await flushPromises()

    wrapper.vm.newApp()
    await nextTick()
    // Keyless, this entry matches what the apps.json migrations treat as the
    // legacy bundled Desktop (manual, named Desktop, desktop.png, no commands,
    // client commands off), so its mirroring would be inferred, not chosen.
    wrapper.vm.editForm.name = 'Desktop'
    wrapper.vm.editForm['image-path'] = 'desktop.png'
    wrapper.vm.editForm['allow-client-commands'] = false
    wrapper.vm.save()
    await flushPromises()
    const body = savedAppBody()
    expect(Object.hasOwn(body, 'desktop-mirror')).toBe(true)
    expect(body['desktop-mirror']).toBe(false)
    wrapper.unmount()
  })

  it('leaves the setting off hosts where it has no effect', async () => {
    const wrapper = mountAppsView({ platform: 'windows', apps: [bundledDesktop] })
    await flushPromises()

    wrapper.vm.editApp(bundledDesktop)
    await nextTick()
    expect(wrapper.find('#virtualDisplay').exists()).toBe(true)
    expect(wrapper.find('#desktopMirror').exists()).toBe(false)
    wrapper.unmount()
  })
})
