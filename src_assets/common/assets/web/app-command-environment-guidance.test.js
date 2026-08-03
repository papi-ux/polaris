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

describe('AppsView command environment guidance', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    delete global.fetch
    document.body.innerHTML = ''
  })

  it('keeps environment-variable guidance visible and opens the related editor', async () => {
    const wrapper = mountAppsView()
    await flushAppsViewLoad()

    const addButton = wrapper.findAll('button').find((button) => button.text().includes('apps.add_new'))
    expect(addButton).toBeTruthy()
    await addButton.trigger('click')
    await nextTick()

    const guidance = wrapper.find('[data-command-environment-help]')
    expect(guidance.exists()).toBe(true)
    expect(guidance.text()).toContain('executable')
    expect(guidance.text()).toContain('DRI_PRIME=1')

    const shortcut = guidance.find('button[aria-controls="appStreamingTweaks"]')
    expect(shortcut.exists()).toBe(true)
    expect(shortcut.text()).toContain('Environment & streaming tweaks')
    expect(shortcut.attributes('aria-expanded')).toBe('false')

    const tweaks = wrapper.find('#appStreamingTweaks')
    expect(tweaks.exists()).toBe(true)
    expect(tweaks.find('summary').text()).toContain('Environment & streaming tweaks')
    expect(tweaks.attributes('open')).toBeUndefined()

    await shortcut.trigger('click')
    await nextTick()

    expect(shortcut.attributes('aria-expanded')).toBe('true')
    expect(wrapper.find('#appStreamingTweaks').attributes('open')).toBeDefined()
    expect(wrapper.find('#appStreamingTweaks .app-editor-disclosure-body').exists()).toBe(true)
  })
})
