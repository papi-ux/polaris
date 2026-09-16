import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpacesView from './SpacesView.vue'
import MultiseatAssignments from '../components/MultiseatAssignments.vue'
import SpacesSetup from '../components/SpacesSetup.vue'
import { spacesGlobal } from '../components/spaces-test-i18n.js'

const clients = { status: true, platform: 'linux', named_certs: [{ uuid: 'device-a', name: 'Living room', perm: 0x07001F00 }] }
const reply = (body, ok = true) => ({ ok, status: ok ? 200 : 503, json: async () => body })
const stubs = {
  MultiseatAssignments: { name: 'MultiseatAssignments', props: ['clients', 'clientsReady'], emits: ['snapshot'], template: '<div data-assignments>{{ clients.length }}</div>' },
  SpacesSetup: { name: 'SpacesSetup', emits: ['state'], template: '<div data-setup></div>' },
  'router-link': true,
}
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })
const start = () => mount(SpacesView, { global: { ...spacesGlobal, stubs } })

describe('the Spaces page', () => {
  it('shows the first-Space card only after a snapshot with no live Space has loaded', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(clients)))
    wrapper = start()
    expect(wrapper.text()).not.toContain('Create your first Space')
    await flushPromises()
    expect(wrapper.text()).not.toContain('Create your first Space')
    wrapper.findComponent(MultiseatAssignments).vm.$emit('snapshot', { enabled: true, profiles: [{ id: 'a', name: 'Alex', archived: true, clients: [] }] })
    await flushPromises()
    expect(wrapper.text()).toContain('Create your first Space')
    wrapper.findComponent(MultiseatAssignments).vm.$emit('snapshot', { enabled: true, profiles: [{ id: 'a', name: 'Alex', clients: [] }] })
    await flushPromises()
    expect(wrapper.text()).not.toContain('Create your first Space')
  })

  it('leads with Host Setup until the host can offer Spaces', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(clients)))
    wrapper = start()
    await flushPromises()
    expect(wrapper.get('[data-setup]').classes()).toContain('order-1')
    expect(wrapper.get('[data-assignments]').classes()).toContain('order-2')
    wrapper.findComponent(SpacesSetup).vm.$emit('state', { available: true, configured: true, hostReady: true })
    await flushPromises()
    expect(wrapper.get('[data-setup]').classes()).toContain('order-2')
    expect(wrapper.get('[data-assignments]').classes()).toContain('order-1')
  })

  it('offers an in-page retry when the paired devices cannot be read', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValueOnce(new Error('Offline')).mockResolvedValueOnce(reply(clients)))
    wrapper = start()
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Could not load paired devices')
    expect(wrapper.get('[data-assignments]').text()).toBe('0')
    await wrapper.get('[data-clients-retry]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[role=alert]').exists()).toBe(false)
    expect(wrapper.get('[data-assignments]').text()).toBe('1')
    expect(fetch).toHaveBeenCalledTimes(2)
  })

  it('links the guide on papi-ux.com and marks the preview', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(clients)))
    wrapper = start()
    await flushPromises()
    expect(wrapper.get('a[href="https://papi-ux.com/docs/spaces/"]').text()).toBe('Spaces guide')
    expect(wrapper.get('.meta-pill').text()).toBe('Preview')
    expect(wrapper.classes()).toContain('operator-console')
  })
})
