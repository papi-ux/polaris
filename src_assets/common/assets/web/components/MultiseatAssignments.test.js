import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import MultiseatAssignments from './MultiseatAssignments.vue'
import { spacesGlobal } from './spaces-test-i18n.js'

const client = { uuid: 'device-a', name: 'Living room', perm: 0x07001F00, temporary_authorization: false }
const snapshot = () => ({ enabled: true, available: true, changing: false, failed: false,
  profiles: [{ id: 'profile-a', name: 'Alex', clients: [] }] })
// A device that may open Alex and Desktop, and opens Desktop first.
const withAccess = () => ({ ...snapshot(), desktop_clients: ['device-a'], desktop_default_clients: ['device-a'],
  profiles: [{ id: 'profile-a', name: 'Alex', clients: [], access_clients: ['device-a'] }] })
// The same device after Alex became its Default Space: nothing it may open changed.
const defaultAlex = () => ({ ...withAccess(), desktop_default_clients: [],
  profiles: [{ id: 'profile-a', name: 'Alex', clients: ['device-a'], access_clients: ['device-a'] }] })
const reply = (body, ok = true, status = 200) => ({ ok, status, json: async () => body })
const start = (props = {}) => mount(MultiseatAssignments, { props, global: spacesGlobal })
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.useRealTimers() })

describe('profile assignments', () => {
  it('keeps each device to one row, and shows its help once there is something to act on', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ ...defaultAlex(), profiles: [...defaultAlex().profiles,
      { id: 'profile-b', name: 'Sam', clients: [], access_clients: ['device-a'] }] })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    expect(wrapper.findAll('[data-default-row]')).toHaveLength(1)
    const help = wrapper.get('#gaming-profile-help-device-a')
    // Still in the page, because the dropdown is described by it, but it takes no room.
    expect(help.classes()).toContain('sr-only')
    expect(wrapper.get('#gaming-profile-device-a').attributes('aria-describedby')).toContain('gaming-profile-help-device-a')
    await wrapper.get('#gaming-profile-device-a').setValue('profile-b')
    expect(wrapper.get('#gaming-profile-help-device-a').classes()).not.toContain('sr-only')
    expect(wrapper.text()).toContain('Unsaved')
  })

  it('hands the owner\'s Desktop setting to Desktop Access, and leaves it out for a host that has none', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ ...withAccess(), desktop_by_default: true })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    expect(wrapper.get('[data-desktop-by-default]').element.checked).toBe(true)
    wrapper.unmount()
    vi.stubGlobal('fetch', vi.fn(async () => reply(withAccess())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    expect(wrapper.find('[data-desktop-by-default]').exists()).toBe(false)
  })

  it('does not mistake a failed device lookup for an unpaired host', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    wrapper = start({ clients: [], clientsReady: false })
    await flushPromises()
    expect(wrapper.text()).not.toContain('Pair a device with permission')
    await wrapper.setProps({ clientsReady: true })
    expect(wrapper.text()).toContain('Pair a device with permission')
  })

  it('keeps ordinary device setup unchanged when multiseat is off', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ ...snapshot(), enabled: false })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    expect(wrapper.find('section').exists()).toBe(false)
    expect(fetch).toHaveBeenCalledTimes(1)
  })

  it('waits for persistence and read-back before showing a saved assignment', async () => {
    let finish
    let current = withAccess()
    vi.stubGlobal('fetch', vi.fn(async (_, options) => options.method === 'POST'
      ? new Promise(resolve => { finish = resolve }) : reply(current)))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    expect(wrapper.get('select').attributes('disabled')).toBeDefined()
    expect(wrapper.text()).not.toContain('opens Alex first')
    expect(JSON.parse(fetch.mock.calls[1][1].body)).toEqual({ client_id: 'device-a', profile_id: 'profile-a' })
    current = defaultAlex()
    finish(reply({ status: true }))
    await flushPromises()
    expect(wrapper.get('select').element.value).toBe('profile-a')
    expect(wrapper.text()).toContain('Saved. Living room opens Alex first. What it may open did not change.')
    expect(wrapper.get('button[aria-label="Save assignment for Living room"]').attributes('disabled')).toBeDefined()
  })

  it('retains the confirmed assignment when an active seat rejects a change', async () => {
    vi.stubGlobal('fetch', vi.fn(async (_, options) => options.method === 'POST'
      ? reply({ status: false, message: 'Stop the Space streams first' }, false, 409) : reply(withAccess())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Stop the Space streams first')
    expect(wrapper.get('select').element.value).toBe('desktop')
    expect(wrapper.text()).not.toContain('opens Alex first')
  })

  it('surfaces the host reason when a refusal carries only an error field', async () => {
    vi.stubGlobal('fetch', vi.fn(async (_, options) => options.method === 'POST'
      ? reply({ status: false, error: 'Invalid paired device.' }, false, 400) : reply(withAccess())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Invalid paired device.')
  })

  it('explains profile sharing using device names without exposing catalog identifiers', async () => {
    const current = withAccess()
    current.profiles[0].clients = ['device-b']
    vi.stubGlobal('fetch', vi.fn(async () => reply(current)))
    wrapper = start({ clients: [client, { ...client, uuid: 'device-b', name: 'Bedroom TV' }] })
    await flushPromises()
    await wrapper.get('#gaming-profile-device-a').setValue('profile-a')
    expect(wrapper.get('#gaming-profile-current-device-a').text()).toContain('Desktop')
    expect(wrapper.get('#gaming-profile-help-device-a').text()).toContain('Also assigned to Bedroom TV')
    expect(wrapper.get('#gaming-profile-help-device-a').text()).toContain('Only one')
    expect(wrapper.text()).toContain('Unsaved change')
    expect(wrapper.text()).not.toContain('device-b')
    expect(wrapper.get('button[aria-label="Save assignment for Living room"]').exists()).toBe(true)
  })

  it('does not claim success while read-back is still pending', async () => {
    let confirm
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(withAccess()))
      .mockResolvedValueOnce(reply({ status: true }))
      .mockImplementationOnce(() => new Promise(resolve => { confirm = resolve })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    await flushPromises()
    expect(wrapper.text()).not.toContain('opens Alex first')
    expect(wrapper.get('select').element.disabled).toBe(true)
    expect(wrapper.get('section').attributes('aria-busy')).toBe('true')
    confirm(reply(defaultAlex()))
    await flushPromises()
    expect(wrapper.text()).toContain('Living room opens Alex first')
  })

  it('locks stale assignments after failed read-back and recovers through refresh', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(withAccess()))
      .mockResolvedValueOnce(reply({ status: true }))
      .mockResolvedValueOnce(reply({}, false, 503))
      .mockResolvedValueOnce(reply(defaultAlex())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    await flushPromises()
    expect(wrapper.text()).not.toContain('opens Alex first')
    expect(wrapper.get('[role=alert]').text()).toContain('Refresh to try again')
    expect(wrapper.get('select').element.disabled).toBe(true)
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[role=alert]').exists()).toBe(false)
    expect(wrapper.get('select').element.disabled).toBe(false)
    expect(wrapper.get('#gaming-profile-current-device-a').text()).toContain('Alex')
    expect(wrapper.text()).not.toContain('Unsaved change')
  })

  it('reports an accepted request whose assignment did not take effect', async () => {
    vi.stubGlobal('fetch', vi.fn(async (_, options) => reply(options.method === 'POST' ? { status: true } : withAccess())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('could not be confirmed')
    expect(wrapper.text()).not.toContain('opens Alex first')
    expect(wrapper.get('select').element.value).toBe('desktop')
  })

  it('keeps pending activation distinct from a confirmed assignment', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(withAccess()))
      .mockResolvedValueOnce(reply({ status: false, message: 'Pending' }, true, 202))
      .mockResolvedValueOnce(reply({ ...withAccess(), changing: true })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('still being applied')
    expect(wrapper.text()).not.toContain('opens Alex first')
    expect(wrapper.get('select').element.disabled).toBe(true)
  })

  it('preserves another device draft when saving and refreshing an assignment', async () => {
    const both = ['device-a', 'device-b']
    let current = { ...withAccess(), desktop_clients: both, desktop_default_clients: both,
      profiles: [{ id: 'profile-a', name: 'Alex', clients: [], access_clients: both }] }
    const other = { ...client, uuid: 'device-b', name: 'Bedroom TV' }
    vi.stubGlobal('fetch', vi.fn(async (_, options) => {
      if (options.method === 'POST') {
        current = { ...current, desktop_default_clients: ['device-b'],
          profiles: [{ id: 'profile-a', name: 'Alex', clients: ['device-a'], access_clients: both }] }
        return reply({ status: true })
      }
      return reply(JSON.parse(JSON.stringify(current)))
    }))
    wrapper = start({ clients: [client, other] })
    await flushPromises()
    await wrapper.get('#gaming-profile-device-a').setValue('profile-a')
    await wrapper.get('#gaming-profile-device-b').setValue('profile-a')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    await flushPromises()
    await wrapper.setProps({ clients: [{ ...client }, { ...other }, { ...client, uuid: 'device-c', name: 'New device' }] })
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.get('#gaming-profile-device-b').element.value).toBe('profile-a')
    expect(wrapper.get('#gaming-profile-current-device-b').text()).toContain('Desktop')
    expect(wrapper.get('#gaming-profile-device-c').element.value).toBe('desktop')
    expect(wrapper.text()).toContain('Unsaved change')
  })

  it('removes a draft for a profile that disappears on refresh', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(withAccess()))
      .mockResolvedValueOnce(reply({ ...withAccess(), profiles: [] })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.get('select').element.value).toBe('desktop')
    expect(wrapper.text()).toContain('No Spaces yet')
    expect(wrapper.text()).not.toContain('Unsaved change')
  })

  it.each([
    ['missing availability', value => { delete value.available }],
    ['nonboolean availability', value => { value.available = 'true' }],
    ['duplicate profile', value => { value.profiles.push({ ...value.profiles[0] }) }],
    ['duplicate device assignment', value => { value.profiles[0].clients = ['device-a']; value.profiles.push({ id: 'profile-b', name: 'Sam', clients: ['device-a'] }) }],
    ['malformed profile', value => { value.profiles = [null] }],
    ['nonstring device', value => { value.profiles[0].clients = [42] }],
    ['device with two Default Spaces', value => { value.profiles[0].clients = ['device-a']; value.desktop_default_clients = ['device-a'] }],
    ['repeated Desktop default', value => { value.desktop_default_clients = ['device-a', 'device-a'] }],
  ])('rejects %s without enabling changes from a stale snapshot', async (_, mutate) => {
    const invalid = snapshot()
    mutate(invalid)
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(snapshot()))
      .mockResolvedValueOnce(reply(invalid)))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Could not verify')
    expect(wrapper.get('select').element.disabled).toBe(true)
  })

  it('offers a retry after the initial load fails', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockRejectedValueOnce(new Error('Connection unavailable'))
      .mockResolvedValueOnce(reply({ ...snapshot(), enabled: false })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Connection unavailable')
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.find('section').exists()).toBe(false)
  })

  it('keeps guests out of profile assignment but removes lost access on request', async () => {
    const former = { ...client, perm: 0 }
    let current = snapshot()
    current.profiles[0].clients = ['device-a']
    current.profiles[0].access_clients = ['device-a']
    vi.stubGlobal('fetch', vi.fn(async (_, options) => {
      if (options.method === 'POST') {
        current = snapshot()
        return reply({ status: true })
      }
      return reply(JSON.parse(JSON.stringify(current)))
    }))
    wrapper = start({ clients: [former, { ...client, uuid: 'guest', name: 'Guest', temporary_authorization: true }] })
    await flushPromises()
    expect(wrapper.findAll('select')).toHaveLength(0)
    expect(wrapper.text()).toContain('can no longer launch games')
    const remove = wrapper.get('button[aria-label="Remove Living room from every Space"]')
    expect(remove.element.disabled).toBe(false)
    await remove.trigger('click')
    await flushPromises()
    expect(JSON.parse(fetch.mock.calls[1][1].body)).toEqual({ client_id: 'device-a', profile_id: '' })
    expect(wrapper.text()).toContain('Living room was removed from every Space.')
    expect(wrapper.find('button[aria-label="Remove Living room from every Space"]').exists()).toBe(false)
  })

  it('offers only places a device may play, and saving Desktop leaves its access alone', async () => {
    let current = { ...snapshot(), access_available: true, management_available: true, desktop_clients: [],
      profiles: [{ id: 'profile-a', name: 'Alex', clients: [], access_clients: ['device-a'] }, { id: 'profile-b', name: 'Sam', clients: [] }] }
    vi.stubGlobal('fetch', vi.fn(async (_, options) => {
      if (options.method === 'POST') {
        current = { ...current, desktop_default_clients: ['device-a'] }
        return reply({ status: true })
      }
      return reply(JSON.parse(JSON.stringify(current)))
    }))
    wrapper = start({ clients: [client] })
    await flushPromises()
    const values = () => wrapper.findAll('#gaming-profile-device-a option').map(option => option.element.value)
    // Without Desktop Access the device opens its only Space, and Sam is not offered.
    expect(values()).toEqual(['profile-a'])
    expect(wrapper.get('#gaming-profile-current-device-a').text()).toContain('Alex')
    expect(wrapper.get('[data-desktop-needs-access]').text()).toContain('give this device Desktop Access')
    current = { ...current, desktop_clients: ['device-a'] }
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(values()).toEqual(['desktop', 'profile-a'])
    expect(wrapper.find('[data-desktop-needs-access]').exists()).toBe(false)
    await wrapper.get('#gaming-profile-device-a').setValue('desktop')
    expect(wrapper.get('#gaming-profile-help-device-a').text()).toContain('Its Spaces stay open to it')
    await wrapper.get('button[aria-label="Save assignment for Living room"]').trigger('click')
    await flushPromises()
    const post = fetch.mock.calls.find(([, options]) => options.method === 'POST')
    expect(JSON.parse(post[1].body)).toEqual({ client_id: 'device-a', profile_id: 'desktop' })
    expect(wrapper.text()).toContain('Saved. Living room opens Desktop first, and its Spaces stay open to it.')
    expect(wrapper.get('#gaming-profile-current-device-a').text()).toContain('Desktop')
    expect(wrapper.get('input[aria-label="Allow Living room to use Alex"]').element.checked).toBe(true)
  })

  it('shows a device with no Space as playing on Desktop', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    expect(wrapper.findAll('#gaming-profile-device-a option').map(option => option.element.value)).toEqual(['desktop'])
    expect(wrapper.get('#gaming-profile-help-device-a').text()).toContain('This device plays on Desktop')
    expect(wrapper.get('button[aria-label="Save assignment for Living room"]').element.disabled).toBe(true)
  })

  it('tells devices with the same name apart by when they paired', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    const twin = { ...client, uuid: 'device-b', paired_at: 1789600000 }
    wrapper = start({ clients: [{ ...client, paired_at: 1789593894 }, twin] })
    await flushPromises()
    const labels = wrapper.findAll('label[for^="gaming-profile-"]').map(label => label.text())
    expect(labels).toHaveLength(2)
    expect(labels[0]).toMatch(/^Living room \(paired .+\)$/)
    expect(labels[1]).toMatch(/^Living room \(paired .+\)$/)
    expect(labels[0]).not.toBe(labels[1])
  })

  it('explains why there are no devices to assign', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    wrapper = start()
    await flushPromises()
    expect(wrapper.text()).toContain('Pair a device with permission to launch apps')
    expect(wrapper.find('select').exists()).toBe(false)
  })

  it('locks every change while a Space streams and names the device to stop', async () => {
    const current = { ...snapshot(), management_available: true, access_available: true,
      activity: [{ profile_id: 'profile-a', client_id: 'device-a', state: 'running' }] }
    vi.stubGlobal('fetch', vi.fn(async () => reply(current)))
    wrapper = start({ clients: [client] })
    await flushPromises()
    const lock = wrapper.get('[data-stream-lock]')
    expect(lock.text()).toContain('Stop the stream on Living room first')
    expect(wrapper.get('select').element.disabled).toBe(true)
    expect(wrapper.get('button[aria-label="Rename Alex"]').element.disabled).toBe(true)
    expect(wrapper.get('button[aria-label="Rename Alex"]').attributes('aria-describedby')).toBe(lock.attributes('id'))
    expect(wrapper.get('[data-spaces-refresh]').element.disabled).toBe(false)
    current.activity = [{ profile_id: 'profile-a', client_id: 'device-a', state: 'stopping' }]
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.get('[data-stream-lock]').text()).toContain('Living room is still closing its Space')
    current.activity = []
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-stream-lock]').exists()).toBe(false)
    expect(wrapper.get('select').element.disabled).toBe(false)
  })

  it('offers Remove for good only when the host says it can', async () => {
    const archived = { id: 'profile-b', name: 'Sam', clients: [], steam: true, archived: true }
    const current = { ...snapshot(), management_available: true, removal_available: true,
      profiles: [{ ...snapshot().profiles[0], steam: true }, archived] }
    vi.stubGlobal('fetch', vi.fn(async () => reply(current)))
    wrapper = start({ clients: [client] })
    await flushPromises()
    expect(wrapper.find('button[aria-label="Remove Sam for good"]').exists()).toBe(true)
    delete current.removal_available
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.find('button[aria-label="Remove Sam for good"]').exists()).toBe(false)
    expect(wrapper.find('button[aria-label="Restore Sam"]').exists()).toBe(true)
  })

  it('shows the host budget when the snapshot carries it', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ ...snapshot(), capacity: { concurrent_limit: 1, concurrent_active: 1 } })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    expect(wrapper.get('[data-spaces-capacity]').text()).toBe('1 of 1 Space slots in use')
  })

  it('confirms a Desktop Access grant through the same read-back as every other change', async () => {
    let current = { ...snapshot(), desktop_clients: [] }
    vi.stubGlobal('fetch', vi.fn(async (_, options) => {
      if (options.method === 'POST') {
        current = { ...current, desktop_clients: ['device-a'] }
        return reply({ status: true })
      }
      return reply(JSON.parse(JSON.stringify(current)))
    }))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('input[aria-label="Allow Desktop for Living room"]').setValue(true)
    await flushPromises()
    expect(JSON.parse(fetch.mock.calls[1][1].body)).toEqual({ profile_id: 'desktop', client_id: 'device-a', allowed: true })
    expect(wrapper.text()).toContain('Desktop Access saved.')
    expect(wrapper.text()).not.toContain('has not been confirmed')
    current = snapshot()
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.find('input[aria-label="Allow Desktop for Living room"]').exists()).toBe(false)
  })

  it('refreshes on its own while visible and pauses while the tab is hidden', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn(async () => reply(snapshot())))
    wrapper = start({ clients: [client] })
    await vi.advanceTimersByTimeAsync(0)
    expect(fetch).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(10000)
    expect(fetch).toHaveBeenCalledTimes(2)
    expect(wrapper.get('select').element.disabled).toBe(false)
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => true })
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(30000)
    expect(fetch).toHaveBeenCalledTimes(2)
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => false })
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(0)
    expect(fetch).toHaveBeenCalledTimes(3)
  })
})
