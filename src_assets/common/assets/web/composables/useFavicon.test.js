import { mount } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useFavicon } from './useFavicon.js'
import { useStreamStats } from './useStreamStats.js'
import { spacesGlobal } from '../components/spaces-test-i18n.js'

let sources, wrappers
function start(withTitle = true) {
  const wrapper = mount(defineComponent({
    setup() {
      const { stats } = useStreamStats()
      if (withTitle) useFavicon(stats)
      return () => null
    },
  }), { global: spacesGlobal })
  wrappers.push(wrapper)
  return wrapper
}
async function receive(stats) {
  sources[0].onmessage({ data: JSON.stringify(stats) })
  await nextTick()
}

beforeEach(() => {
  sources = []; wrappers = []
  vi.useFakeTimers()
  vi.stubGlobal('fetch', vi.fn(() => new Promise(() => {})))
  vi.stubGlobal('EventSource', class {
    constructor(url) { this.url = url; this.close = vi.fn(); sources.push(this) }
  })
  document.title = 'Polaris original title'
})
afterEach(() => {
  wrappers.reverse().forEach(wrapper => wrapper.unmount())
  document.head.querySelectorAll('link[rel="icon"]').forEach(link => link.remove())
  vi.clearAllTimers()
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe('streaming browser tab', () => {
  it('follows idle → live → idle and client-name changes on the existing stats transport', async () => {
    const icon = document.createElement('link')
    icon.rel = 'icon'
    document.head.appendChild(icon)
    start()
    expect(document.title).toBe('Polaris original title')
    await receive({ streaming: false, client_name: 'Previous TV' })
    expect(document.title).toBe('Polaris original title')
    await receive({ streaming: true, client_name: 'Living Room TV' })
    expect(document.title).toBe('● Streaming to Living Room TV')
    expect(icon.getAttribute('href')).toContain('polaris-favicon.svg')
    await receive({ streaming: true, client_name: 'Steam Deck' })
    expect(document.title).toBe('● Streaming to Steam Deck')
    await receive({ streaming: false, last_session: { client_name: 'Steam Deck' } })
    expect(document.title).toBe('Polaris original title')
    expect(icon.getAttribute('href')).toContain('polaris-favicon.ico')
    expect(sources).toHaveLength(1)
    expect(sources[0].url).toBe('./api/stats/stream-sse')
    expect(fetch).not.toHaveBeenCalled()
  })

  it('uses an already-reported live session immediately, even without an icon element', async () => {
    start(false)
    await receive({ streaming: true, client_name: '  Nova Deck  ' })
    start()
    expect(document.title).toBe('● Streaming to Nova Deck')
    expect(sources).toHaveLength(1)
    expect(fetch).not.toHaveBeenCalled()
  })

  it.each([undefined, '', '   ', 42, { name: 'Saved TV' }])('does not invent a device for an unavailable display name (%j)', async (client_name) => {
    start()
    await receive({ streaming: true, client_name, client_uuid: 'private-device-id', client_type: 'Saved device type',
      last_session: { client_name: 'Previous TV' } })
    expect(document.title).toBe('● Streaming')
  })

  it('drops a former client name when the live report no longer names a device', async () => {
    start()
    await receive({ streaming: true, client_name: 'Living Room TV' })
    await receive({ streaming: true })
    expect(document.title).toBe('● Streaming')
  })

  it('clears the title on feed failure and waits for a new report before showing live again', async () => {
    start()
    await receive({ streaming: true, client_name: 'Living Room TV' })
    expect(document.title).toBe('● Streaming to Living Room TV')
    sources[0].onerror()
    await nextTick()
    expect(document.title).toBe('Polaris original title')
    expect(fetch).toHaveBeenCalledTimes(1)
  })

  it('clears a stale live title when the existing channel watchdog expires', async () => {
    start()
    await receive({ streaming: true, client_name: 'Living Room TV' })
    expect(document.title).toBe('● Streaming to Living Room TV')
    await vi.advanceTimersByTimeAsync(5000)
    expect(document.title).toBe('Polaris original title')
    expect(sources[0].close).toHaveBeenCalledTimes(1)
    expect(fetch).toHaveBeenCalledTimes(1)
  })

  it.each([null, { streaming: 'true', client_name: 'Saved TV' }])('does not keep a live claim after an unavailable or malformed report (%j)', async (stats) => {
    start()
    await receive({ streaming: true, client_name: 'Living Room TV' })
    expect(document.title).toBe('● Streaming to Living Room TV')
    await receive(stats)
    expect(document.title).toBe('Polaris original title')
  })

  it('restores the original title on disposal while another stats consumer remains', async () => {
    start(false)
    const titleOwner = start()
    await receive({ streaming: true, client_name: 'Living Room TV' })
    expect(document.title).toBe('● Streaming to Living Room TV')
    titleOwner.unmount()
    wrappers.pop()
    expect(document.title).toBe('Polaris original title')
    await receive({ streaming: true, client_name: 'Other client' })
    expect(document.title).toBe('Polaris original title')
    expect(sources[0].close).not.toHaveBeenCalled()
  })
})
