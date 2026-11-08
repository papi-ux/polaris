import { readFileSync } from 'node:fs'
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import LiveSessionCard from './LiveSessionCard.vue'
import ConfirmActionDialog from './ConfirmActionDialog.vue'
import { connectionChip, sessionElapsedLabel } from '../live-session.js'

const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
const t = i18n.global.t
const peer = (transport, confirmed = true) => ({ scope: 'peer_status', confirmed, transport, observed_at_ms: 1, reason: 'observed' })
const answer = (transport, confirmed = true) => ({
  schema_version: 1, status: confirmed ? 'tailscale' : 'unknown', stream_instance_id: 'stream-1',
  tailscale: { confirmed, transport, scope: 'peer_status', source: 'tailscaled_localapi', observed_at_ms: Date.now(), max_age_ms: 10000, reason: 'observed' },
})
const live = { streaming: true, stream_instance_id: 'stream-1', session_elapsed_s: 720 }
const views = []
let network
async function card(props = {}, options = {}) {
  const view = mount(LiveSessionCard, {
    attachTo: options.attach ? document.body : undefined,
    props: { title: 'Hades', appId: 'hades', artSrc: './api/covers/image?name=Hades', device: 'RP6 · Nova', stats: live, canDisconnect: true, canEnd: true, ...props },
    global: { plugins: [i18n], stubs: { ConfirmActionDialog: !options.attach } },
  })
  views.push(view)
  await flushPromises()
  return view
}
beforeEach(() => {
  network = null
  vi.stubGlobal('fetch', vi.fn(async url => ({ ok: true, json: async () => (url === './api/apps/close' ? { status: true } : network) })))
})
afterEach(() => {
  views.splice(0).forEach(view => view.unmount())
  document.body.innerHTML = ''
  vi.unstubAllGlobals()
})

describe('LiveSessionCard', () => {
  it('shows the game, its art, the device, the time and both actions', async () => {
    network = answer('direct')
    const view = await card()
    expect(view.get('[data-session-title]').text()).toBe('Hades')
    expect(view.get('[data-session-art]').attributes('src')).toBe('./api/covers/image?name=Hades')
    expect(view.get('[data-session-device] dd').text()).toBe('RP6 · Nova')
    expect(view.get('[data-session-elapsed] dd').text()).toBe('12 min')
    expect(view.get('[data-session-connection]').text()).toContain('Tailscale · direct')
    expect(view.get('[data-disconnect-client]').text()).toBe('Disconnect Client')
    expect(view.get('[data-end-session]').text()).toBe('End')
  })

  it('names both actions and the card for a screen reader', async () => {
    network = answer('derp')
    const view = await card()
    expect(view.get('[data-live-session-card]').attributes('aria-label')).toBe('Live session')
    expect(view.get('[data-disconnect-client]').attributes('aria-label')).toBe('Disconnect Client RP6 · Nova')
    expect(view.get('[data-end-session]').attributes('aria-label')).toBe('End Hades')
    // The chip's limit is read out too, not left to a hover.
    expect(view.get('[data-session-connection] .sr-only').text()).toContain('has not been verified')
  })

  it('leaves out what the host has not reported rather than filling it in', async () => {
    const view = await card({ device: '', stats: { streaming: true, stream_instance_id: 'stream-1' } })
    expect(view.find('[data-session-device]').exists()).toBe(false)
    expect(view.find('[data-session-elapsed]').exists()).toBe(false)
    expect(view.find('[data-session-connection]').exists()).toBe(false)
    expect(view.text()).not.toContain('Unknown')
  })

  it('counts from the start this page saw when the host sends no count', async () => {
    const view = await card({ stats: { streaming: true, stream_instance_id: 'stream-1' }, witnessedStartMs: Date.now() - 125000 })
    expect(view.get('[data-session-elapsed] dd').text()).toBe('2 min')
  })

  it('shows no chip for a stream the host could not confirm, or for another stream', async () => {
    network = answer('unknown', false)
    const unconfirmed = await card()
    expect(unconfirmed.find('[data-session-connection]').exists()).toBe(false)
    network = { ...answer('direct'), stream_instance_id: 'another-stream' }
    const other = await card()
    expect(other.find('[data-session-connection]').exists()).toBe(false)
  })

  it('asks for evidence only while streaming', async () => {
    await card({ stats: { streaming: false } })
    expect(fetch.mock.calls.filter(([url]) => url === './api/network/connection')).toHaveLength(0)
    await card()
    expect(fetch.mock.calls.filter(([url]) => url === './api/network/connection')).toHaveLength(1)
  })

  it('falls back to a plain tile when there is no cover or it fails to load, and retries a new one', async () => {
    const none = await card({ artSrc: '' })
    expect(none.find('[data-session-art]').exists()).toBe(false)
    expect(none.find('[data-session-art-fallback]').exists()).toBe(true)
    const view = await card()
    await view.get('[data-session-art]').trigger('error')
    expect(view.find('[data-session-art]').exists()).toBe(false)
    expect(view.find('[data-session-art-fallback]').exists()).toBe(true)
    await view.setProps({ artSrc: './api/covers/image?name=Control' })
    expect(view.get('[data-session-art]').attributes('src')).toBe('./api/covers/image?name=Control')
  })

  it('names the session when the host has not said which game is running', async () => {
    expect((await card({ title: '' })).get('[data-session-title]').text()).toBe('Live session')
  })

  it('offers an action only where it is allowed', async () => {
    expect((await card({ canDisconnect: false })).find('[data-disconnect-client]').exists()).toBe(false)
    expect((await card({ canEnd: false })).find('[data-end-session]').exists()).toBe(false)
    expect((await card({ canDisconnect: false, canEnd: false })).find('.mc-session-actions').exists()).toBe(false)
  })

  it('asks the page to confirm Disconnect Client, and holds it while it runs', async () => {
    const view = await card()
    await view.get('[data-disconnect-client]').trigger('click')
    expect(view.emitted('disconnect')).toHaveLength(1)
    await view.setProps({ disconnecting: true })
    expect(view.get('[data-disconnect-client]').attributes('disabled')).toBeDefined()
  })

  const closes = () => fetch.mock.calls.filter(([url]) => url === './api/apps/close')
  const confirm = async () => { document.querySelector('[data-confirm-confirm]').click(); await flushPromises() }

  it('ends the session through the Library\'s own route, only after confirmation', async () => {
    const view = await card({}, { attach: true })
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    expect(document.body.textContent).toContain('Hades stops streaming.')
    document.querySelector('[data-confirm-cancel]').click(); await flushPromises()
    expect(closes()).toHaveLength(0)
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    await confirm()
    expect(closes()).toHaveLength(1)
    expect(closes()[0][1].method).toBe('POST')
    expect(view.emitted('ended')).toHaveLength(1)
    expect(document.querySelector('[data-confirm-confirm]')).toBeNull()
  })

  it('stays open and reports nothing ended when the host refuses', async () => {
    fetch.mockImplementation(async url => ({ ok: true, json: async () => (url === './api/apps/close' ? { status: false } : network) }))
    const view = await card({}, { attach: true })
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    await confirm()
    expect(view.emitted('ended')).toBeUndefined()
    expect(document.querySelector('[data-confirm-confirm]')).not.toBeNull()
  })

  it('closes an open confirmation when the session can no longer be ended', async () => {
    const view = await card({}, { attach: true })
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    await view.setProps({ canEnd: false }); await flushPromises()
    expect(document.querySelector('[data-confirm-confirm]')).toBeNull()
    expect(closes()).toHaveLength(0)
  })

  it('retires confirmation for a replacement app even when its name and stream stay the same', async () => {
    const view = await card({}, { attach: true })
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    await view.setProps({ appId: 'replacement-hades' }); await flushPromises()
    expect(document.querySelector('[data-confirm-confirm]')).toBeNull()
    expect(closes()).toHaveLength(0)
  })

  it('ignores a queued confirmation after its stream was replaced', async () => {
    const view = await card({}, { attach: true })
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    const dialog = view.getComponent(ConfirmActionDialog)
    await view.setProps({ stats: { ...live, stream_instance_id: 'replacement' } })
    dialog.vm.$emit('confirm'); await flushPromises()
    expect(closes()).toHaveLength(0)
  })

  it('keeps a same-session confirmation across telemetry updates and requires a fresh intent after Cancel', async () => {
    const view = await card({}, { attach: true })
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    await view.setProps({ stats: { ...live, fps: 59, session_elapsed_s: 721 } }); await flushPromises()
    expect(document.querySelector('[data-confirm-confirm]')).not.toBeNull()
    document.querySelector('[data-confirm-cancel]').click(); await flushPromises()
    view.getComponent(ConfirmActionDialog).vm.$emit('confirm'); await flushPromises()
    expect(closes()).toHaveLength(0)
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    await confirm()
    expect(closes()).toHaveLength(1)
  })

  it('retires End confirmation when another stream replaces the displayed session', async () => {
    const view = await card({}, { attach: true })
    await view.get('[data-end-session]').trigger('click'); await flushPromises()
    expect(document.querySelector('[data-confirm-confirm]')).not.toBeNull()

    await view.setProps({ stats: { ...live, stream_instance_id: 'stream-2' } })
    await flushPromises()

    expect(document.querySelector('[data-confirm-confirm]')).toBeNull()
    expect(closes()).toHaveLength(0)
  })

  it('reflows by its own width, keeps touch-sized actions and adds no motion', () => {
    const source = readFileSync('src_assets/common/assets/web/components/LiveSessionCard.vue', 'utf8')
    const style = source.slice(source.indexOf('<style scoped>'))
    expect(style).toContain('container: mc-session / inline-size')
    expect(style).toContain('@container mc-session (min-width: 36rem)')
    expect(style).toContain('min-height: 2.75rem')
    expect(style).not.toMatch(/animation|transition/)
  })
})

describe('connectionChip', () => {
  it.each([
    ['direct', 'Tailscale · direct'],
    ['derp', 'Tailscale · DERP relay'],
    ['peer_relay', 'Tailscale · peer relay'],
    ['unknown', 'Tailscale · type unknown'],
    ['something_new', 'Tailscale · type unknown'],
  ])('labels a confirmed %s connection', (transport, label) => {
    expect(connectionChip(peer(transport), t).label).toBe(label)
  })

  it('shows nothing the host has not confirmed, so a LAN stream is never called anything', () => {
    expect(connectionChip(peer('direct', false), t)).toBeNull()
    expect(connectionChip({ ...peer('direct'), scope: 'control_connection' }, t)).toBeNull()
    expect(connectionChip(null, t)).toBeNull()
  })
})

describe('sessionElapsedLabel', () => {
  it('prefers the host count, which holds for a page opened mid-stream', () => {
    expect(sessionElapsedLabel({ hostSeconds: 42 })).toBe('42s')
    expect(sessionElapsedLabel({ hostSeconds: 720, witnessedStartMs: 1000, now: 2000 })).toBe('12 min')
    expect(sessionElapsedLabel({ hostSeconds: 3900 })).toBe('1 h 5 min')
  })

  it('falls back to the start this page saw, and is empty when neither is known', () => {
    expect(sessionElapsedLabel({ witnessedStartMs: 1000, now: 91000 })).toBe('1 min')
    expect(sessionElapsedLabel({})).toBe('')
    expect(sessionElapsedLabel({ hostSeconds: '42' })).toBe('')
    expect(sessionElapsedLabel({ hostSeconds: null, witnessedStartMs: 0 })).toBe('')
  })

  it('never shows a negative time from a clock that moved', () => {
    expect(sessionElapsedLabel({ hostSeconds: -5 })).toBe('0s')
    expect(sessionElapsedLabel({ witnessedStartMs: 5000, now: 1000 })).toBe('0s')
  })
})
