import { mount } from '@vue/test-utils'
import { defineComponent } from 'vue'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { useStreamStats } from './useStreamStats.js'
import { useSystemStats } from './useSystemStats.js'

function deferred() {
  let resolve
  let reject
  const promise = new Promise((resolvePromise, rejectPromise) => {
    resolve = resolvePromise
    reject = rejectPromise
  })
  return { promise, resolve, reject }
}

function okResponse(payload = {}) {
  return {
    ok: true,
    status: 200,
    json: vi.fn().mockResolvedValue(payload),
  }
}

async function flushPromises() {
  await Promise.resolve()
  await Promise.resolve()
}

function mountComposable(factory) {
  const exposed = {}
  const wrapper = mount(defineComponent({
    setup() {
      Object.assign(exposed, factory())
      return () => null
    },
  }))
  return { exposed, wrapper }
}

function installMockEventSource() {
  const instances = []

  class MockEventSource {
    constructor(url) {
      this.url = url
      this.close = vi.fn()
      instances.push(this)
    }
  }

  vi.stubGlobal('EventSource', MockEventSource)
  return instances
}

describe('idle polling optimizations', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn())
    window.location.hash = ''
  })

  afterEach(() => {
    vi.clearAllTimers()
    vi.useRealTimers()
    vi.unstubAllGlobals()
    window.location.hash = ''
  })

  it('deduplicates system stats requests while a previous poll is still in flight', async () => {
    const first = deferred()
    fetch
      .mockReturnValueOnce(first.promise)
      .mockResolvedValueOnce(okResponse({ gpu: { name: 'RTX 4090' } }))

    const { exposed, wrapper } = mountComposable(() => useSystemStats(1000))

    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(1)

    first.resolve(okResponse({ gpu: { name: 'RTX 4090' } }))
    await flushPromises()
    await vi.advanceTimersByTimeAsync(1000)

    expect(fetch).toHaveBeenCalledTimes(2)
    expect(exposed.gpu.value).toEqual({ name: 'RTX 4090' })

    wrapper.unmount()
  })

  it('backs off system stats polling after transient failures', async () => {
    fetch
      .mockRejectedValueOnce(new TypeError('network down'))
      .mockResolvedValueOnce(okResponse({ gpu: { name: 'RTX 4090' } }))

    const { wrapper } = mountComposable(() => useSystemStats(1000))
    await flushPromises()

    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(999)
    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1001)
    expect(fetch).toHaveBeenCalledTimes(2)

    wrapper.unmount()
  })

  it('deduplicates stream stats fallback polls while a previous request is still in flight', async () => {
    const eventSources = installMockEventSource()
    const first = deferred()
    fetch
      .mockReturnValueOnce(first.promise)
      .mockResolvedValueOnce(okResponse({ streaming: true, fps: 120 }))

    const { exposed, wrapper } = mountComposable(() => useStreamStats(1000))
    eventSources[0].onerror()

    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1000)
    expect(fetch).toHaveBeenCalledTimes(1)

    first.resolve(okResponse({ streaming: true, fps: 120 }))
    await flushPromises()
    await vi.advanceTimersByTimeAsync(1000)

    expect(fetch).toHaveBeenCalledTimes(2)
    expect(exposed.stats.value).toEqual({ streaming: true, fps: 120 })

    wrapper.unmount()
  })

  it('backs off stream stats fallback polling after transient failures', async () => {
    const eventSources = installMockEventSource()
    fetch
      .mockRejectedValueOnce(new TypeError('listener restarting'))
      .mockResolvedValueOnce(okResponse({ streaming: true, fps: 120 }))

    const { wrapper } = mountComposable(() => useStreamStats(1000))
    eventSources[0].onerror()
    await flushPromises()

    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(999)
    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1001)
    expect(fetch).toHaveBeenCalledTimes(2)

    wrapper.unmount()
  })
})
