import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ref } from 'vue'
import { readFileSync } from 'node:fs'

import TroubleshootingView from './views/TroubleshootingView.vue'

let channel
let wrapper
vi.mock('./composables/useStreamStats', () => ({ useStreamStats: () => channel }))
vi.mock('./composables/useToast', () => ({ useToast: () => ({ toast: vi.fn() }) }))

// The real English strings, so a missing locale key fails here instead of rendering a raw key.
const en = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const t = (key, params = {}) => {
  const value = key.split('.').reduce((node, part) => node?.[part], en)
  if (typeof value !== 'string') return key
  return value.replace(/\{(\w+)\}/g, (_, name) => String(params[name] ?? ''))
}

// The browser's own list, which only changes when a controller connects or disconnects.
let pads = []
const gamepad = (vibrates) => ({
  index: 0,
  id: 'Test Controller',
  buttons: [{}, {}],
  axes: [0, 0],
  vibrationActuator: vibrates ? { playEffect: vi.fn(async () => 'complete') } : undefined,
})

async function mountView() {
  channel = { stats: ref({ streaming: false }), connected: ref(true) }
  wrapper = shallowMount(TroubleshootingView, {
    attachTo: document.body,
    global: { provide: { i18n: { t } }, mocks: { $t: t }, directives: { tooltip: {} } },
  })
  await flushPromises()
}

const rumble = () => wrapper.get('[data-controller-rumble]')
const hint = () => wrapper.find('[data-controller-rumble-hint]')
const detect = () => wrapper.findAll('button').find((button) => button.text() === en.troubleshooting.controller_detect)

async function browserConnects(pad) {
  pads = [pad]
  window.dispatchEvent(new Event('gamepadconnected'))
  await flushPromises()
}

describe('controller tester in the production troubleshooting view', () => {
  beforeEach(() => {
    pads = []
    Object.defineProperty(navigator, 'getGamepads', { configurable: true, value: () => pads })
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({}), text: async () => '' })))
  })

  afterEach(() => {
    wrapper?.unmount()
    wrapper = null
    delete navigator.getGamepads
    vi.unstubAllGlobals()
  })

  it('explains a disabled rumble test and links the reason to the button', async () => {
    await mountView()
    expect(rumble().attributes('disabled')).toBeDefined()
    expect(hint().text()).toBe(en.troubleshooting.controller_rumble_detect_first)
    expect(rumble().attributes('aria-describedby')).toBe(hint().attributes('id'))
  })

  it('detects a controller that connected after the page opened and enables rumble', async () => {
    await mountView()
    // The controller arrives only now, as it does in a browser after a button press.
    await browserConnects(gamepad(true))
    await detect().trigger('click')
    await flushPromises()

    expect(rumble().attributes('disabled')).toBeUndefined()
    expect(hint().exists()).toBe(false)
    expect(rumble().attributes('aria-describedby')).toBeUndefined()

    await rumble().trigger('click')
    await flushPromises()
    expect(pads[0].vibrationActuator.playEffect).toHaveBeenCalledTimes(1)
  })

  it('says when the detected controller cannot vibrate in this browser', async () => {
    await mountView()
    await browserConnects(gamepad(false))
    await detect().trigger('click')
    await flushPromises()

    expect(rumble().attributes('disabled')).toBeDefined()
    expect(hint().text()).toBe(en.troubleshooting.controller_rumble_unsupported)
  })

  it('asks to detect again once the controller disconnects', async () => {
    await mountView()
    await browserConnects(gamepad(true))
    await detect().trigger('click')
    await flushPromises()
    expect(rumble().attributes('disabled')).toBeUndefined()

    pads = []
    window.dispatchEvent(new Event('gamepaddisconnected'))
    await flushPromises()
    expect(rumble().attributes('disabled')).toBeDefined()
    expect(hint().text()).toBe(en.troubleshooting.controller_rumble_detect_first)
  })

  it('detects a later controller without needing a browser connection event', async () => {
    await mountView()
    pads = [gamepad(true)]
    await detect().trigger('click')
    await flushPromises()
    expect(rumble().attributes('disabled')).toBeUndefined()
  })

  it('uses a capable controller beyond the first slot and retires it when another remains', async () => {
    await mountView()
    pads = [gamepad(false), { ...gamepad(true), index: 1 }]
    window.dispatchEvent(new Event('gamepadconnected'))
    await detect().trigger('click')
    await flushPromises()
    expect(rumble().attributes('disabled')).toBeUndefined()
    await rumble().trigger('click')
    expect(pads[1].vibrationActuator.playEffect).toHaveBeenCalledTimes(1)
    pads = [pads[0]]
    window.dispatchEvent(new Event('gamepaddisconnected'))
    await flushPromises()
    expect(rumble().attributes('disabled')).toBeDefined()
    expect(hint().text()).toBe(en.troubleshooting.controller_rumble_detect_first)
    await detect().trigger('click')
    await flushPromises()
    expect(hint().text()).toBe(en.troubleshooting.controller_rumble_unsupported)
  })

  it('stops listening for controllers when the page closes', async () => {
    const removed = vi.spyOn(window, 'removeEventListener')
    await mountView()
    wrapper.unmount()
    wrapper = null
    const events = removed.mock.calls.map(([name]) => name)
    expect(events).toContain('gamepadconnected')
    expect(events).toContain('gamepaddisconnected')
    removed.mockRestore()
  })
})
