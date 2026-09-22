import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpacesDevices from './SpacesDevices.vue'
import { spacesGlobal } from './spaces-test-i18n.js'

let wrapper
const launch = 0x04000000
const tv = { uuid: 'tv', name: 'TV', perm: launch }
const rp6 = { uuid: 'rp6', friendly_name: 'Retroid', perm: launch }
const guest = { uuid: 'guest', name: 'Guest', perm: launch, temporary_authorization: true }
const alex = (clients = ['tv'], access = []) => ({ id: 'a', name: 'Alex', clients, access_clients: access })
const snapshot = (extra = {}) => ({ enabled: true, available: true, changing: false, failed: false, profiles: [alex()], ...extra })
const reply = (body, status = 200) => ({ ok: status < 300, status, json: async () => body })
const dialog = () => document.body.querySelector('[role="dialog"]')
// next is the snapshot the host shows after the change; none means the host shows no change.
function start(state = snapshot(), next = null, props = {}) {
  const refresh = async () => { if (next) await wrapper.setProps({ state: next, ...(next.by_default === undefined ? {} : { byDefault: next.by_default }) }); return true }
  wrapper = mount(SpacesDevices, { attachTo: document.body, global: spacesGlobal,
    props: { state, clients: [tv, rp6, guest], accessAvailable: true, refresh, ...props } })
}
const box = label => wrapper.get(`input[aria-label="${label}"]`)
const posted = (call = 0) => JSON.parse(fetch.mock.calls[call][1].body)
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })

describe('the device table', () => {
  it('lists each device once, with a column for Desktop and for every Space', () => {
    start(snapshot({ desktop_clients: ['tv'], profiles: [alex(), { id: 'b', name: 'Sam', clients: [], access_clients: ['rp6'] },
      { id: 'c', name: 'Old', clients: [], archived: true }] }))
    expect(wrapper.findAll('[data-device-row]')).toHaveLength(2)
    expect(wrapper.findAll('[data-devices-column]').map(cell => cell.text())).toEqual(['Desktop', 'Alex', 'Sam'])
    expect(wrapper.get('[data-devices-count]').text()).toBe('2')
    expect(wrapper.text()).not.toContain('Guest')
    expect(box('Allow Desktop for TV').element.checked).toBe(true)
    expect(box('Allow Desktop for Retroid').element.checked).toBe(false)
    // A Default Space counts as access: the device may open it.
    expect(box('Allow TV to use Alex').element.checked).toBe(true)
    expect(box('Allow Retroid to use Sam').element.checked).toBe(true)
    expect(box('Allow Retroid to use Alex').element.checked).toBe(false)
  })

  it('keeps its table roles, so the rows still read as a table when they are laid out as cards', () => {
    start()
    expect(wrapper.get('table').attributes('role')).toBe('table')
    expect(wrapper.findAll('[role="columnheader"]').length).toBe(3)
    expect(wrapper.findAll('tbody [role="rowheader"]').length).toBe(3)
    // Each cell names its column for the card layout, where the header row is not shown.
    expect(wrapper.get('[data-device-row="rp6"] .devices-cell-label').text()).toBe('Alex')
  })

  it('shows Space columns only when the host can change access, and Desktop only when it lists it', () => {
    start(snapshot({ desktop_clients: [] }), null, { accessAvailable: false })
    expect(wrapper.findAll('[data-devices-column]').map(cell => cell.text())).toEqual(['Desktop'])
    wrapper.unmount()
    start(snapshot())
    expect(wrapper.findAll('[data-devices-column]').map(cell => cell.text())).toEqual(['Alex'])
  })

  it('never grants Desktop implicitly', () => {
    start(snapshot({ desktop_clients: [] }))
    expect(box('Allow Desktop for TV').element.checked).toBe(false)
    expect(box('Allow Desktop for Retroid').element.checked).toBe(false)
  })

  it('tells same-named devices apart in the row and in each checkbox label', () => {
    start(snapshot({ desktop_clients: [] }), null, { clients: [
      { uuid: 'rp6', name: 'RetroidPocket6', perm: launch, paired_at: 1789593894 },
      { uuid: 'rp6-debug', name: 'RetroidPocket6', perm: launch, paired_at: 1789600000 }] })
    const names = wrapper.findAll('[data-device-name]').map(name => name.text())
    expect(names.every(name => /^RetroidPocket6 \(paired .+\)$/.test(name))).toBe(true)
    expect(names[0]).not.toBe(names[1])
    const labels = wrapper.findAll('input[aria-label^="Allow Desktop for"]').map(input => input.attributes('aria-label'))
    expect(labels).toHaveLength(2)
    expect(labels[0]).not.toBe(labels[1])
  })

  it('says nothing at all when there is no device to list', () => {
    start(snapshot({ profiles: [alex([])] }), null, { clients: [guest] })
    expect(wrapper.find('[data-spaces-devices]').exists()).toBe(false)
  })
})

describe('one tick', () => {
  it('lets a device leave its Default Space by unticking it, and says so once the host shows it', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(snapshot(), snapshot({ profiles: [alex([])] }))
    expect(box('Allow TV to use Alex').element.disabled).toBe(false)
    await box('Allow TV to use Alex').setValue(false); await flushPromises()
    expect(fetch.mock.calls[0][0]).toBe('./api/multiseat/access')
    expect(posted()).toEqual({ profile_id: 'a', client_id: 'tv', allowed: false })
    expect(wrapper.get('[role="status"]').text()).toBe('Device Access saved.')
    expect(wrapper.emitted('busy')).toEqual([[true], [false]])
  })

  it('changes only the requested grant', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(snapshot(), snapshot({ profiles: [alex(['tv'], ['rp6'])] }))
    await box('Allow Retroid to use Alex').setValue(true); await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(1)
    expect(posted()).toEqual({ profile_id: 'a', client_id: 'rp6', allowed: true })
    expect(box('Allow Retroid to use Alex').element.checked).toBe(true)
    expect(wrapper.text()).toContain('Device Access saved.')
  })

  it('gives Desktop to the one device asked for', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(snapshot({ desktop_clients: [] }), snapshot({ desktop_clients: ['rp6'] }))
    await box('Allow Desktop for Retroid').setValue(true); await flushPromises()
    expect(posted()).toEqual({ profile_id: 'desktop', client_id: 'rp6', allowed: true })
    expect(wrapper.text()).toContain('Desktop Access saved.')
  })

  it('keeps the old tick when the host refuses during a stream, and says why', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, message: 'Stop Space streams first' }, 409)))
    start()
    await box('Allow Retroid to use Alex').setValue(true); await flushPromises()
    expect(box('Allow Retroid to use Alex').element.checked).toBe(false)
    expect(wrapper.get('[role="alert"]').text()).toContain('Stop Space streams first')
    expect(wrapper.text()).not.toContain('Access saved')
  })

  it('shows the host reason when a refusal carries only an error field', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, error: 'Invalid Space or paired device.' }, 400)))
    start()
    await box('Allow Retroid to use Alex').setValue(true); await flushPromises()
    expect(wrapper.get('[role="alert"]').text()).toContain('Invalid Space or paired device.')
  })

  it('does not announce a grant the host accepted but does not show yet', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false }, 202)))
    start(snapshot({ desktop_clients: [] }))
    await box('Allow Desktop for Retroid').setValue(true); await flushPromises()
    expect(wrapper.text()).toContain('has not been confirmed')
    expect(wrapper.text()).not.toContain('Access saved')
  })

  it('names the stream lock on every control while a Space streams', () => {
    start(snapshot({ desktop_clients: [] }), null, { locked: true, lockReasonId: 'spaces-stream-lock', byDefault: false })
    for (const control of [box('Allow Retroid to use Alex'), box('Allow Desktop for TV'), wrapper.get('[data-desktop-by-default]'),
      wrapper.get('[data-access-select-all]'), wrapper.get('[data-access-clear-all]')]) {
      expect(control.element.disabled).toBe(true)
      expect(control.attributes('aria-describedby')).toBe('spaces-stream-lock')
    }
  })
})

// All and None: one host change, where ticking thirteen devices was thirteen saves and thirteen
// restarts of Spaces.
describe('every device at once', () => {
  const bulk = column => wrapper.get(`[data-access-bulk="${column}"]`)

  it('lets every device into a Space with one request, and says so only once the host shows it', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(snapshot(), snapshot({ profiles: [alex(['tv'], ['rp6'])] }))
    await bulk('a').get('[data-access-select-all]').trigger('click'); await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(1)
    expect(fetch.mock.calls[0][0]).toBe('./api/multiseat/access/all')
    expect(posted()).toEqual({ profile_id: 'a', allowed: true })
    expect(wrapper.text()).toContain('All 2 devices can open Alex.')
    expect(bulk('a').get('[data-access-select-all]').element.disabled).toBe(true)
  })

  it('asks before it removes every device from a Space, and says what else goes with them', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(snapshot(), snapshot({ profiles: [alex([])] }))
    await bulk('a').get('[data-access-clear-all]').trigger('click'); await flushPromises()
    expect(fetch).not.toHaveBeenCalled()
    expect(dialog().textContent).toContain('Remove every device from Alex?')
    expect(dialog().textContent).toContain('A device that opens this Space first goes back to opening another place first.')
    dialog().querySelector('[data-confirm-confirm]').click(); await flushPromises()
    expect(posted()).toEqual({ profile_id: 'a', allowed: false })
    expect(wrapper.text()).toContain('No device can open Alex now.')
    expect(dialog()).toBeNull()
    expect(bulk('a').get('[data-access-clear-all]').element.disabled).toBe(true)
  })

  it('gives every device Desktop with one request, and takes it back after asking', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(snapshot({ desktop_clients: [] }), snapshot({ desktop_clients: ['tv', 'rp6'] }))
    await bulk('desktop').get('[data-access-select-all]').trigger('click'); await flushPromises()
    expect(posted()).toEqual({ profile_id: 'desktop', allowed: true })
    expect(wrapper.text()).toContain('All 2 devices can open Desktop.')
    wrapper.unmount()
    start(snapshot({ desktop_clients: ['rp6'] }), snapshot({ desktop_clients: [] }))
    await bulk('desktop').get('[data-access-clear-all]').trigger('click'); await flushPromises()
    expect(dialog().textContent).toContain('Remove Desktop Access from every device?')
    dialog().querySelector('[data-confirm-confirm]').click(); await flushPromises()
    expect(posted(1)).toEqual({ profile_id: 'desktop', allowed: false })
    expect(wrapper.text()).toContain('No device with a Space can open Desktop now.')
  })

  it('offers None while the host still lists a device that was unpaired since', () => {
    start(snapshot({ profiles: [alex([], ['gone'])] }))
    expect(bulk('a').get('[data-access-clear-all]').element.disabled).toBe(false)
  })

  it('does not claim a change the host did not show, and names a host too old to make one', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start()
    await bulk('a').get('[data-access-select-all]').trigger('click'); await flushPromises()
    expect(wrapper.text()).toContain('The change has not been confirmed.')
    vi.stubGlobal('fetch', vi.fn(async () => reply({}, 404)))
    await bulk('a').get('[data-access-select-all]').trigger('click'); await flushPromises()
    expect(wrapper.get('[role="alert"]').text()).toContain('cannot change every device at once')
  })

  it('leaves a lone device without them', () => {
    start(snapshot(), null, { clients: [rp6] })
    expect(wrapper.find('[data-devices-bulk]').exists()).toBe(false)
  })
})

describe('Desktop with a Space', () => {
  it('turns on, and keeps the switch as the host has it until the host says otherwise', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(snapshot(), { ...snapshot(), by_default: true }, { byDefault: false })
    const toggle = wrapper.get('[data-desktop-by-default]')
    expect(toggle.element.checked).toBe(false)
    await toggle.setValue(true); await flushPromises()
    expect(fetch.mock.calls[0][0]).toBe('./api/multiseat/settings')
    expect(posted()).toEqual({ desktop_by_default: true })
    expect(wrapper.get('[data-desktop-by-default]').element.checked).toBe(true)
    expect(wrapper.text()).toContain('A device you let into a Space now gets Desktop with it.')
  })

  it('leaves the switch as it was when the host refuses, and says why', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, message: 'The Desktop Access setting was not saved.' }, 503)))
    start(snapshot(), null, { byDefault: false })
    await wrapper.get('[data-desktop-by-default]').setValue(true); await flushPromises()
    expect(wrapper.get('[data-desktop-by-default]').element.checked).toBe(false)
    expect(wrapper.get('[role="alert"]').text()).toContain('was not saved')
  })

  it('shows no switch on a host that has no such setting', () => {
    start()
    expect(wrapper.find('[data-desktop-by-default]').exists()).toBe(false)
  })
})

describe('Default Space', () => {
  const both = () => snapshot({ desktop_clients: ['rp6'], desktop_default_clients: ['rp6'], profiles: [alex(['tv'], ['rp6'])] })

  it('asks only a device with more than one place to play', () => {
    start(both())
    // TV may open Alex and nothing else, so it is told rather than asked.
    expect(wrapper.find('#gaming-profile-tv').exists()).toBe(false)
    expect(wrapper.get('[data-device-row="tv"] [data-default-only]').text()).toBe('Alex')
    const options = wrapper.findAll('#gaming-profile-rp6 option').map(option => [option.element.value, option.text()])
    expect(options).toEqual([['desktop', 'Desktop'], ['a', 'Alex']])
    expect(wrapper.get('#gaming-profile-rp6').element.value).toBe('desktop')
    // The device name is the dropdown's label.
    expect(wrapper.get('label[for="gaming-profile-rp6"]').text()).toBe('Retroid')
  })

  it('offers Save for an unsaved choice, and takes it back when the choice is undone', async () => {
    start(both())
    const save = 'button[aria-label="Save assignment for Retroid"]'
    expect(wrapper.find(save).exists()).toBe(false)
    expect(wrapper.get('#gaming-profile-help-rp6').classes()).toContain('sr-only')
    await wrapper.get('#gaming-profile-rp6').setValue('a')
    expect(wrapper.get(save).text()).toBe('Save')
    expect(wrapper.get('#gaming-profile-current-rp6').text()).toBe('Unsaved change. Default: Desktop')
    expect(wrapper.get('#gaming-profile-help-rp6').text()).toContain('Also assigned to TV')
    await wrapper.get('#gaming-profile-rp6').setValue('desktop')
    expect(wrapper.find(save).exists()).toBe(false)
  })

  it('saves the choice, and says so once the host shows it', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(both(), snapshot({ desktop_clients: ['rp6'], desktop_default_clients: [], profiles: [alex(['tv', 'rp6'], ['rp6'])] }))
    await wrapper.get('#gaming-profile-rp6').setValue('a')
    await wrapper.get('button[aria-label="Save assignment for Retroid"]').trigger('click'); await flushPromises()
    expect(fetch.mock.calls[0][0]).toBe('./api/multiseat/assign')
    expect(posted()).toEqual({ client_id: 'rp6', profile_id: 'a' })
    expect(wrapper.get('[role="status"]').text()).toContain('Saved. Retroid opens Alex first.')
    expect(wrapper.get('#gaming-profile-rp6').element.value).toBe('a')
    expect(wrapper.find('button[aria-label="Save assignment for Retroid"]').exists()).toBe(false)
  })

  it('goes back to what the host has when the host refuses the choice', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, message: 'Stop the Space streams first' }, 409)))
    start(both())
    await wrapper.get('#gaming-profile-rp6').setValue('a')
    await wrapper.get('button[aria-label="Save assignment for Retroid"]').trigger('click'); await flushPromises()
    expect(wrapper.get('[role="alert"]').text()).toContain('Stop the Space streams first')
    expect(wrapper.get('#gaming-profile-rp6').element.value).toBe('desktop')
    expect(wrapper.text()).not.toContain('Unsaved change')
  })

  it('drops an unsaved choice once unticking the Space takes that place away', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    start(both(), snapshot({ desktop_clients: ['rp6'], desktop_default_clients: ['rp6'] }))
    await wrapper.get('#gaming-profile-rp6').setValue('a')
    await box('Allow Retroid to use Alex').setValue(false); await flushPromises()
    expect(wrapper.find('#gaming-profile-rp6').exists()).toBe(false)
    expect(wrapper.get('[data-device-row="rp6"] [data-default-only]').text()).toBe('Desktop')
    expect(wrapper.text()).not.toContain('Unsaved change')
  })

  it('lists a device that lost launch permission so it can be removed, and keeps its ticks read-only', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true })))
    const former = { ...tv, perm: 0 }
    start(snapshot(), snapshot({ profiles: [alex([])] }), { clients: [former, rp6] })
    expect(wrapper.get('[data-device-row="tv"]').text()).toContain('can no longer launch games')
    expect(box('Allow TV to use Alex').element.checked).toBe(true)
    expect(box('Allow TV to use Alex').element.disabled).toBe(true)
    await wrapper.get('button[aria-label="Remove TV from every Space"]').trigger('click'); await flushPromises()
    expect(posted()).toEqual({ client_id: 'tv', profile_id: '' })
    expect(wrapper.text()).toContain('TV was removed from every Space.')
    expect(wrapper.find('[data-device-row="tv"]').exists()).toBe(false)
  })
})
