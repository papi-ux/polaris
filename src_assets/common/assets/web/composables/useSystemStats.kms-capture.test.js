import { mount } from '@vue/test-utils'
import { defineComponent } from 'vue'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { useSystemStats } from './useSystemStats.js'

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

describe('useSystemStats capture readiness', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn())
  })

  afterEach(() => {
    vi.unstubAllGlobals()
    vi.useRealTimers()
  })

  it('exposes kms_capture and the running binary, and clears both when the host stops sending them', async () => {
    const kms = { state: 'not_in_use', route_kind: 'other', capture: 'portal', route: 'portal', capability_set_aside: true }
    const runningBinary = { path: '/usr/libexec/polaris/polaris-kms', version: '1.4.13' }
    fetch
      .mockResolvedValueOnce(okResponse({ kms_capture: kms, running_binary: runningBinary }))
      .mockResolvedValueOnce(okResponse({ gpu: null }))

    const { exposed, wrapper } = mountComposable(() => useSystemStats(1000))
    await flushPromises()
    expect(exposed.kmsCapture.value).toEqual(kms)
    expect(exposed.runningBinary.value).toEqual(runningBinary)

    await vi.advanceTimersByTimeAsync(1000)
    await flushPromises()
    expect(exposed.kmsCapture.value).toBeNull()
    expect(exposed.runningBinary.value).toBeNull()

    wrapper.unmount()
  })

  it('retires readiness after a failed poll and does not invent audio or display checks', async () => {
    fetch.mockResolvedValueOnce(okResponse({ displays: [], audio: { sink: 'default' } }))
      .mockResolvedValueOnce({ ok: false, status: 503 })
      .mockResolvedValueOnce(okResponse({ gpu: null }))
    const { exposed, wrapper } = mountComposable(() => useSystemStats(1000))
    await flushPromises()
    expect(exposed.readiness.value).toEqual({ displays: 'ready', audio: 'ready' })
    await vi.advanceTimersByTimeAsync(1000)
    expect(exposed.readiness.value).toEqual({ displays: 'unavailable', audio: 'unavailable' })
    // The old sink can still be shown as historical data; it is no longer a passed check.
    expect(exposed.audio.value.sink).toBe('default')
    await vi.advanceTimersByTimeAsync(2000)
    expect(exposed.readiness.value).toEqual({ displays: 'unavailable', audio: 'unavailable' })
    wrapper.unmount()
  })
})
