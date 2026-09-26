import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpacesDiagnostics from './SpacesDiagnostics.vue'
import { spacesGlobal } from './spaces-test-i18n.js'

const setup = () => ({
  version: 2, distribution: 'ubuntu', immutable_host: false, service_uid: 1000,
  host_prerequisites_ready: true, configured: true, available: true,
  checks: [
    ...['docker', 'docker_access', 'identity', 'input', 'gpu', 'security'].map(id => ({ id, title: id, detail: `Reported ${id}`, action: '', state: 'ready' })),
    { id: 'runtime', title: 'Runtime', detail: 'Image ready', action: '', state: 'ready',
      runtime: { status: 'ready', code: 'runtime_ready', id: 'steam-default', variant: 'default', nvidia_driver: '' } },
    { id: 'spaces', title: 'Spaces', detail: 'Controller available', action: '', state: 'ready' },
  ],
})
const state = () => ({ enabled: true, available: true, changing: false, failed: false, profiles: [], activity: [] })
const reply = body => ({ ok: true, json: async () => body })
const healthy = url => Promise.resolve(reply(url.endsWith('/setup') ? setup() : state()))
let wrapper
const start = () => {
  wrapper = mount(SpacesDiagnostics, { global: { ...spacesGlobal,
    stubs: { 'router-link': { props: ['to'], template: '<a :href="to"><slot /></a>' } },
  } })
  return wrapper
}
async function open(value = true) {
  wrapper.element.open = value
  await wrapper.trigger('toggle')
  await flushPromises()
}
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.restoreAllMocks(); vi.useRealTimers() })

describe('Spaces Doctor section', () => {
  it('does not inspect containers until opened, and sends only the existing read requests', async () => {
    vi.stubGlobal('fetch', vi.fn(healthy))
    start()
    expect(fetch).not.toHaveBeenCalled()
    expect(wrapper.text()).toContain('Not checked')
    await open()
    expect(fetch.mock.calls.map(([url]) => url)).toEqual(['./api/spaces/setup', './api/multiseat/profiles'])
    for (const [, options] of fetch.mock.calls) {
      expect(options.method).toBeUndefined()
      expect(options.credentials).toBe('include')
      expect(options.cache).toBe('no-store')
    }
    expect(wrapper.text()).toContain('Checks passed')
    expect(wrapper.findAll('[data-spaces-doctor-group]')).toHaveLength(3)
    expect(wrapper.get('[data-spaces-doctor-open]').attributes('href')).toBe('/spaces')
    expect(wrapper.findAll('button')).toHaveLength(1)
  })
  it('a failed refresh removes old green results while retaining verified partial evidence', async () => {
    vi.stubGlobal('fetch', vi.fn(healthy))
    start(); await open()
    fetch.mockImplementation(url => url.endsWith('/setup') ? Promise.reject(new Error('offline')) : healthy(url))
    await wrapper.get('[data-spaces-doctor-refresh]').trigger('click'); await flushPromises()
    expect(wrapper.get('summary').text()).toContain('Could not verify')
    expect(wrapper.find('[data-spaces-finding="docker"]').exists()).toBe(false)
    expect(wrapper.get('[data-spaces-finding="setup-unavailable"]').text()).toContain('could not be verified')
    expect(wrapper.get('[data-spaces-finding="controller"]').text()).toContain('controller is available')
  })
  it('invalid successful responses are unverified', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(); await open()
    expect(wrapper.get('summary').text()).toContain('Could not verify')
    expect(wrapper.findAll('[data-spaces-finding]')).toHaveLength(2)
  })
  it('closing cancels requests and a late completion cannot overwrite a fresh reopening', async () => {
    const pending = []
    vi.stubGlobal('fetch', vi.fn((url, options) => new Promise(resolve => pending.push({ url, options, resolve }))))
    start(); await open()
    await open(false)
    expect(pending.every(item => item.options.signal.aborted)).toBe(true)
    fetch.mockImplementation(healthy)
    await open()
    expect(wrapper.get('summary').text()).toContain('Checks passed')
    pending.forEach(item => item.resolve(reply({ status: false })))
    await flushPromises()
    expect(wrapper.get('summary').text()).toContain('Checks passed')
  })
  it('unmount aborts both outstanding reads and does not start another request', async () => {
    const pending = []
    vi.stubGlobal('fetch', vi.fn((url, options) => new Promise(resolve => pending.push({ url, options, resolve }))))
    start(); await open()
    wrapper.unmount(); wrapper = null
    expect(pending.every(item => item.options.signal.aborted)).toBe(true)
    pending.forEach(item => item.resolve(reply(item.url.endsWith('/setup') ? setup() : state())))
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(2)
  })
  it('timeout aborts inspection and reports missing evidence', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn((_url, options) => new Promise((_resolve, reject) => {
      options.signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true })
    })))
    start(); await open()
    await vi.advanceTimersByTimeAsync(12000); await flushPromises()
    expect(wrapper.get('summary').text()).toContain('Could not verify')
    expect(wrapper.get('[data-spaces-doctor-refresh]').attributes('disabled')).toBeUndefined()
  })
  it('reopening completed checks requests a new snapshot', async () => {
    vi.stubGlobal('fetch', vi.fn(healthy))
    start(); await open(); await open(false); await open()
    expect(fetch).toHaveBeenCalledTimes(4)
  })
})
