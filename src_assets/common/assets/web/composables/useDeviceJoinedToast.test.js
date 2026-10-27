import { effectScope, nextTick, ref } from 'vue'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import messages from '../public/assets/locale/en.json'
import Toast from '../components/Toast.vue'
import { useClients } from './useClients.js'
import { useStreamStats } from './useStreamStats.js'
import { useToast } from './useToast.js'
import { useDeviceJoinedToast } from './useDeviceJoinedToast.js'

const deck = { uuid: 'paired-deck', name: 'Steam Deck', client_family: 'nova', connected: false }
const liveDeck = { name: 'Steam Deck', client_family: 'nova', stream_instance_id: 'host:1' }

describe('device joined notices from existing feeds', () => {
  let scopes
  let wrappers
  let t
  const { toasts, toast, dismiss, immediateDismissals } = useToast()

  function observe(source, options = {}) {
    const scope = effectScope()
    scopes.push(scope)
    scope.run(() => useDeviceJoinedToast(source, { t, ...options }))
    return scope
  }

  beforeEach(() => {
    vi.useFakeTimers()
    scopes = []
    wrappers = []
    t = createI18n({ legacy: false, locale: 'en', messages: { en: messages } }).global.t
  })

  afterEach(() => {
    scopes.forEach(scope => scope.stop())
    wrappers.forEach(wrapper => wrapper.unmount())
    toasts.value.forEach(toast => dismiss(toast.id))
    vi.clearAllTimers()
    vi.useRealTimers()
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
    document.body.innerHTML = ''
  })

  it('silently seeds paired clients, announces one new pair and suppresses duplicate polls and edits', async () => {
    const snapshot = ref(null)
    observe(snapshot)
    snapshot.value = [{ ...deck, connected: true }]
    await nextTick()
    expect(toasts.value).toHaveLength(0)

    snapshot.value = [{ ...deck, connected: true }, { uuid: 'new-device', name: 'Shield' }]
    await nextTick()
    expect(toasts.value.map(toast => toast.message)).toEqual(['Shield joined'])
    const id = toasts.value[0].id
    snapshot.value = [{ uuid: 'new-device', name: 'Shield renamed', perm: 7 }, { ...deck, connected: true }]
    await nextTick()
    expect(toasts.value.map(toast => toast.id)).toEqual([id])
  })

  it('announces an offline-to-connected edge once and allows a true reconnect', async () => {
    const snapshot = ref([deck])
    observe(snapshot)
    snapshot.value = [{ ...deck, connected: true }]
    await nextTick()
    expect(toasts.value.map(toast => toast.message)).toEqual(['Steam Deck (Nova) joined'])
    const firstId = toasts.value[0].id
    snapshot.value = [{ ...deck, connected: true }]
    await nextTick()
    expect(toasts.value[0].id).toBe(firstId)
    dismiss(firstId)
    snapshot.value = [deck]
    await nextTick()
    snapshot.value = [{ ...deck, connected: true }]
    await nextTick()
    expect(toasts.value).toHaveLength(1)
    expect(toasts.value[0].id).not.toBe(firstId)
  })

  it('uses stream identities for a reconnect between polls and for same-name viewers', async () => {
    const stats = ref({ streaming: true, clients: [liveDeck] })
    observe(stats, { stream: true })
    expect(toasts.value).toHaveLength(0)
    stats.value = { streaming: true, clients: [liveDeck, { ...liveDeck, stream_instance_id: 'host:2' }] }
    await nextTick()
    expect(toasts.value).toHaveLength(1)
    const id = toasts.value[0].id
    stats.value = { streaming: true, clients: [{ ...liveDeck, stream_instance_id: 'host:2' }, liveDeck] }
    await nextTick()
    expect(toasts.value[0].id).toBe(id)
    dismiss(id)
    stats.value = { streaming: true, clients: [{ ...liveDeck, stream_instance_id: 'host:3' }] }
    await nextTick()
    expect(toasts.value[0].id).not.toBe(id)
    stats.value = { streaming: false, clients: [] }
    await nextTick()
    stats.value = { streaming: true, ...liveDeck, client_name: liveDeck.name }
    await nextTick()
    expect(toasts.value[0].message).toBe('Steam Deck (Nova) joined')
  })

  it('waits for an identified stream snapshot instead of announcing incomplete initial state', async () => {
    const stats = ref({ streaming: true, client_name: 'Steam Deck' })
    observe(stats, { stream: true })
    stats.value = { streaming: true, clients: [liveDeck] }
    await nextTick()
    expect(toasts.value).toHaveLength(0)
    stats.value = { streaming: false }
    await nextTick()
    stats.value = { streaming: true, client_name: 'Shield', stream_instance_id: 'host:2' }
    await nextTick()
    expect(toasts.value[0].message).toBe('Shield joined')
  })

  it('ignores unavailable, unnamed and unidentified readings without inventing a type', async () => {
    const snapshot = ref([])
    observe(snapshot)
    snapshot.value = [{ uuid: 'nameless', client_family: 'nova' }, { name: 'No identity' }]
    await nextTick()
    expect(toasts.value).toHaveLength(0)
    snapshot.value = [{ uuid: 'unknown-kind', name: 'My handheld', client_family: 'unreported-kind' }]
    await nextTick()
    expect(toasts.value[0].message).toBe('My handheld joined')
    dismiss(toasts.value[0].id)
    snapshot.value = null
    await nextTick()
    snapshot.value = [{ ...deck, connected: true }]
    await nextTick()
    expect(toasts.value).toHaveLength(0)
  })

  it('stays silent after an initial paired-client read fails and on failed polls', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => {})
    const payloads = [
      { status: false },
      { status: true, named_certs: [{ ...deck, connected: true }] },
      { status: false },
      { status: true, named_certs: [{ ...deck, connected: true }] },
      { status: true, named_certs: [deck] },
      { status: true, named_certs: [{ ...deck, connected: true }] },
    ]
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => payloads.shift() })))
    const { clients, clientsReady, refreshClients } = useClients()
    observe(() => clientsReady.value ? clients.value : null)
    for (let index = 0; index < 5; index++) {
      await refreshClients()
      await nextTick()
      expect(toasts.value).toHaveLength(0)
    }
    await refreshClients()
    await nextTick()
    expect(toasts.value[0].message).toBe('Steam Deck (Nova) joined')
    expect(fetch).toHaveBeenCalledTimes(6)
  })

  it('observes the shared stats transport without adding a connection or treating its recovery as a join', async () => {
    const sources = []
    vi.stubGlobal('EventSource', class {
      constructor() { sources.push(this) }
      close() {}
    })
    let channel
    wrappers.push(mount({ setup() {
      channel = useStreamStats()
      useDeviceJoinedToast(() => channel.connected.value ? channel.stats.value : null, { t, stream: true })
      return () => null
    } }))
    const receive = async data => {
      sources[0].onmessage({ data: JSON.stringify(data) })
      await nextTick()
    }
    await receive({ streaming: true, clients: [liveDeck] })
    expect(toasts.value).toHaveLength(0)
    await receive({ streaming: false, clients: [] })
    await receive({ streaming: true, clients: [{ ...liveDeck, stream_instance_id: 'host:2' }] })
    expect(toasts.value).toHaveLength(1)
    dismiss(toasts.value[0].id)
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, status: 200, json: async () => ({ streaming: true, clients: [liveDeck] }) })))
    sources[0].onerror()
    await nextTick()
    await flushPromises()
    expect(toasts.value).toHaveLength(0)
    expect(sources).toHaveLength(1)
    expect(fetch).toHaveBeenCalledTimes(1)
  })

  it('uses the existing polite, dismissible toast and clears it on navigation before gameplay', async () => {
    wrappers.push(mount(Toast, { attachTo: document.body }))
    const snapshot = ref([])
    const scope = observe(snapshot)
    snapshot.value = [{ ...deck, name: '<b>Steam Deck</b>' }]
    await nextTick()
    const notice = document.querySelector('[role="status"]')
    expect(notice.textContent).toContain('<b>Steam Deck</b> (Nova) joined')
    expect(notice.querySelector('b')).toBeNull()
    expect(notice.getAttribute('aria-live')).toBe('polite')
    expect(notice.querySelector('button').getAttribute('aria-label')).toBe('Dismiss notification')
    notice.querySelector('button').click()
    await nextTick()
    expect(toasts.value).toHaveLength(0)
    snapshot.value = []
    await nextTick()
    snapshot.value = [deck]
    await nextTick()
    scope.stop()
    await nextTick()
    expect(toasts.value).toHaveLength(0)
    snapshot.value = [{ uuid: 'after-navigation', name: 'Shield' }]
    await nextTick()
    expect(toasts.value).toHaveLength(0)
  })

  it('silently re-seeds after a hidden tab or a page remount and expires a notice normally', async () => {
    const snapshot = ref([])
    const scope = observe(snapshot)
    const hidden = vi.spyOn(document, 'hidden', 'get').mockReturnValue(true)
    document.dispatchEvent(new Event('visibilitychange'))
    snapshot.value = [deck]
    await nextTick()
    hidden.mockReturnValue(false)
    document.dispatchEvent(new Event('visibilitychange'))
    snapshot.value = [{ ...deck, connected: true }]
    await nextTick()
    expect(toasts.value).toHaveLength(0)
    scope.stop()
    observe(snapshot)
    expect(toasts.value).toHaveLength(0)
    snapshot.value = [deck]
    await nextTick()
    snapshot.value = [{ ...deck, connected: true }]
    await nextTick()
    expect(toasts.value).toHaveLength(1)
    await vi.advanceTimersByTimeAsync(3000)
    expect(toasts.value).toHaveLength(0)
  })

  it('hides only disposed notices before the next frame while Vue owns their leave', async () => {
    wrappers.push(mount(Toast, { attachTo: document.body, global: { stubs: { transition: false, 'transition-group': false } } }))
    const snapshot = ref([])
    const scope = observe(snapshot)
    snapshot.value = [deck]
    await nextTick()
    const ownedId = toasts.value[0].id
    const otherId = toast('Unrelated notification')
    await nextTick()
    const owned = document.querySelector(`[data-toast-id="${ownedId}"]`)
    const other = document.querySelector(`[data-toast-id="${otherId}"]`)

    scope.stop()
    expect(immediateDismissals.value.has(ownedId)).toBe(true)
    await nextTick()
    expect(toasts.value.map(notice => notice.id)).toEqual([otherId])
    expect(owned.isConnected).toBe(true)
    expect(owned.classList.contains('toast-leave-active')).toBe(true)
    expect(owned.style.display).toBe('none')
    expect(other.style.display).toBe('')
    expect(immediateDismissals.value.size).toBe(0)
  })

  it('preserves ordinary dismiss and timeout leaves, including a notice already leaving on disposal', async () => {
    wrappers.push(mount(Toast, { attachTo: document.body, global: { stubs: { transition: false, 'transition-group': false } } }))
    const snapshot = ref([])
    const scope = observe(snapshot)
    snapshot.value = [deck]
    await nextTick()
    const ownedId = toasts.value[0].id
    const owned = document.querySelector(`[data-toast-id="${ownedId}"]`)
    dismiss(ownedId)
    await nextTick()
    expect(owned.classList.contains('toast-leave-active')).toBe(true)
    expect(owned.style.display).toBe('')
    scope.stop()
    expect(owned.style.display).toBe('none')
    await nextTick()

    const timedId = toast('Ordinary timeout')
    await nextTick()
    const timed = document.querySelector(`[data-toast-id="${timedId}"]`)
    await vi.advanceTimersByTimeAsync(3000)
    expect(timed.classList.contains('toast-leave-active')).toBe(true)
    expect(timed.style.display).toBe('')
    expect(toasts.value).toHaveLength(0)
  })
})
