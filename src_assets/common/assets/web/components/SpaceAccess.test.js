import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, expect, it, vi } from 'vitest'
import SpaceAccess from './SpaceAccess.vue'
import { validSnapshot } from '../spaces-access.js'
let wrapper
const space = { id: 'a', name: 'Alex', clients: ['default'], access_clients: [] }
const devices = [{ uuid: 'default', name: 'TV', perm: 0x04000000 }, { uuid: 'rp6', friendly_name: 'Retroid', perm: 0x04000000 },
  { uuid: 'guest', name: 'Guest', perm: 0x04000000, temporary_authorization: true }]
function start(refresh = async () => true) { wrapper = mount(SpaceAccess, { props: { space, clients: devices, ready: true, refresh } }) }
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })
it('shows eligible devices and keeps default access locked', () => {
  start(); expect(wrapper.findAll('input')).toHaveLength(2)
  expect(wrapper.findAll('input')[0].element.disabled).toBe(true)
  expect(wrapper.text()).toContain('Default Space'); expect(wrapper.text()).not.toContain('Guest')
})
it('changes only the requested grant and confirms server readback', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, status: 200, json: async () => ({ status: true }) })))
  start(async () => { await wrapper.setProps({ space: { ...space, access_clients: ['rp6'] } }); return true })
  await wrapper.findAll('input')[1].setValue(true); await flushPromises()
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ profile_id: 'a', client_id: 'rp6', allowed: true })
  expect(wrapper.text()).toContain('Device Access Saved.')
})
it('keeps old permission visible when the host refuses during a stream', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 409, json: async () => ({ status: false, message: 'Stop Space streams first' }) })))
  start(); await wrapper.findAll('input')[1].setValue(true); await flushPromises()
  expect(wrapper.findAll('input')[1].element.checked).toBe(false)
  expect(wrapper.text()).toContain('Stop Space streams first'); expect(wrapper.text()).not.toContain('Access Saved')
})
it('allows shared access across Spaces but rejects duplicate or archived grants', () => {
  const snapshot = { enabled: true, available: true, changing: false, failed: false, profiles: [space, { ...space, id: 'b', clients: [], access_clients: ['default'] }] }
  expect(validSnapshot(snapshot)).toBe(true)
  expect(validSnapshot({ ...snapshot, profiles: [{ ...space, access_clients: ['rp6', 'rp6'] }] })).toBe(false)
  expect(validSnapshot({ ...snapshot, profiles: [{ ...space, archived: true, clients: [], access_clients: ['rp6'] }] })).toBe(false)
})
