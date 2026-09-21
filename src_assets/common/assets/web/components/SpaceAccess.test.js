import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, expect, it, vi } from 'vitest'
import SpaceAccess from './SpaceAccess.vue'
import { validSnapshot } from '../spaces-access.js'
import { spacesGlobal } from './spaces-test-i18n.js'
let wrapper
const space = { id: 'a', name: 'Alex', clients: ['default'], access_clients: [] }
const devices = [{ uuid: 'default', name: 'TV', perm: 0x04000000 }, { uuid: 'rp6', friendly_name: 'Retroid', perm: 0x04000000 },
  { uuid: 'guest', name: 'Guest', perm: 0x04000000, temporary_authorization: true }]
function start(refresh = async () => true, props = {}) {
  wrapper = mount(SpaceAccess, { global: spacesGlobal, props: { space, clients: devices, ready: true, refresh, ...props } })
}
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })
it('shows eligible devices, marks the Default Space with a way to change it, and lets it be unticked', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, status: 200, json: async () => ({ status: true }) })))
  start(async () => { await wrapper.setProps({ space: { ...space, clients: [] } }); return true })
  expect(wrapper.findAll('input')).toHaveLength(2)
  expect(wrapper.findAll('input')[0].element.checked).toBe(true)
  expect(wrapper.findAll('input')[0].element.disabled).toBe(false)
  expect(wrapper.text()).toContain('Default Space'); expect(wrapper.text()).not.toContain('Guest')
  expect(wrapper.get('.control-chip').text()).toBe('1')
  await wrapper.get('[data-default-change]').trigger('click')
  expect(wrapper.emitted('open-default')).toHaveLength(1)
  // Unticking is how a device leaves a Space; the host drops it as the Default Space too.
  await wrapper.findAll('input')[0].setValue(false); await flushPromises()
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ profile_id: 'a', client_id: 'default', allowed: false })
  expect(wrapper.text()).toContain('Device Access saved.')
})
it('says when no device can be given access', () => {
  start(async () => true, { clients: [devices[2]] })
  expect(wrapper.findAll('input')).toHaveLength(0)
  expect(wrapper.text()).toContain('Pair a device with permission to launch apps first.')
})
it('changes only the requested grant and confirms server readback', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, status: 200, json: async () => ({ status: true }) })))
  start(async () => { await wrapper.setProps({ space: { ...space, access_clients: ['rp6'] } }); return true })
  await wrapper.findAll('input')[1].setValue(true); await flushPromises()
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ profile_id: 'a', client_id: 'rp6', allowed: true })
  expect(wrapper.text()).toContain('Device Access saved.')
})
it('keeps old permission visible when the host refuses during a stream', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 409, json: async () => ({ status: false, message: 'Stop Space streams first' }) })))
  start(); await wrapper.findAll('input')[1].setValue(true); await flushPromises()
  expect(wrapper.findAll('input')[1].element.checked).toBe(false)
  expect(wrapper.text()).toContain('Stop Space streams first'); expect(wrapper.text()).not.toContain('Access saved')
})
it('shows the host reason when a bad request carries only an error field', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 400, json: async () => ({ status: false, error: 'Invalid Space or paired device.' }) })))
  start(); await wrapper.findAll('input')[1].setValue(true); await flushPromises()
  expect(wrapper.get('[role=alert]').text()).toContain('Invalid Space or paired device.')
})
it('names the stream lock on its checkboxes when the parent passes one', () => {
  start(async () => true, { locked: true, lockReasonId: 'spaces-stream-lock' })
  expect(wrapper.findAll('input')[1].attributes('aria-describedby')).toBe('spaces-stream-lock')
  expect(wrapper.findAll('input')[1].element.disabled).toBe(true)
})
it('allows shared access across Spaces but rejects duplicate or archived grants', () => {
  const snapshot = { enabled: true, available: true, changing: false, failed: false, profiles: [space, { ...space, id: 'b', clients: [], access_clients: ['default'] }] }
  expect(validSnapshot(snapshot)).toBe(true)
  expect(validSnapshot({ ...snapshot, profiles: [{ ...space, access_clients: ['rp6', 'rp6'] }] })).toBe(false)
  expect(validSnapshot({ ...snapshot, profiles: [{ ...space, archived: true, clients: [], access_clients: ['rp6'] }] })).toBe(false)
})

// Select all and clear all: one host change, where ticking thirteen devices was thirteen saves and
// thirteen restarts of Spaces.
const reply = (body, status = 200) => ({ ok: status < 300, status, json: async () => body })
const dialog = () => document.body.querySelector('[role="dialog"]')
function startAttached(refresh, props = {}) {
  wrapper = mount(SpaceAccess, { attachTo: document.body, global: spacesGlobal, props: { space, clients: devices, ready: true, refresh, ...props } })
}
it('lets every device in with one request, and says so only once the host shows it', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
  startAttached(async () => { await wrapper.setProps({ space: { ...space, access_clients: ['rp6'] } }); return true })
  await wrapper.get('[data-access-select-all]').trigger('click'); await flushPromises()
  expect(fetch).toHaveBeenCalledTimes(1)
  expect(fetch.mock.calls[0][0]).toBe('./api/multiseat/access/all')
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ profile_id: 'a', allowed: true })
  expect(wrapper.text()).toContain('All 2 devices can open Alex.')
  expect(wrapper.get('[data-access-select-all]').element.disabled).toBe(true)
})
it('asks before it removes every device, and says what else goes with them', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
  startAttached(async () => { await wrapper.setProps({ space: { ...space, clients: [], access_clients: [] } }); return true })
  await wrapper.get('[data-access-clear-all]').trigger('click'); await flushPromises()
  expect(fetch).not.toHaveBeenCalled()
  expect(dialog().textContent).toContain('Remove every device from Alex?')
  expect(dialog().textContent).toContain('A device that opens this Space first goes back to opening another place first.')
  dialog().querySelector('[data-confirm-confirm]').click(); await flushPromises()
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ profile_id: 'a', allowed: false })
  expect(wrapper.text()).toContain('No device can open Alex now.')
  expect(wrapper.get('[data-access-clear-all]').element.disabled).toBe(true)
})
it('does not claim a change the host did not show, and names a host too old to make one', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
  startAttached(async () => true)
  await wrapper.get('[data-access-select-all]').trigger('click'); await flushPromises()
  expect(wrapper.text()).toContain('The change has not been confirmed.')
  vi.stubGlobal('fetch', vi.fn(async () => reply({}, 404)))
  await wrapper.get('[data-access-select-all]').trigger('click'); await flushPromises()
  expect(wrapper.get('[role="alert"]').text()).toContain('cannot change every device at once')
})
it('offers neither while a Space streams, and leaves a lone device without them', async () => {
  startAttached(async () => true, { locked: true, lockReasonId: 'spaces-stream-lock' })
  expect(wrapper.get('[data-access-select-all]').element.disabled).toBe(true)
  expect(wrapper.get('[data-access-clear-all]').attributes('aria-describedby')).toBe('spaces-stream-lock')
  wrapper.unmount()
  startAttached(async () => true, { clients: [devices[1]] })
  expect(wrapper.find('[data-access-bulk]').exists()).toBe(false)
})
