import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, expect, it, vi } from 'vitest'
import DesktopAccess from './DesktopAccess.vue'
import { validSnapshot } from '../spaces-access.js'
import { spacesGlobal } from './spaces-test-i18n.js'
let wrapper
const clients = [{ uuid: 'rp6', friendly_name: 'Retroid', perm: 0x04000000 },
  { uuid: 'guest', name: 'Guest', perm: 0x04000000, temporary_authorization: true }]
function start(refresh = async () => true, props = {}) { wrapper = mount(DesktopAccess, { global: spacesGlobal, props: { clients, allowed: [], refresh, ...props } }) }
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })
it('never grants desktop access implicitly and excludes guests', () => {
  start(); expect(wrapper.findAll('input')).toHaveLength(1)
  expect(wrapper.find('input').element.checked).toBe(false)
  expect(wrapper.text()).not.toContain('Guest')
})
it('tells same-named devices apart in the list and in each checkbox label', () => {
  start(async () => true, { clients: [
    { uuid: 'rp6', name: 'RetroidPocket6', perm: 0x04000000, paired_at: 1789593894 },
    { uuid: 'rp6-debug', name: 'RetroidPocket6', perm: 0x04000000, paired_at: 1789600000 },
  ] })
  const labels = wrapper.findAll('input').map(input => input.attributes('aria-label'))
  expect(labels).toHaveLength(2)
  expect(labels.every(label => /^Allow Desktop for RetroidPocket6 \(paired .+\)$/.test(label))).toBe(true)
  expect(labels[0]).not.toBe(labels[1])
})
it('says when no device can be granted Desktop', () => {
  start(async () => true, { clients: [clients[1]] })
  expect(wrapper.findAll('input')).toHaveLength(0)
  expect(wrapper.text()).toContain('Pair a device with permission to launch apps first.')
})
it('confirms only the requested device grant after server readback', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, status: 200, json: async () => ({ status: true }) })))
  start(async () => { await wrapper.setProps({ allowed: ['rp6'] }); return true })
  await wrapper.find('input').setValue(true); await flushPromises()
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ profile_id: 'desktop', client_id: 'rp6', allowed: true })
  expect(wrapper.text()).toContain('Desktop Access saved.')
})
it('does not announce success for a refused or unconfirmed grant', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 409, json: async () => ({ status: false, message: 'End the stream first' }) })))
  start(); await wrapper.find('input').setValue(true); await flushPromises()
  expect(wrapper.find('input').element.checked).toBe(false)
  expect(wrapper.text()).toContain('End the stream first'); expect(wrapper.text()).not.toContain('Access saved')
  fetch.mockResolvedValue({ ok: false, status: 202, json: async () => ({ status: false }) })
  await wrapper.find('input').setValue(true); await flushPromises()
  expect(wrapper.text()).toContain('has not been confirmed'); expect(wrapper.text()).not.toContain('Access saved')
})
it('shows the host reason when a bad request carries only an error field', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 400, json: async () => ({ status: false, error: 'Unknown Space.' }) })))
  start(); await wrapper.find('input').setValue(true); await flushPromises()
  expect(wrapper.get('[role=alert]').text()).toContain('Unknown Space.')
})
it('rejects ambiguous desktop grants in the refreshed snapshot', () => {
  const base = { enabled: true, available: true, changing: false, failed: false, profiles: [] }
  expect(validSnapshot({ ...base, desktop_clients: ['rp6'] })).toBe(true)
  for (const desktop_clients of [['rp6', 'rp6'], [''], 'rp6', [true]])
    expect(validSnapshot({ ...base, desktop_clients })).toBe(false)
})

const bulkReply = (body, status = 200) => ({ ok: status < 300, status, json: async () => body })
const dialog = () => document.body.querySelector('[role="dialog"]')
const two = [{ uuid: 'rp6', friendly_name: 'Retroid', perm: 0x04000000 }, { uuid: 'tv', name: 'TV', perm: 0x04000000 }]
function startAttached(refresh, props = {}) {
  wrapper = mount(DesktopAccess, { attachTo: document.body, global: spacesGlobal, props: { clients: two, allowed: [], refresh, ...props } })
}
it('gives every device Desktop with one request', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => bulkReply({ status: true })))
  startAttached(async () => { await wrapper.setProps({ allowed: ['rp6', 'tv'] }); return true })
  await wrapper.get('[data-access-select-all]').trigger('click'); await flushPromises()
  expect(fetch).toHaveBeenCalledTimes(1)
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ profile_id: 'desktop', allowed: true })
  expect(wrapper.text()).toContain('All 2 devices can open Desktop.')
})
it('asks before it takes Desktop from every device', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => bulkReply({ status: true })))
  startAttached(async () => { await wrapper.setProps({ allowed: [] }); return true }, { allowed: ['rp6'] })
  await wrapper.get('[data-access-clear-all]').trigger('click'); await flushPromises()
  expect(fetch).not.toHaveBeenCalled()
  expect(dialog().textContent).toContain('Remove Desktop Access from every device?')
  dialog().querySelector('[data-confirm-confirm]').click(); await flushPromises()
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ profile_id: 'desktop', allowed: false })
  expect(wrapper.text()).toContain('No device with a Space can open Desktop now.')
})
it('turns on Desktop with a Space, and keeps the switch as the host has it until the host says otherwise', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => bulkReply({ status: true })))
  startAttached(async () => { await wrapper.setProps({ byDefault: true }); return true }, { byDefault: false })
  const toggle = wrapper.get('[data-desktop-by-default]')
  expect(toggle.element.checked).toBe(false)
  await toggle.setValue(true); await flushPromises()
  expect(fetch.mock.calls[0][0]).toBe('./api/multiseat/settings')
  expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ desktop_by_default: true })
  expect(wrapper.get('[data-desktop-by-default]').element.checked).toBe(true)
  expect(wrapper.text()).toContain('A device you let into a Space now gets Desktop with it.')
})
it('leaves the switch as it was when the host refuses, and says why', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => bulkReply({ status: false, message: 'The Desktop Access setting was not saved.' }, 503)))
  startAttached(async () => true, { byDefault: false })
  await wrapper.get('[data-desktop-by-default]').setValue(true); await flushPromises()
  expect(wrapper.get('[data-desktop-by-default]').element.checked).toBe(false)
  expect(wrapper.get('[role="alert"]').text()).toContain('was not saved')
})
it('shows no switch on a host that has no such setting', () => {
  startAttached(async () => true)
  expect(wrapper.find('[data-desktop-by-default]').exists()).toBe(false)
  expect(validSnapshot({ enabled: true, available: true, changing: false, failed: false, profiles: [], desktop_by_default: true })).toBe(true)
  expect(validSnapshot({ enabled: true, available: true, changing: false, failed: false, profiles: [], desktop_by_default: 'yes' })).toBe(false)
})
