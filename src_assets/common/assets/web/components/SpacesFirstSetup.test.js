import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import SpacesFirstSetup from './SpacesFirstSetup.vue'
import { validJobSnapshot } from '../spaces-job.js'
import { spacesGlobal } from './spaces-test-i18n.js'

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
const start = (props = { hostReady: true }) => mount(SpacesFirstSetup, { attachTo: document.body, props, global: spacesGlobal })

describe('first-space preparation', () => {
  it('shows unpublished runtimes honestly without offering a fake download', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply({ ...state(), available: false, runtimes: [], message: 'The runtime is not published yet.' })))
    wrapper = start()
    await flushPromises()
    expect(wrapper.get('[data-setup-unavailable]').text()).toContain('not published')
    expect(wrapper.find('form').exists()).toBe(false)
    expect(fetch.mock.calls[0][1].method).toBeUndefined()
  })

  it('names the reason the host gives for an unavailable setup and links its section', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply({ ...state(), available: false, runtimes: [],
      message: 'The verified gaming runtime is not published for this preview yet.', unavailable_reason: 'runtime_not_published' })))
    wrapper = start()
    await flushPromises()
    const card = wrapper.get('[data-setup-unavailable]')
    expect(card.text()).toContain('Not available yet')
    expect(card.text()).toContain('nothing to download until it is')
    expect(card.get('a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#prepare-your-first-space')
  })

  it('reconnects to the host job after navigation and sends no duplicate request', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply(state(job()))))
    wrapper = start()
    await flushPromises()
    expect(wrapper.text()).toContain('Living room')
    expect(wrapper.text()).toContain('Downloading')
    expect(wrapper.find('form').exists()).toBe(false)
    wrapper.unmount()
    wrapper = start()
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(2)
    expect(fetch.mock.calls.every(([, options]) => options.method === undefined)).toBe(true)
  })

  it('polls a working job only while the tab is visible and backs off when the host fails', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply(state(job()))))
    wrapper = start()
    await vi.advanceTimersByTimeAsync(0)
    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(2000)
    expect(fetch).toHaveBeenCalledTimes(2)
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => true })
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(10000)
    expect(fetch).toHaveBeenCalledTimes(2)
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => false })
    fetch.mockRejectedValueOnce(new Error('Offline'))
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(0)
    expect(fetch).toHaveBeenCalledTimes(3)
    await vi.advanceTimersByTimeAsync(2000)
    expect(fetch).toHaveBeenCalledTimes(3)
    await vi.advanceTimersByTimeAsync(2000)
    expect(fetch).toHaveBeenCalledTimes(4)
  })

  it('retains the same request after an uncertain response and reload', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(state()))
      .mockRejectedValueOnce(new Error('Offline'))
      .mockResolvedValueOnce(reply(state()))
      .mockResolvedValueOnce(reply({ ...state(job()), accepted: true }, 202)))
    wrapper = start()
    await flushPromises()
    await wrapper.get('input').setValue('Living room')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    const original = JSON.parse(fetch.mock.calls[1][1].body)
    expect(original).toEqual({ operation: 'start', request_id: id, runtime_id: runtime.id, name: 'Living room' })
    wrapper.unmount()
    wrapper = start()
    await flushPromises()
    await button('Retry saved request').trigger('click')
    await flushPromises()
    expect(JSON.parse(fetch.mock.calls[3][1].body)).toEqual(original)
    expect(sessionStorage.getItem('polaris.spaces.first-setup')).toBeNull()
  })

  it('fences cancellation by request identity and offers no cancel during the home commit', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(state(job())))
      .mockResolvedValueOnce(reply({ ...state(job('preparing')), accepted: false }, 409)))
    wrapper = start()
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
    wrapper = start()
    await flushPromises()
    await button('Reconnect to setup').trigger('click')
    await flushPromises()
    expect(button('Stop setup').attributes('disabled')).toBeDefined()
    expect(wrapper.text()).toContain('could not be verified')
  })

  it('requires host prerequisites and does not present prepared storage as playable', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply(state()))
      .mockResolvedValueOnce(reply(state(job('prepared')))))
    wrapper = start({})
    await flushPromises()
    expect(button('Download and prepare').attributes('disabled')).toBeDefined()
    await button('Reconnect to setup').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('has not started a game or enabled streaming')
    expect(button('Stop setup')).toBeUndefined()
  })

  it('names what blocks a job and what was kept for recovery', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply({ ...state({ ...job('recovery_required'), can_cancel: false, can_retry: false,
      blocked_by: ['journal_fault'], recovery: { reference: 'ghcr.io/papi-ux/polaris-worker-steam@sha256:' + 'a'.repeat(64),
        image: 'sha256:' + 'b'.repeat(64), code: 'preparing', doc_anchor: '#recover-an-interrupted-setup' } }), available: false,
      message: 'Saved setup state could not be secured.', unavailable_reason: 'journal_fault' })))
    wrapper = start()
    await flushPromises()
    expect(wrapper.text()).toContain('Recovery required')
    expect(wrapper.get('[data-setup-blocked]').text()).toContain('setup journal could not be secured')
    const recovery = wrapper.get('[data-setup-recovery]')
    expect(recovery.text()).toContain('sha256:' + 'b'.repeat(64))
    expect(recovery.get('a').attributes('href')).toBe('https://papi-ux.com/docs/spaces/#recover-an-interrupted-setup')
    expect(button('Retry setup')).toBeUndefined()
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
      { ...state(), unavailable_reason: 'not a word' },
      state({ ...job(), blocked_by: 'journal_fault' }),
      state({ ...job(), recovery: { doc_anchor: 'javascript:1' } }),
    ]) expect(validJobSnapshot(bad)).toBe(false)
  })
})


describe('first-space activation', () => {
  const prepared = () => ({ ...state({ ...job('prepared'), can_activate: true, gpu_id: '' }),
    graphics: [{ id: 'pci-0000_01_00.0', label: 'NVIDIA graphics' }] })
  it('sends only the saved request and discovered graphics selection, then confirms the restart and waits for the host', async () => {
    const configured = { ...prepared(), job: { ...job('restart_required'), can_activate: false, gpu_id: 'pci-0000_01_00.0' } }
    vi.stubGlobal('fetch', vi.fn(async (url, options) => {
      if (url === './api/restart') return reply({ restarting: true })
      if (url === './api/config') return reply({ status: true })
      return options?.method === 'POST' ? reply({ ...configured, accepted: true }, 202) : reply(prepared())
    }))
    wrapper = start()
    await flushPromises()
    await button('Enable Spaces').trigger('click')
    await flushPromises()
    expect(JSON.parse(fetch.mock.calls[1][1].body)).toEqual({ operation: 'activate', request_id: id, gpu_id: 'pci-0000_01_00.0' })
    expect(fetch).toHaveBeenCalledTimes(2)
    expect(wrapper.text()).toContain('disconnects active streams')
    await button('Restart Polaris and finish setup').trigger('click')
    await flushPromises()
    const dialog = document.body.querySelector('[role="dialog"]')
    expect(dialog.textContent).toContain('Every stream disconnects')
    expect(fetch.mock.calls.filter(([url]) => url === './api/restart')).toHaveLength(0)
    vi.useFakeTimers()
    dialog.querySelector('[data-confirm-confirm]').click()
    await vi.advanceTimersByTimeAsync(1500)
    expect(fetch.mock.calls.filter(([url]) => url === './api/restart')).toHaveLength(1)
    expect(fetch.mock.calls.find(([url]) => url === './api/restart')[1].body).toBeUndefined()
    expect(wrapper.get('[role=status]').text()).toContain('Polaris is back')
    expect(wrapper.find('[role=alert]').exists()).toBe(false)
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(button('Restart Polaris and finish setup').attributes('disabled')).toBeDefined()
  })
  it('cannot enable without a matching GPU or after an unverified response', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(reply({ ...prepared(), graphics: [] }))
      .mockResolvedValueOnce(reply(prepared()))
      .mockRejectedValueOnce(new Error('Offline')))
    wrapper = start()
    await flushPromises()
    expect(button('Enable Spaces').attributes('disabled')).toBeDefined()
    expect(wrapper.text()).toContain('No accessible graphics card')
    await button('Reconnect to setup').trigger('click'); await flushPromises()
    await button('Enable Spaces').trigger('click'); await flushPromises()
    expect(button('Enable Spaces').attributes('disabled')).toBeDefined()
    expect(fetch.mock.calls.filter(([url]) => url === './api/restart')).toHaveLength(0)
  })
  it('rejects activation authority in contradictory or malformed snapshots', () => {
    expect(validJobSnapshot(prepared())).toBe(true)
    expect(validJobSnapshot({ ...prepared(), graphics: [{ id: '/dev/dri/renderD128', label: 'GPU' }] })).toBe(false)
    expect(validJobSnapshot({ ...prepared(), job: { ...job('downloading'), can_activate: true } })).toBe(false)
    expect(validJobSnapshot({ ...prepared(), job: { ...job('restart_required'), gpu_id: '' } })).toBe(false)
  })
})
