import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpacesList from './SpacesList.vue'
import { validSnapshot } from '../spaces-access.js'
import { spacesGlobal } from './spaces-test-i18n.js'
let wrapper
// A host sends both: `family` is what decides behaviour, and `steam` stays for
// a client that predates launcher families.
const space = () => ({ id: 'space-a', name: 'Alex', clients: ['handheld'], family: 'steam', steam: true, archived: false })
const second = () => ({ id: 'space-b', name: 'Sam', clients: [], family: 'steam', steam: true, archived: false })
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/u
async function typeName(value) {
  const input = dialog().querySelector('[data-remove-name]')
  input.value = value
  input.dispatchEvent(new Event('input'))
  await flushPromises()
}
async function chooseRemoveForGood() {
  const choice = dialog().querySelector('[data-remove-delete]')
  choice.checked = true
  choice.dispatchEvent(new Event('change'))
  await flushPromises()
}
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
  it('confirms persisted removal as an archive by default and shows it as restorable', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a' })))
    start({ removalAvailable: true, profiles: [space(), second()],
      refresh: async () => { await wrapper.setProps({ profiles: [{ ...space(), archived: true, clients: [] }, second()] }); return true } })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    // Deleting games and saves is never what a plain confirm does.
    expect(dialog().querySelector('[data-remove-archive]').checked).toBe(true)
    expect(dialog().querySelector('[data-remove-delete]').checked).toBe(false)
    expect(dialog().querySelector('[data-remove-name]')).toBeNull()
    expect(dialog().querySelector('[data-confirm-confirm]').textContent.trim()).toBe('Archive Space')
    await confirm()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ operation: 'remove', profile_id: 'space-a' })
    expect(wrapper.text()).toContain('Alex was removed.')
    expect(wrapper.find('[data-space="space-a"]').exists()).toBe(false)
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
  it('keeps Archive selected until Remove for good is chosen, then waits for the exact name', async () => {
    vi.stubGlobal('fetch', vi.fn())
    start({ removalAvailable: true, profiles: [space(), second()] })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    expect(dialog().querySelector('[data-remove-choice]').textContent).toContain('Keeps its games, saves and Steam sign-in')
    expect(dialog().textContent).toContain('This does not free disk space')
    await chooseRemoveForGood()
    expect(dialog().textContent).toContain('Remove Alex for good?')
    expect(dialog().textContent).toContain('Installed games, saves and the Steam sign-in are deleted from this PC')
    expect(dialog().textContent).toContain('This cannot be undone')
    expect(dialog().textContent).toContain('Devices lose access to it')
    expect(dialog().textContent).not.toContain('This does not free disk space')
    expect(dialog().textContent).toContain('Type Alex to confirm')
    const button = () => dialog().querySelector('[data-confirm-confirm]')
    expect(button().textContent.trim()).toBe('Remove for good')
    expect(button().disabled).toBe(true)
    for (const typed of ['alex', 'Alex ', ' Alex', 'Ale']) {
      await typeName(typed)
      expect(button().disabled, typed).toBe(true)
    }
    await typeName('Alex')
    expect(button().disabled).toBe(false)
    expect(fetch).not.toHaveBeenCalled()
    dialog().querySelector('[data-confirm-cancel]').click()
    await flushPromises()
    expect(dialog()).toBeNull()
    // Opening again starts from Archive with an empty name.
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    expect(dialog().querySelector('[data-remove-archive]').checked).toBe(true)
    expect(dialog().querySelector('[data-remove-name]')).toBeNull()
    expect(fetch).not.toHaveBeenCalled()
  })

  it('removes for good with the typed name and a request identity, and confirms it by the Space being gone', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a' })))
    start({ removalAvailable: true, profiles: [space(), second()],
      refresh: async () => { await wrapper.setProps({ profiles: [second()] }); return true } })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    await chooseRemoveForGood()
    await typeName('Alex')
    await confirm()
    expect(fetch).toHaveBeenCalledTimes(1)
    const body = JSON.parse(fetch.mock.calls[0][1].body)
    expect(Object.keys(body).sort()).toEqual(['confirm_name', 'operation', 'profile_id', 'request_id'])
    expect(body).toMatchObject({ operation: 'delete', profile_id: 'space-a', confirm_name: 'Alex' })
    expect(body.request_id).toMatch(uuid)
    expect(wrapper.get('[role=status]').text()).toContain('Alex was removed for good. Its games and saves are deleted.')
    expect(dialog()).toBeNull()
    expect(wrapper.find('[data-space="space-a"]').exists()).toBe(false)
    expect(wrapper.find('[aria-label="Restore Alex"]').exists()).toBe(false)
  })

  it('shows the host reason, its fix and what Docker kept when a removal for good does not finish', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, profile_id: 'space-a', code: 'space_storage_not_removed',
      message: 'Docker did not confirm that this Space\'s games and saves were deleted. The Space is archived for now.',
      action: 'Remove it for good again from Archived Spaces to finish.', kept_volume: 'pv-space-a' }, 503)))
    start({ removalAvailable: true, profiles: [space(), second()],
      refresh: async () => { await wrapper.setProps({ profiles: [{ ...space(), archived: true, clients: [] }, second()] }); return true } })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    await chooseRemoveForGood()
    await typeName('Alex')
    await confirm()
    const alert = wrapper.get('[role=alert]').text()
    expect(alert).toContain('Docker did not confirm')
    expect(alert).toContain('Remove it for good again from Archived Spaces to finish.')
    expect(alert).toContain('Docker volume pv-space-a')
    expect(wrapper.text()).not.toContain('was removed for good')
    expect(wrapper.get('[aria-label="Remove Alex for good"]').exists()).toBe(true)
  })

  it('offers Remove for good beside Restore for an archived Space', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a', kept_network: 'pn-space-a' })))
    start({ removalAvailable: true, profiles: [{ ...space(), archived: true, clients: [] }, second()],
      refresh: async () => { await wrapper.setProps({ profiles: [second()] }); return true } })
    expect(wrapper.get('[aria-label="Restore Alex"]').exists()).toBe(true)
    await wrapper.get('[aria-label="Remove Alex for good"]').trigger('click')
    await flushPromises()
    expect(dialog().querySelector('[data-remove-choice]')).toBeNull()
    expect(dialog().textContent).toContain('Remove Alex for good?')
    expect(dialog().textContent).not.toContain('Devices lose access to it')
    expect(dialog().querySelector('[data-confirm-confirm]').disabled).toBe(true)
    await typeName('Alex')
    await confirm()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toMatchObject({ operation: 'delete', profile_id: 'space-a', confirm_name: 'Alex' })
    expect(wrapper.get('[role=status]').text()).toContain('Alex was removed for good.')
    expect(wrapper.get('[role=status]').text()).toContain('Docker may have kept its network pn-space-a')
  })

  it('keeps the last Space of a family archivable only, even beside another family', async () => {
    // A new Space copies one of its own family, so the last Heroic Space is
    // kept even when Steam Spaces remain, and the reverse.
    vi.stubGlobal('fetch', vi.fn())
    start({ removalAvailable: true, profiles: [space(), { ...second(), family: 'heroic', steam: false }] })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    expect(dialog().querySelector('[data-remove-delete]').disabled).toBe(true)
    expect(dialog().querySelector('#space-remove-last').textContent).toContain('This is the only Space')
  })

  it('keeps the last Space archivable only and says why before anything is typed', async () => {
    vi.stubGlobal('fetch', vi.fn())
    start({ removalAvailable: true })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    const choice = dialog().querySelector('[data-remove-delete]')
    expect(choice.disabled).toBe(true)
    expect(choice.getAttribute('aria-describedby')).toBe('space-remove-last')
    expect(dialog().querySelector('#space-remove-last').textContent).toContain('This is the only Space')
    expect(dialog().querySelector('[data-remove-archive]').checked).toBe(true)
    expect(dialog().querySelector('[data-confirm-confirm]').disabled).toBe(false)
    dialog().querySelector('[data-confirm-cancel]').click()
    await flushPromises()
    await wrapper.setProps({ profiles: [{ ...space(), archived: true, clients: [] }] })
    await wrapper.get('[aria-label="Remove Alex for good"]').trigger('click')
    await flushPromises()
    expect(dialog().querySelector('[data-remove-last]')).not.toBeNull()
    expect(dialog().querySelector('[data-remove-name]')).toBeNull()
    expect(dialog().querySelector('[data-confirm-confirm]').disabled).toBe(true)
    expect(fetch).not.toHaveBeenCalled()
  })

  it('keeps the plain archive dialog on a host that cannot remove for good', async () => {
    vi.stubGlobal('fetch', vi.fn())
    start({ profiles: [space(), { ...second(), archived: true }] })
    await wrapper.get('[aria-label="Remove Alex"]').trigger('click')
    await flushPromises()
    expect(dialog().querySelector('[data-remove-choice]')).toBeNull()
    expect(dialog().querySelector('[data-confirm-confirm]').textContent.trim()).toBe('Remove Space')
    dialog().querySelector('[data-confirm-cancel]').click()
    await flushPromises()
    expect(wrapper.find('[aria-label="Remove Sam for good"]').exists()).toBe(false)
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
    expect(validSnapshot({ ...snapshot, removal_available: true })).toBe(true)
    expect(validSnapshot({ ...snapshot, removal_available: 'true' })).toBe(false)
    expect(validSnapshot({ ...snapshot, profiles: [{ ...space(), archived: true }] })).toBe(false)
    expect(validSnapshot({ ...snapshot, profiles: [{ ...space(), archived: true, clients: [] }] })).toBe(true)
  })
})
