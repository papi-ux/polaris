import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpacesList from './SpacesList.vue'
import { validSnapshot } from '../spaces-access.js'
let wrapper
const space = () => ({ id: 'space-a', name: 'Alex', clients: ['handheld'], steam: true, archived: false })
const reply = (body, status = 200) => ({ ok: status < 300, status, json: async () => body })
function start(props = {}) {
  wrapper = mount(SpacesList, { attachTo: document.body, props: { profiles: [space()], manageable: true, ready: true,
    clients: [{ uuid: 'handheld', friendly_name: 'Retroid Pocket 6' }], refresh: async () => true, ...props } })
  return wrapper
}
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })
describe('Spaces management', () => {
  it('separates current activity from device access and never guesses a Steam user', async () => {
    start({ activity: [{ profile_id: 'space-a', client_id: 'handheld', state: 'running' }] })
    expect(wrapper.text()).toContain('Playing On Retroid Pocket 6')
    expect(wrapper.text()).toContain('Available To: Retroid Pocket 6')
    expect(wrapper.text()).toContain('Check Or Change In Steam Big Picture')
    await wrapper.setProps({ activity: [{ profile_id: 'space-a', client_id: 'handheld', state: 'stopping' }] })
    expect(wrapper.text()).toContain('Stopping On Retroid Pocket 6')
    await wrapper.setProps({ activity: null })
    expect(wrapper.get('article [role=status]').text()).toBe('Status Unavailable')
    await wrapper.setProps({ activity: [] })
    expect(wrapper.get('article [role=status]').text()).toBe('Available')
  })
  it('rejects malformed or unrelated activity without accepting guessed availability', () => {
    const snapshot = { enabled: true, available: true, changing: false, failed: false, profiles: [space()] }
    const item = { profile_id: 'space-a', client_id: 'handheld', state: 'starting' }
    expect(validSnapshot({ ...snapshot, activity: [item] })).toBe(true)
    for (const activity of [null, {}, [{ ...item, profile_id: 'unknown' }], [{ ...item, state: 'ready' }], [{ ...item, client_id: 5 }]]) {
      expect(validSnapshot({ ...snapshot, activity })).toBe(false)
    }
  })
  it('names the device and requires an explicit removal confirmation', async () => {
    vi.stubGlobal('fetch', vi.fn())
    start()
    expect(wrapper.text()).toContain('Available To: Retroid Pocket 6')
    await wrapper.get('[aria-label="Archive Alex"]').trigger('click')
    expect(wrapper.get('form').text()).toContain('This does not free disk space.')
    expect(wrapper.get('form').text()).toContain('Installed games, saves, and Steam sign-in stay')
    expect(fetch).not.toHaveBeenCalled()
    await wrapper.get('form button[type=button]').trigger('click')
    expect(wrapper.find('form').exists()).toBe(false)
    expect(fetch).not.toHaveBeenCalled()
  })
  it('confirms persisted removal, shows it as restorable, and exposes no delete-data switch', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a' })))
    start({ refresh: async () => { await wrapper.setProps({ profiles: [{ ...space(), archived: true, clients: [] }] }); return true } })
    await wrapper.get('[aria-label="Archive Alex"]').trigger('click')
    await wrapper.get('form').trigger('submit'); await flushPromises()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ operation: 'remove', profile_id: 'space-a' })
    expect(wrapper.text()).toContain('Alex was archived.')
    expect(wrapper.find('article').exists()).toBe(false)
    expect(wrapper.get('[aria-label="Restore Alex"]').exists()).toBe(true)
  })
  it('does not report success on an accepted request with stale or unavailable read-back', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, profile_id: 'space-a' }, 202)))
    start({ refresh: async () => false })
    await wrapper.get('[aria-label="Archive Alex"]').trigger('click')
    await wrapper.get('form').trigger('submit'); await flushPromises()
    expect(wrapper.text()).not.toContain('was archived.')
    expect(wrapper.text()).toContain('has not been confirmed')
    expect(wrapper.find('form').exists()).toBe(true)
    expect(wrapper.get('form button[type=button]').text()).toBe('Close')
    await wrapper.setProps({ profiles: [{ ...space(), archived: true, clients: [] }] })
    await flushPromises()
    expect(wrapper.text()).toContain('Alex was archived.')
    expect(wrapper.find('form').exists()).toBe(false)
  })
  it('retains the space and explains active-stream refusal', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, profile_id: 'space-a', message: 'Stop space streams first' }, 409)))
    start()
    await wrapper.get('[aria-label="Archive Alex"]').trigger('click')
    await wrapper.get('form').trigger('submit'); await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Stop space streams first')
    expect(wrapper.find('article').exists()).toBe(true)
  })
  it('restores without silently reassigning the previous devices', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a' })))
    start({ profiles: [{ ...space(), archived: true, clients: [] }],
      refresh: async () => { await wrapper.setProps({ profiles: [{ ...space(), clients: [] }] }); return true } })
    await wrapper.get('[aria-label="Restore Alex"]').trigger('click')
    await wrapper.get('form').trigger('submit'); await flushPromises()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ operation: 'restore', profile_id: 'space-a' })
    expect(wrapper.text()).toContain('Choose its devices below.')
    expect(wrapper.get('article').text()).toContain('No device access yet.')
  })
  it('renames by stable identity and verifies the new name', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a' })))
    start({ refresh: async () => { await wrapper.setProps({ profiles: [{ ...space(), name: 'Living room' }] }); return true } })
    await wrapper.get('[aria-label="Rename Alex"]').trigger('click')
    await wrapper.get('input').setValue(' Living room ')
    await wrapper.get('form').trigger('submit'); await flushPromises()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ operation: 'rename', profile_id: 'space-a', name: 'Living room' })
    expect(wrapper.text()).toContain('Space renamed to Living room.')
  })
  it('does not expose management against an older host and rejects archived routing', () => {
    start({ manageable: false })
    expect(wrapper.find('button').exists()).toBe(false)
    const snapshot = { enabled: true, available: true, changing: false, failed: false, profiles: [space()] }
    expect(validSnapshot(snapshot)).toBe(true)
    expect(validSnapshot({ ...snapshot, management_available: 'true' })).toBe(false)
    expect(validSnapshot({ ...snapshot, profiles: [{ ...space(), archived: true }] })).toBe(false)
    expect(validSnapshot({ ...snapshot, profiles: [{ ...space(), archived: true, clients: [] }] })).toBe(true)
  })
})
