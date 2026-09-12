import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import MultiseatAssignments from './MultiseatAssignments.vue'

const client = { uuid: 'device-a', name: 'Living room', perm: 0x07001F00, temporary_authorization: false }
const snapshot = () => ({ enabled: true, available: true, changing: false, failed: false,
  profiles: [{ id: 'profile-a', name: 'Alex', clients: [] }] })
const reply = (body, ok = true, status = 200) => ({ ok, status, json: async () => body })
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })

describe('profile assignments', () => {
  it('keeps ordinary device setup unchanged when multiseat is off', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ ...snapshot(), enabled: false })))
    wrapper = mount(MultiseatAssignments, { props: { clients: [client] } })
    await flushPromises()
    expect(wrapper.find('section').exists()).toBe(false)
    expect(fetch).toHaveBeenCalledTimes(1)
  })

  it('waits for persistence and read-back before showing a saved assignment', async () => {
    let finish
    const current = snapshot()
    vi.stubGlobal('fetch', vi.fn(async (_, options) => options.method === 'POST'
      ? new Promise(resolve => { finish = resolve }) : reply(current)))
    wrapper = mount(MultiseatAssignments, { props: { clients: [client] } })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    expect(wrapper.get('select').attributes('disabled')).toBeDefined()
    expect(wrapper.text()).not.toContain('Assignment saved.')
    expect(JSON.parse(fetch.mock.calls[1][1].body)).toEqual({ client_id: 'device-a', profile_id: 'profile-a' })
    current.profiles[0].clients = ['device-a']
    finish(reply({ status: true }))
    await flushPromises()
    expect(wrapper.get('select').element.value).toBe('profile-a')
    expect(wrapper.text()).toContain('Assignment saved.')
    expect(wrapper.get('button').attributes('disabled')).toBeDefined()
  })

  it('retains the confirmed assignment when an active seat rejects a change', async () => {
    vi.stubGlobal('fetch', vi.fn(async (_, options) => options.method === 'POST'
      ? reply({ status: false, message: 'Stop profile sessions first' }, false, 409) : reply(snapshot())))
    wrapper = mount(MultiseatAssignments, { props: { clients: [client] } })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Stop profile sessions first')
    expect(wrapper.get('select').element.value).toBe('')
    expect(wrapper.text()).not.toContain('Assignment saved.')
  })
})
