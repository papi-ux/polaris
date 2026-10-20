import { mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import TailscaleConnectionReadout from './components/TailscaleConnectionReadout.vue'

const t = (key) => key.split('.').at(-1)
const stats = (id = 'process.stream.42') => ({ streaming: true, stream_instance_id: id })
const answer = (id = 'process.stream.42') => ({
  schema_version: 1, status: 'tailscale', stream_instance_id: id,
  tailscale: { confirmed: true, transport: 'direct', scope: 'peer_status',
    source: 'tailscaled_localapi', observed_at_ms: Date.now(), max_age_ms: 10000, reason: 'observed' },
})
const response = (body) => ({ ok: true, json: async () => body })
let wrapper
function render(value) {
  wrapper = mount(TailscaleConnectionReadout, {
    props: { stats: value }, global: { provide: { i18n: { t } }, mocks: { $t: t } },
  })
  return wrapper
}
const status = () => wrapper.get('[data-tailscale-peer-status]').text()
beforeEach(() => {
  vi.useFakeTimers()
  vi.setSystemTime(new Date('2026-10-08T08:00:00Z'))
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = null
  vi.unstubAllGlobals()
  vi.useRealTimers()
})

describe('mounted Doctor Tailscale evidence', () => {
  it('does not query an idle stream and fetches authenticated evidence when one starts', async () => {
    const fetch = vi.fn().mockResolvedValue(response(answer()))
    vi.stubGlobal('fetch', fetch)
    render({ streaming: false })
    await vi.advanceTimersByTimeAsync(0)
    expect(fetch).not.toHaveBeenCalled()
    expect(status()).toBe('tailscale_peer_unverified')
    await wrapper.setProps({ stats: stats() })
    await vi.advanceTimersByTimeAsync(0)
    expect(fetch).toHaveBeenCalledWith('./api/network/connection', expect.objectContaining({ credentials: 'include', cache: 'no-store' }))
    expect(status()).toBe('tailscale_peer_direct')
  })

  it('expires displayed evidence when refresh cannot complete, and aborts bounded requests', async () => {
    let signal
    const fetch = vi.fn().mockResolvedValueOnce(response(answer())).mockImplementation((_url, options) => {
      signal = options.signal
      return new Promise((_resolve, reject) => signal.addEventListener('abort', () => reject(new Error('aborted'))))
    })
    vi.stubGlobal('fetch', fetch)
    render(stats())
    await vi.advanceTimersByTimeAsync(0)
    expect(status()).toBe('tailscale_peer_direct')
    await vi.advanceTimersByTimeAsync(8000)
    expect(signal.aborted).toBe(true)
    expect(status()).toBe('tailscale_peer_unverified')
  })

  it('discards an old stream response that arrives after the visible stream changes', async () => {
    let finish
    vi.stubGlobal('fetch', vi.fn().mockImplementation(() => new Promise((resolve) => { finish = resolve })))
    render(stats())
    await wrapper.setProps({ stats: stats('process.stream.43') })
    finish(response(answer('process.stream.42')))
    await vi.advanceTimersByTimeAsync(0)
    expect(status()).toBe('tailscale_peer_unverified')
    await wrapper.setProps({ stats: { streaming: false } })
    expect(status()).toBe('tailscale_peer_unverified')
  })

  it('abandons the previous stream request and asks again at once for the new stream', async () => {
    const calls = []
    vi.stubGlobal('fetch', vi.fn().mockImplementation((_url, options) => new Promise((resolve, reject) => {
      calls.push({ signal: options.signal, resolve })
      options.signal.addEventListener('abort', () => reject(new Error('aborted')))
    })))
    render(stats())
    await vi.advanceTimersByTimeAsync(0)
    expect(calls).toHaveLength(1)
    await wrapper.setProps({ stats: stats('process.stream.43') })
    await vi.advanceTimersByTimeAsync(0)
    // No five-second wait for the next poll: the old request is aborted and a new one is out.
    expect(calls[0].signal.aborted).toBe(true)
    expect(calls).toHaveLength(2)
    calls[1].resolve(response(answer('process.stream.43')))
    await vi.advanceTimersByTimeAsync(0)
    expect(status()).toBe('tailscale_peer_direct')
    // The abandoned request's late failure does not erase the new stream's answer.
    await vi.advanceTimersByTimeAsync(0)
    expect(status()).toBe('tailscale_peer_direct')
  })

  it('cancels the in-flight request and polling when the Doctor card leaves the page', async () => {
    let signal
    const fetch = vi.fn().mockImplementation((_url, options) => {
      signal = options.signal
      return new Promise((_resolve, reject) => signal.addEventListener('abort', () => reject(new Error('aborted'))))
    })
    vi.stubGlobal('fetch', fetch)
    render(stats())
    wrapper.unmount()
    wrapper = null
    await vi.advanceTimersByTimeAsync(20000)
    expect(signal.aborted).toBe(true)
    expect(fetch).toHaveBeenCalledTimes(1)
    expect(vi.getTimerCount()).toBe(0)
  })
})
