import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpacesList from './SpacesList.vue'
import { validSnapshot } from '../spaces-access.js'
import { spacesGlobal } from './spaces-test-i18n.js'
let wrapper
const space = () => ({ id: 'space-a', name: 'Alex', clients: ['handheld'], steam: true, archived: false })
const reply = (body, status = 200) => ({ ok: status < 300, status, json: async () => body })
const dialog = () => document.body.querySelector('[role="dialog"]')
function start(props = {}) {
  wrapper = mount(SpacesList, { attachTo: document.body, global: spacesGlobal, props: { profiles: [space()], manageable: true, ready: true,
    clients: [{ uuid: 'handheld', friendly_name: 'Retroid Pocket 6', perm: 0x04000000 }], refresh: async () => true, ...props } })
  return wrapper
}
async function confirm() {
  dialog().querySelector('[data-confirm-confirm]').click()
  await flushPromises()
}
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })
describe('Spaces management', () => {
  it('separates current activity from device access and never guesses a Steam user', async () => {
    start({ activity: [{ profile_id: 'space-a', client_id: 'handheld', state: 'running' }] })
    expect(wrapper.text()).toContain('Playing on Retroid Pocket 6')
    expect(wrapper.text()).toContain('Available to Retroid Pocket 6')
    expect(wrapper.text()).not.toContain('Steam Account:')
    await wrapper.setProps({ activity: [{ profile_id: 'space-a', client_id: 'handheld', state: 'stopping' }] })
    expect(wrapper.text()).toContain('Stopping on Retroid Pocket 6')
    await wrapper.setProps({ activity: null })
    expect(wrapper.get('article [role=status]').text()).toBe('Status unknown')
    await wrapper.setProps({ activity: [] })
    expect(wrapper.get('article [role=status]').text()).toBe('Ready')
    expect(wrapper.get('article [role=status]').classes()).toContain('text-success')
  })
  it('lists only devices that can still open the Space', () => {
    start({ profiles: [{ ...space(), access_clients: ['tv', 'former'] }],
      clients: [{ uuid: 'handheld', friendly_name: 'Retroid Pocket 6', perm: 0x04000000 }, { uuid: 'tv', name: 'Bedroom TV', perm: 0x04000000 },
        { uuid: 'former', name: 'Old phone', perm: 0 }] })
    expect(wrapper.text()).toContain('Available to Retroid Pocket 6, Bedroom TV')
    expect(wrapper.text()).not.toContain('Old phone')
  })
  it('rejects malformed or unrelated activity without accepting guessed availability', () => {
    const snapshot = { enabled: true, available: true, changing: false, failed: false, profiles: [space()] }
    const item = { profile_id: 'space-a', client_id: 'handheld', state: 'starting' }
    expect(validSnapshot({ ...snapshot, activity: [item] })).toBe(true)
    for (const activity of [null, {}, [{ ...item, profile_id: 'unknown' }], [{ ...item, state: 'ready' }], [{ ...item, client_id: 5 }]]) {
      expect(validSnapshot({ ...snapshot, activity })).toBe(false)
    }
  })
  it('names the device and requires an explicit removal confirmation in the shared dialog', async () => {
    vi.stubGlobal('fetch', vi.fn())
    start()
    expect(wrapper.text()).toContain('Available to Retroid Pocket 6')
    expect(dialog()).toBeNull()
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    expect(dialog().textContent).toContain('Remove Alex?')
    expect(dialog().textContent).toContain('This does not free disk space')
    expect(dialog().textContent).toContain('Installed games, saves and the Steam sign-in stay')
    expect(dialog().textContent).toContain('Stop Space streams before making this change')
    expect(fetch).not.toHaveBeenCalled()
    dialog().querySelector('[data-confirm-cancel]').click()
    await flushPromises()
    expect(dialog()).toBeNull()
    expect(fetch).not.toHaveBeenCalled()
  })
  it('confirms persisted removal, shows it as restorable, and exposes no delete-data switch', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a' })))
    start({ refresh: async () => { await wrapper.setProps({ profiles: [{ ...space(), archived: true, clients: [] }] }); return true } })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    await confirm()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ operation: 'remove', profile_id: 'space-a' })
    expect(wrapper.text()).toContain('Alex was removed.')
    expect(wrapper.find('article').exists()).toBe(false)
    expect(dialog()).toBeNull()
    expect(wrapper.get('[aria-label="Restore Alex"]').exists()).toBe(true)
  })
  it('does not report success on an accepted request with stale or unavailable read-back', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, profile_id: 'space-a' }, 202)))
    start({ refresh: async () => false })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    await confirm()
    expect(wrapper.text()).not.toContain('was removed.')
    expect(wrapper.text()).toContain('has not been confirmed')
    expect(dialog()).toBeNull()
    await wrapper.setProps({ profiles: [{ ...space(), archived: true, clients: [] }] })
    await flushPromises()
    expect(wrapper.text()).toContain('Alex was removed.')
  })
  it('retains the space and explains active-stream refusal with the host reason', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, profile_id: 'space-a', error: 'Stop every Space stream first' }, 409)))
    start()
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    await confirm()
    expect(wrapper.get('[role=alert]').text()).toContain('Stop every Space stream first')
    expect(wrapper.find('article').exists()).toBe(true)
  })
  it('restores without silently reassigning the previous devices', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a' })))
    start({ profiles: [{ ...space(), archived: true, clients: [] }],
      refresh: async () => { await wrapper.setProps({ profiles: [{ ...space(), clients: [] }] }); return true } })
    await wrapper.get('[aria-label="Restore Alex"]').trigger('click')
    await flushPromises()
    expect(dialog().textContent).toContain('Restore Alex?')
    await confirm()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ operation: 'restore', profile_id: 'space-a' })
    expect(wrapper.text()).toContain('Choose its devices')
    expect(wrapper.get('article').text()).toContain('No device access yet.')
  })
  it('renames in place by stable identity and verifies the new name', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a' })))
    start({ refresh: async () => { await wrapper.setProps({ profiles: [{ ...space(), name: 'Living room' }] }); return true } })
    await wrapper.get('[aria-label="Rename Alex"]').trigger('click')
    expect(wrapper.get('article form').exists()).toBe(true)
    await wrapper.get('input').setValue(' Living room ')
    await wrapper.get('form').trigger('submit'); await flushPromises()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ operation: 'rename', profile_id: 'space-a', name: 'Living room' })
    expect(wrapper.text()).toContain('Space renamed to Living room.')
    expect(wrapper.find('form').exists()).toBe(false)
  })
  it('says what an empty list needs and offers archived Spaces only when there are some', () => {
    start({ profiles: [], creationAvailable: false })
    expect(wrapper.get('[data-spaces-empty]').text()).toBe('No Spaces yet. Finish Host Setup to prepare the first one.')
    wrapper.unmount()
    start({ profiles: [{ ...space(), archived: true, clients: [] }], creationAvailable: true })
    expect(wrapper.get('[data-spaces-empty]').text()).toContain('No Spaces yet. Create one below.')
    expect(wrapper.get('[data-spaces-empty]').text()).toContain('Or restore an archived Space.')
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
