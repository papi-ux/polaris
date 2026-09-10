import { mount, flushPromises } from '@vue/test-utils'
import { describe, it, vi, expect, afterEach } from 'vitest'
import fixtures from '../../../../../tests/fixtures/live-tuning-v1.json'
import LiveTuningControl from './LiveTuningControl.vue'

let wrappers = []
afterEach(() => { wrappers.forEach(w => w.unmount()); wrappers = []; vi.unstubAllGlobals() })
describe('Live Tuning switch', () => {
  it('shares one stream connection and confirms both switches only after save', async () => {
    const sources = []
    vi.stubGlobal('EventSource', class { constructor() { sources.push(this) } close() {} })
    let resolveSave
    vi.stubGlobal('fetch', vi.fn(() => new Promise(resolve => { resolveSave = resolve })))
    const a = mount(LiveTuningControl), b = mount(LiveTuningControl)
    wrappers.push(a, b)
    expect(sources).toHaveLength(1)
    const off = { ...fixtures[0].live_tuning, host_instance: 'switch-test-host', sequence: 1 }
    sources[0].onmessage({ data: JSON.stringify({ live_tuning: off }) })
    await flushPromises()
    const button = a.get('[role=switch]')
    expect(button.attributes('aria-checked')).toBe('false')
    await button.trigger('click')
    expect(fetch).toHaveBeenCalledTimes(1)
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ enabled: true })
    expect(fetch.mock.calls[0][1].headers['If-Match']).toBe(`"${off.configuration_revision}"`)
    expect(button.attributes('aria-checked')).toBe('false')
    expect(b.text()).toContain('Saving')
    resolveSave({ ok: true, json: async () => ({ status: true, live_tuning: { ...off, enabled: true, state: 'stable', sequence: 2 } }) })
    await flushPromises()
    expect(button.attributes('aria-checked')).toBe('true')
    expect(b.get('[role=switch]').attributes('aria-checked')).toBe('true')
  })
  it('keeps the confirmed switch on failure and stops an unused connection', async () => {
    const sources = []
    const close = vi.fn()
    vi.stubGlobal('EventSource', class { constructor() { sources.push(this) } close = close })
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 412, json: async () => ({ status: false }) })))
    const a = mount(LiveTuningControl); wrappers.push(a)
    sources[0].onmessage({ data: JSON.stringify({ live_tuning: { ...fixtures[0].live_tuning, host_instance: 'failure-test-host', sequence: 1 } }) })
    await flushPromises()
    await a.get('[role=switch]').trigger('click')
    await flushPromises()
    expect(a.get('[role=switch]').attributes('aria-checked')).toBe('false')
    expect(a.get('[role=alert]').text()).toContain('Settings changed')
    expect(fetch).toHaveBeenCalledTimes(1)
    a.unmount(); wrappers = []
    expect(close).toHaveBeenCalledTimes(1)
  })
  it('marks a missing or invalid envelope unknown after valid status', async () => {
    const sources = []
    vi.stubGlobal('EventSource', class { constructor() { sources.push(this) } close() {} })
    const a = mount(LiveTuningControl); wrappers.push(a)
    const live = { ...fixtures[0].live_tuning, host_instance: 'invalid-test', sequence: 1 }
    const emit = async payload => { sources[0].onmessage({ data: JSON.stringify(payload) }); await flushPromises() }
    await emit({ live_tuning: live })
    expect(a.get('[role=switch]').attributes('disabled')).toBeUndefined()
    await emit({ streaming: true })
    expect(a.text()).toContain('Unknown')
    expect(a.get('[role=switch]').attributes('disabled')).toBeDefined()
    await emit({ live_tuning: { ...live, version: 99, sequence: 2 } })
    expect(a.text()).toContain('Unknown')
    await emit({ live_tuning: { ...live, sequence: 3 } })
    expect(a.get('[role=switch]').attributes('disabled')).toBeUndefined()
  })
  it('does not apply or announce an old save after the host restarts', async () => {
    const sources = []
    vi.stubGlobal('EventSource', class { constructor() { sources.push(this) } close() {} })
    let resolveSave
    vi.stubGlobal('fetch', vi.fn(() => new Promise(resolve => { resolveSave = resolve })))
    const announced = vi.fn()
    window.addEventListener('polaris:live-tuning-saved', announced)
    const a = mount(LiveTuningControl); wrappers.push(a)
    const old = { ...fixtures[0].live_tuning, host_instance: 'old-save-host', sequence: 1 }
    sources[0].onmessage({ data: JSON.stringify({ live_tuning: old }) }); await flushPromises()
    await a.get('[role=switch]').trigger('click')
    sources[0].onmessage({ data: JSON.stringify({ live_tuning: { ...old, host_instance: 'new-save-host' } }) }); await flushPromises()
    resolveSave({ ok: true, json: async () => ({ status: true, live_tuning: { ...old, enabled: true, sequence: 100 } }) })
    await flushPromises()
    expect(a.get('[role=switch]').attributes('aria-checked')).toBe('false')
    expect(announced).not.toHaveBeenCalled()
    window.removeEventListener('polaris:live-tuning-saved', announced)
  })

})
