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

describe('useSystemStats game mode host', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn())
  })

  afterEach(() => {
    vi.unstubAllGlobals()
    vi.useRealTimers()
  })

  it('exposes game_mode_host from the stats payload so the console can show a running Game Mode session', async () => {
    fetch.mockResolvedValueOnce(okResponse({
      game_mode_host: { installed: true, session_active: true, evidence: ['gamescope-session-plus running as this account (pid 500)'] },
      display_session: { status: 'game_mode_session' },
    }))

    const { exposed, wrapper } = mountComposable(() => useSystemStats(1000))
    await flushPromises()

    expect(exposed.gameModeHost.value).toEqual({
      installed: true,
      session_active: true,
      evidence: ['gamescope-session-plus running as this account (pid 500)'],
    })
    expect(exposed.displaySession.value.status).toBe('game_mode_session')

    wrapper.unmount()
  })

  it('clears game_mode_host when a host stops reporting it', async () => {
    fetch
      .mockResolvedValueOnce(okResponse({ game_mode_host: { installed: true, session_active: false, evidence: [] } }))
      .mockResolvedValueOnce(okResponse({ gpu: null }))

    const { exposed, wrapper } = mountComposable(() => useSystemStats(1000))
    await flushPromises()
    expect(exposed.gameModeHost.value.installed).toBe(true)

    await vi.advanceTimersByTimeAsync(1000)
    await flushPromises()
    expect(exposed.gameModeHost.value).toBeNull()

    wrapper.unmount()
  })
})
