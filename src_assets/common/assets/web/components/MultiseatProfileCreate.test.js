import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import MultiseatProfileCreate from './MultiseatProfileCreate.vue'
import { spacesGlobal } from './spaces-test-i18n.js'

const id = '12345678-1234-4234-8234-123456789abc'
const source = { id: 'profile-a', name: 'Alex', family: 'steam', steam: true, clients: ['device-a'] }
const created = { id, name: 'Player 2', family: 'steam', steam: true, clients: [] }
const reply = (body, status = 200) => ({ ok: status < 400, status, json: async () => body })
const button = text => wrapper.findAll('button').find(item => item.text() === text)
let wrapper, refresh
beforeEach(() => {
  sessionStorage.clear()
  refresh = vi.fn(async () => true)
  vi.stubGlobal('crypto', { randomUUID: vi.fn(() => id) })
  vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: id })))
})
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })

async function open(props = {}) {
  wrapper = mount(MultiseatProfileCreate, { global: spacesGlobal, props: { profiles: [source], ready: true, refresh, ...props } })
  await button('Create a Space').trigger('click')
  await wrapper.get('input').setValue('Player 2')
}

describe('Steam profile creation', () => {
  it('submits only a name, configured source, and stable request identity', async () => {
    refresh.mockImplementation(async () => {
      await wrapper.setProps({ profiles: [source, created] })
      return true
    })
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(1)
    const [url, options] = fetch.mock.calls[0]
    expect(url).toBe('./api/multiseat/profiles')
    expect(options.method).toBe('POST')
    expect(options.credentials).toBe('include')
    expect(JSON.parse(options.body)).toEqual({ request_id: id, family: 'steam', name: 'Player 2' })
    expect(wrapper.get('[role=status]').text()).toContain('Player 2 was created')
    expect(wrapper.find('form').exists()).toBe(false)
    expect(wrapper.emitted('busy')).toEqual([[true], [false]])
  })

  it('waits for verified catalog read-back before confirming creation', async () => {
    let finish
    refresh.mockImplementation(() => new Promise(resolve => { finish = resolve }))
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.text()).not.toContain('was created')
    expect(button('Creating…').element.disabled).toBe(true)
    await wrapper.setProps({ profiles: [source, created] })
    expect(wrapper.text()).not.toContain('was created')
    finish(true)
    await flushPromises()
    expect(wrapper.text()).toContain('Player 2 was created')
  })

  it('confirms a lost write response from the saved catalog', async () => {
    fetch.mockRejectedValueOnce(new Error('Connection lost'))
    refresh.mockImplementation(async () => {
      await wrapper.setProps({ profiles: [source, created] })
      return true
    })
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.find('[role=alert]').exists()).toBe(false)
    expect(wrapper.text()).toContain('Player 2 was created')
  })

  it('retains the same request when a response is lost and the catalog has no result', async () => {
    fetch.mockRejectedValueOnce(new Error('Connection lost'))
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.text()).not.toContain('was created')
    expect(wrapper.get('input').element.disabled).toBe(true)
    expect(button('Retry creation')).toBeDefined()
    refresh.mockImplementation(async () => {
      await wrapper.setProps({ profiles: [source, created] })
      return true
    })
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(2)
    expect(fetch.mock.calls[0][1].body).toBe(fetch.mock.calls[1][1].body)
    expect(crypto.randomUUID).toHaveBeenCalledTimes(1)
    expect(wrapper.text()).toContain('Player 2 was created')
  })

  it('checks a pending creation without sending another write', async () => {
    fetch.mockResolvedValueOnce(reply({ status: false, profile_id: id }, 202))
    refresh.mockImplementationOnce(async () => {
      await wrapper.setProps({ ready: false, locked: true })
      return true
    }).mockImplementationOnce(async () => {
      await wrapper.setProps({ profiles: [source, created], ready: true, locked: false })
      return true
    })
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.text()).toContain('still creating')
    expect(wrapper.text()).not.toContain('was created')
    expect(button('Retry creation').element.disabled).toBe(true)
    expect(button('Check creation status').element.disabled).toBe(false)
    await button('Check creation status').trigger('click')
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(1)
    expect(wrapper.text()).toContain('Player 2 was created')
  })

  it('does not accept a matching catalog entry while the controller is unavailable', async () => {
    fetch.mockResolvedValueOnce(reply({ status: false, message: 'Catalog requires review' }, 503))
    refresh.mockImplementation(async () => {
      await wrapper.setProps({ profiles: [source, created], ready: false, locked: true })
      return true
    })
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.text()).not.toContain('was created')
    expect(wrapper.get('[role=alert]').text()).toContain('Catalog requires review')
    expect(button('Retry creation').element.disabled).toBe(true)
  })

  it('offers the launchers this PC already runs a Space for, and sends the chosen one', async () => {
    // A Space is a launcher plus a home, so the choice is the launcher rather
    // than which Space to copy: every Space of a family shares its image.
    await open({ profiles: [source, { id: 'fixture', name: 'Comparison', family: '' },
      { id: 'heroic-a', name: 'Heroic Space', family: 'heroic', clients: [] },
      { id: 'gone', name: 'Archived', family: 'lutris', archived: true, clients: [] }] })
    expect(wrapper.findAll('option').map(item => item.text())).toEqual(['Steam', 'Heroic'])
    await wrapper.get('select').setValue('heroic')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(JSON.parse(fetch.mock.calls[0][1].body).family).toBe('heroic')
  })

  it('points at Host Setup when no launcher is set up yet', async () => {
    wrapper = mount(MultiseatProfileCreate, { global: spacesGlobal, props: { profiles: [{ ...source, family: '', steam: false }], refresh } })
    expect(button('Create a Space').element.disabled).toBe(true)
    expect(wrapper.text()).toContain('prepared under Host Setup')
    expect(fetch).not.toHaveBeenCalled()
  })

  it.each(['   ', '😀'.repeat(40), 'namecontrol'])('rejects a blank or oversized UTF-8 name before submitting', async value => {
    await open()
    await wrapper.get('input').setValue(value)
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(button('Create Space').element.disabled).toBe(true)
    expect(fetch).not.toHaveBeenCalled()
  })

  it('allows correction after the server rejects a source before provisioning', async () => {
    fetch.mockResolvedValueOnce(reply({ message: 'Select an existing Steam Space to base the new one on.' }, 404))
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.get('input').element.disabled).toBe(false)
    expect(button('Cancel')).toBeDefined()
    expect(wrapper.text()).not.toContain('was created')
  })

  it('retains retry state when the creation response names another request', async () => {
    fetch.mockResolvedValueOnce(reply({ status: true, profile_id: 'other' }))
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('could not be verified')
    expect(wrapper.text()).not.toContain('was created')
    expect(button('Retry creation')).toBeDefined()
  })

  it('releases the busy state even when catalog refresh fails', async () => {
    refresh.mockRejectedValue(new Error('Offline'))
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toContain('Could not refresh Spaces')
    expect(wrapper.emitted('busy')).toEqual([[true], [false]])
    expect(wrapper.text()).not.toContain('was created')
  })

  it('restores an unfinished request after navigation without automatically posting it again', async () => {
    fetch.mockRejectedValueOnce(new Error('Connection lost'))
    await open()
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    const body = fetch.mock.calls[0][1].body
    wrapper.unmount()
    wrapper = mount(MultiseatProfileCreate, { global: spacesGlobal, props: { profiles: [source], ready: true, refresh } })
    await flushPromises()
    expect(wrapper.get('input').element.value).toBe('Player 2')
    expect(wrapper.get('input').element.disabled).toBe(true)
    expect(fetch).toHaveBeenCalledTimes(1)
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(fetch.mock.calls[1][1].body).toBe(body)
    expect(crypto.randomUUID).toHaveBeenCalledTimes(1)
  })

  it('clears a restored request only after finding its exact saved space', async () => {
    sessionStorage.setItem('polaris:spaces:create-request:v1', JSON.stringify({ request_id: id, family: 'steam', name: created.name }))
    wrapper = mount(MultiseatProfileCreate, { global: spacesGlobal, props: { profiles: [source, created], ready: true, refresh } })
    await flushPromises()
    expect(wrapper.text()).toContain('Player 2 was created')
    expect(sessionStorage.getItem('polaris:spaces:create-request:v1')).toBeNull()
    expect(fetch).not.toHaveBeenCalled()
  })

  it('ignores a request saved before launchers, which named a Space to copy', async () => {
    // Left by an older console in this browser. It cannot be replayed as it
    // stands, and guessing a launcher for it would create the wrong Space.
    sessionStorage.setItem('polaris:spaces:create-request:v1',
      JSON.stringify({ request_id: id, source_profile_id: source.id, name: created.name }))
    wrapper = mount(MultiseatProfileCreate, { global: spacesGlobal, props: { profiles: [source], ready: true, refresh } })
    await flushPromises()
    expect(wrapper.find('form').exists()).toBe(false)
    expect(fetch).not.toHaveBeenCalled()
  })

  it('ignores invalid saved requests and never submits them', async () => {
    sessionStorage.setItem('polaris:spaces:create-request:v1', JSON.stringify({ request_id: 'invalid', family: 'steam', name: created.name }))
    wrapper = mount(MultiseatProfileCreate, { global: spacesGlobal, props: { profiles: [source], ready: true, refresh } })
    await flushPromises()
    expect(wrapper.find('form').exists()).toBe(false)
    expect(fetch).not.toHaveBeenCalled()
  })
})
