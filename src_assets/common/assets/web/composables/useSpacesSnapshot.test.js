import { mount } from '@vue/test-utils'
import { defineComponent } from 'vue'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { useSpacesSnapshot } from './useSpacesSnapshot.js'

const snapshot = extra => ({ enabled: true, available: true, changing: false, failed: false, profiles: [], ...extra })
const reply = (body, ok = true) => ({ ok, status: ok ? 200 : 503, json: async () => body })
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.useRealTimers() })

function host(options = {}) {
  const Host = defineComponent({
    setup() {
      const api = useSpacesSnapshot({ intervalMs: 1000, maxBackoffMs: 8000, ...options })
      api.start()
      return api
    },
    template: '<div>{{ state.enabled }}</div>',
  })
  wrapper = mount(Host)
  return wrapper
}

describe('useSpacesSnapshot', () => {
  it('does not start polling from a visibility event before start is called', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    wrapper = mount(defineComponent({ setup: () => useSpacesSnapshot(), template: '<div />' }))
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(30000)
    expect(fetch).not.toHaveBeenCalled()
  })
  it('can stop polling for disabled Spaces and resume on an explicit refresh', async () => {
    vi.useFakeTimers()
    let enabled = false
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot({ enabled }))))
    host({ shouldPoll: () => enabled })
    await vi.advanceTimersByTimeAsync(30000)
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(0)
    expect(fetch).toHaveBeenCalledTimes(1)
    enabled = true
    await wrapper.vm.start()
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(3)
  })
  it('stop retires an in-flight read even if the transport completes after abort', async () => {
    vi.useFakeTimers()
    let resolve, signal
    vi.stubGlobal('fetch', vi.fn((_url, options) => { signal = options.signal; return new Promise(done => { resolve = done }) }))
    host()
    wrapper.vm.stop()
    expect(signal.aborted).toBe(true)
    resolve(reply(snapshot()))
    await vi.advanceTimersByTimeAsync(30000)
    expect(wrapper.vm.loaded).toBe(false)
    expect(wrapper.vm.loading).toBe(false)
    expect(fetch).toHaveBeenCalledTimes(1)
  })
  it('loads once, then polls on a completion-driven interval', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    host()
    await vi.advanceTimersByTimeAsync(0)
    expect(fetch).toHaveBeenCalledTimes(1)
    expect(wrapper.vm.loaded).toBe(true)
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(2)
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(3)
  })

  it('backs off while the host is not answering and recovers on success', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn().mockRejectedValueOnce(new Error('down')).mockRejectedValueOnce(new Error('down'))
      .mockResolvedValue(reply(snapshot())))
    host()
    await vi.advanceTimersByTimeAsync(0)
    expect(wrapper.vm.loadError).toBe('down')
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(2)
    await vi.advanceTimersByTimeAsync(4000)
    expect(fetch).toHaveBeenCalledTimes(3)
    expect(wrapper.vm.loadError).toBe('')
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(4)
  })

  it('skips a poll while the caller is busy and never overlaps a load in flight', async () => {
    vi.useFakeTimers()
    let busy = false
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    host({ busy: () => busy })
    await vi.advanceTimersByTimeAsync(0)
    busy = true
    await vi.advanceTimersByTimeAsync(3000)
    expect(fetch).toHaveBeenCalledTimes(1)
    busy = false
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(2)
  })

  it('resets keys the host may omit before merging a new snapshot', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot({ desktop_clients: ['a'], capacity: { concurrent_limit: 1, concurrent_active: 0 },
      activity: [], creation_available: true, removal_available: true }))).mockResolvedValueOnce(reply(snapshot())))
    host()
    await vi.waitFor(() => expect(wrapper.vm.state.desktop_clients).toEqual(['a']))
    expect(wrapper.vm.state.capacity).toEqual({ concurrent_limit: 1, concurrent_active: 0 })
    await wrapper.vm.load()
    expect(wrapper.vm.state.desktop_clients).toBeUndefined()
    expect(wrapper.vm.state.capacity).toBeNull()
    expect(wrapper.vm.state.activity).toBeNull()
    expect(wrapper.vm.state.creation_available).toBe(false)
    expect(wrapper.vm.state.removal_available).toBe(false)
  })

  it('reports an invalid snapshot with the verify message and keeps the last good state', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(snapshot({ profiles: [{ id: 'a', name: 'Alex', clients: [] }] })))
      .mockResolvedValueOnce(reply({ enabled: 'yes' })))
    host({ messages: { verify: 'Could not verify.' } })
    await vi.waitFor(() => expect(wrapper.vm.state.profiles).toHaveLength(1))
    expect(await wrapper.vm.load()).toBe(false)
    expect(wrapper.vm.loadError).toBe('Could not verify.')
    expect(wrapper.vm.state.profiles).toHaveLength(1)
  })
})
