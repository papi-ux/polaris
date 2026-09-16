import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import MultiseatAssignments from './MultiseatAssignments.vue'
import { spacesGlobal } from './spaces-test-i18n.js'

const client = { uuid: 'device-a', name: 'Living room', perm: 0x07001F00, temporary_authorization: false }
const snapshot = () => ({ enabled: true, available: true, changing: false, failed: false,
  profiles: [{ id: 'profile-a', name: 'Alex', clients: [] }] })
const reply = (body, ok = true, status = 200) => ({ ok, status, json: async () => body })
const start = (props = {}) => mount(MultiseatAssignments, { props, global: spacesGlobal })
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.useRealTimers() })

describe('profile assignments', () => {
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
    const current = snapshot()
    vi.stubGlobal('fetch', vi.fn(async (_, options) => options.method === 'POST'
      ? new Promise(resolve => { finish = resolve }) : reply(current)))
    wrapper = start({ clients: [client] })
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
      ? reply({ status: false, message: 'Stop the Space streams first' }, false, 409) : reply(snapshot())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Stop the Space streams first')
    expect(wrapper.get('select').element.value).toBe('')
    expect(wrapper.text()).not.toContain('Assignment saved.')
  })

  it('surfaces the host reason when a refusal carries only an error field', async () => {
    vi.stubGlobal('fetch', vi.fn(async (_, options) => options.method === 'POST'
      ? reply({ status: false, error: 'Invalid paired device.' }, false, 400) : reply(snapshot())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Invalid paired device.')
  })

  it('explains profile sharing using device names without exposing catalog identifiers', async () => {
    const current = snapshot()
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
      .mockResolvedValueOnce(reply(snapshot()))
      .mockResolvedValueOnce(reply({ status: true }))
      .mockImplementationOnce(() => new Promise(resolve => { confirm = resolve })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.text()).not.toContain('Assignment saved.')
    expect(wrapper.get('select').element.disabled).toBe(true)
    expect(wrapper.get('section').attributes('aria-busy')).toBe('true')
    confirm(reply({ ...snapshot(), profiles: [{ id: 'profile-a', name: 'Alex', clients: ['device-a'] }] }))
    await flushPromises()
    expect(wrapper.text()).toContain('Living room has default Space Alex')
  })

  it('locks stale assignments after failed read-back and recovers through refresh', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(snapshot()))
      .mockResolvedValueOnce(reply({ status: true }))
      .mockResolvedValueOnce(reply({}, false, 503))
      .mockResolvedValueOnce(reply({ ...snapshot(), profiles: [{ id: 'profile-a', name: 'Alex', clients: ['device-a'] }] })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.text()).not.toContain('Assignment saved.')
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
    vi.stubGlobal('fetch', vi.fn(async (_, options) => reply(options.method === 'POST' ? { status: true } : snapshot())))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('could not be confirmed')
    expect(wrapper.text()).not.toContain('Assignment saved.')
    expect(wrapper.get('select').element.value).toBe('')
  })

  it('keeps pending activation distinct from a confirmed assignment', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(snapshot()))
      .mockResolvedValueOnce(reply({ status: false, message: 'Pending' }, true, 202))
      .mockResolvedValueOnce(reply({ ...snapshot(), changing: true })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('still being applied')
    expect(wrapper.text()).not.toContain('Assignment saved.')
    expect(wrapper.get('select').element.disabled).toBe(true)
  })

  it('preserves another device draft when saving and refreshing an assignment', async () => {
    let current = snapshot()
    const other = { ...client, uuid: 'device-b', name: 'Bedroom TV' }
    vi.stubGlobal('fetch', vi.fn(async (_, options) => {
      if (options.method === 'POST') {
        current = { ...snapshot(), profiles: [{ id: 'profile-a', name: 'Alex', clients: ['device-a'] }] }
        return reply({ status: true })
      }
      return reply(JSON.parse(JSON.stringify(current)))
    }))
    wrapper = start({ clients: [client, other] })
    await flushPromises()
    await wrapper.get('#gaming-profile-device-a').setValue('profile-a')
    await wrapper.get('#gaming-profile-device-b').setValue('profile-a')
    await wrapper.get('button').trigger('click')
    await flushPromises()
    await wrapper.setProps({ clients: [{ ...client }, { ...other }, { ...client, uuid: 'device-c', name: 'New device' }] })
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.get('#gaming-profile-device-b').element.value).toBe('profile-a')
    expect(wrapper.get('#gaming-profile-current-device-b').text()).toContain('Desktop')
    expect(wrapper.get('#gaming-profile-device-c').element.value).toBe('')
    expect(wrapper.text()).toContain('Unsaved change')
  })

  it('removes a draft for a profile that disappears on refresh', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce(reply(snapshot()))
      .mockResolvedValueOnce(reply({ ...snapshot(), profiles: [] })))
    wrapper = start({ clients: [client] })
    await flushPromises()
    await wrapper.get('select').setValue('profile-a')
    await wrapper.get('[data-spaces-refresh]').trigger('click')
    await flushPromises()
    expect(wrapper.get('select').element.value).toBe('')
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

  it('keeps guests out of profile assignment but permits removal of lost access', async () => {
    const former = { ...client, perm: 0 }
    const current = snapshot()
    current.profiles[0].clients = ['device-a']
    vi.stubGlobal('fetch', vi.fn(async () => reply(current)))
    wrapper = start({ clients: [former, { ...client, uuid: 'guest', temporary_authorization: true }] })
    await flushPromises()
    expect(wrapper.findAll('select')).toHaveLength(1)
    expect(wrapper.get('option[value="profile-a"]').element.disabled).toBe(true)
    expect(wrapper.get('option[value=""]').element.disabled).toBe(false)
    expect(wrapper.text()).toContain('no longer has Space access')
    await wrapper.get('select').setValue('')
    expect(wrapper.get('button').element.disabled).toBe(false)
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
