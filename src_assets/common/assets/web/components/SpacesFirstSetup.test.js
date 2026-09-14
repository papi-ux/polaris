import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import SpacesFirstSetup from './SpacesFirstSetup.vue'
import { validJobSnapshot } from '../spaces-job.js'

const id = '12345678-1234-1234-1234-123456789abc'
const runtime = { id: 'steam-test', variant: 'default', nvidia_driver: '' }
const state = job => ({ version: 1, available: true, runtimes: [runtime], job: job || null, message: '' })
const job = (phase = 'downloading') => ({
  request_id: id, runtime_id: runtime.id, name: 'Living room', state: phase,
  message: phase === 'prepared' ? 'Your Steam home is prepared. Configuration still needs attention.' : 'Preparing setup',
  can_cancel: phase === 'downloading', can_retry: phase === 'cancelled' || phase === 'failed',
})
const reply = (body, status = 200) => ({ status, ok: status >= 200 && status < 300, json: async () => body })
let wrapper
beforeEach(() => { sessionStorage.clear(); vi.stubGlobal('crypto', { randomUUID: () => id }) })
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.useRealTimers() })
const button = label => wrapper.findAll('button').find(item => item.text() === label)

describe('first-space preparation', () => {
  it('shows unpublished runtimes honestly without offering a fake download', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply({ ...state(), available: false, runtimes: [], message: 'The runtime is not published yet.' })))
    wrapper = mount(SpacesFirstSetup, { props: { hostReady: true } })
    await flushPromises()
    expect(wrapper.text()).toContain('not published')
    expect(wrapper.find('form').exists()).toBe(false)
    expect(fetch.mock.calls[0][1].method).toBeUndefined()
  })

  it('reconnects to the host job after navigation and sends no duplicate request', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply(state(job()))))
    wrapper = mount(SpacesFirstSetup, { props: { hostReady: true } })
    await flushPromises()
    expect(wrapper.text()).toContain('Living room')
    expect(wrapper.find('form').exists()).toBe(false)
    wrapper.unmount()
    wrapper = mount(SpacesFirstSetup, { props: { hostReady: true } })
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(2)
    expect(fetch.mock.calls.every(([, options]) => options.method === undefined)).toBe(true)
  })

  it('retains the same request after an uncertain response and reload', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(state()))
      .mockRejectedValueOnce(new Error('Offline'))
      .mockResolvedValueOnce(reply(state()))
      .mockResolvedValueOnce(reply({ ...state(job()), accepted: true }, 202)))
    wrapper = mount(SpacesFirstSetup, { props: { hostReady: true } })
    await flushPromises()
    await wrapper.get('input').setValue('Living room')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    const original = JSON.parse(fetch.mock.calls[1][1].body)
    expect(original).toEqual({ operation: 'start', request_id: id, runtime_id: runtime.id, name: 'Living room' })
    wrapper.unmount()
    wrapper = mount(SpacesFirstSetup, { props: { hostReady: true } })
    await flushPromises()
    await button('Retry saved request').trigger('click')
    await flushPromises()
    expect(JSON.parse(fetch.mock.calls[3][1].body)).toEqual(original)
    expect(sessionStorage.getItem('polaris.spaces.first-setup')).toBeNull()
  })

  it('fences cancellation by request identity and offers no cancel during the home commit', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(state(job())))
      .mockResolvedValueOnce(reply({ ...state(job('preparing')), accepted: false }, 409)))
    wrapper = mount(SpacesFirstSetup, { props: { hostReady: true } })
    await flushPromises()
    await button('Stop setup').trigger('click')
    await flushPromises()
    expect(JSON.parse(fetch.mock.calls[1][1].body)).toEqual({ operation: 'cancel', request_id: id })
    expect(wrapper.text()).toContain('not accepted')
    expect(button('Stop setup')).toBeUndefined()
  })

  it('disables mutation on a malformed refresh rather than trusting stale progress', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(state(job())))
      .mockResolvedValueOnce(reply({ ...state(job()), version: 2 })))
    wrapper = mount(SpacesFirstSetup, { props: { hostReady: true } })
    await flushPromises()
    await button('Reconnect to setup').trigger('click')
    await flushPromises()
    expect(button('Stop setup').attributes('disabled')).toBeDefined()
    expect(wrapper.text()).toContain('could not be verified')
  })

  it('requires host prerequisites and does not present prepared storage as playable', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(state()))
      .mockResolvedValueOnce(reply(state(job('prepared')))))
    wrapper = mount(SpacesFirstSetup)
    await flushPromises()
    expect(button('Download and prepare').attributes('disabled')).toBeDefined()
    await button('Reconnect to setup').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('has not started a game or enabled streaming')
    expect(button('Stop setup')).toBeUndefined()
  })

  it('rejects ambiguous status and runtime identities', () => {
    expect(validJobSnapshot(state(job()))).toBe(true)
    for (const bad of [
      { ...state(), runtimes: [runtime, runtime] },
      { ...state(), runtimes: [] },
      state({ ...job('prepared'), can_cancel: true }),
      state({ ...job(), can_retry: true }),
      state({ ...job(), request_id: 'other' }),
      { ...state(), runtimes: [{ ...runtime, id: '../image' }] },
    ]) expect(validJobSnapshot(bad)).toBe(false)
  })
})
