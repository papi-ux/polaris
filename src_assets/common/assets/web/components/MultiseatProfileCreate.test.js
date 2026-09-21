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

  describe('the first Space of a launcher', () => {
    const steam = { family: 'steam', has_space: true, installed: true, runtime_id: '' }
    const heroic = { family: 'heroic', has_space: false, installed: false, runtime_id: 'heroic-nvidia-host' }
    const job = (state, extra = {}) => ({ kind: 'create', request_id: id, profile_id: '', runtime_id: heroic.runtime_id,
      nvidia_driver: '', state, code: state, message: '', action: '', family: 'heroic', name: 'Player 2', ...extra })
    const heroicSpace = { id, name: 'Player 2', family: 'heroic', steam: false, clients: [] }

    it('offers a launcher this PC has no Space for yet, and says what creating one will do', async () => {
      await open({ launchers: [steam, heroic] })
      expect(wrapper.findAll('option').map(item => item.text())).toEqual(['Steam', 'Heroic'])
      expect(wrapper.find('[data-first-of-launcher]').exists()).toBe(false)
      await wrapper.get('select').setValue('heroic')
      expect(wrapper.get('[data-first-of-launcher]').text()).toBe('This will be the first Heroic Space on this PC. ' +
        'Polaris downloads the Heroic gaming runtime first, a few gigabytes, and then creates the Space.')
      await wrapper.setProps({ launchers: [steam, { ...heroic, installed: true }] })
      expect(wrapper.get('[data-first-of-launcher]').text()).toBe('This will be the first Heroic Space on this PC. ' +
        'Its gaming runtime is already downloaded.')
    })

    it('follows the host through the download and the creation, then confirms from the saved catalog', async () => {
      vi.useFakeTimers()
      fetch.mockResolvedValueOnce(reply({ status: false, profile_id: id, code: '', message: 'Creating the Space', job: job('downloading') }, 202))
      refresh.mockImplementation(async () => { await wrapper.setProps({ job: job('downloading') }); return true })
      await open({ launchers: [steam, heroic], pollMs: 1000 })
      await wrapper.get('select').setValue('heroic')
      await wrapper.get('form').trigger('submit')
      await flushPromises()
      expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ request_id: id, family: 'heroic', name: 'Player 2' })
      expect(wrapper.get('[data-create-progress]').text())
        .toBe('Downloading the Heroic gaming runtime. You can leave this page and come back.')
      // Nothing to press while the host works: no retry, no status check, no half-true "still creating".
      expect(wrapper.text()).not.toContain('still creating')
      expect(button('Check creation status')).toBeUndefined()
      expect(wrapper.get('button[type=submit]').element.disabled).toBe(true)
      // It reads the job back on its own, sooner than the page would.
      const before = refresh.mock.calls.length
      await vi.advanceTimersByTimeAsync(2100)
      expect(refresh.mock.calls.length).toBe(before + 2)
      await wrapper.setProps({ job: job('creating') })
      expect(wrapper.get('[data-create-progress]').text()).toBe('Creating Player 2.')
      await wrapper.setProps({ job: job('done', { code: 'space_created' }), profiles: [source, heroicSpace] })
      await flushPromises()
      expect(wrapper.text()).toContain('Player 2 was created')
      expect(wrapper.find('form').exists()).toBe(false)
      expect(fetch).toHaveBeenCalledTimes(1)
      const after = refresh.mock.calls.length
      await vi.advanceTimersByTimeAsync(3000)
      expect(refresh.mock.calls.length).toBe(after)
      vi.useRealTimers()
    })

    it('says why a download failed and retries with the same request', async () => {
      fetch.mockResolvedValue(reply({ status: false, profile_id: id, job: job('downloading') }, 202))
      refresh.mockImplementation(async () => { await wrapper.setProps({ job: job('downloading') }); return true })
      await open({ launchers: [steam, heroic] })
      await wrapper.get('select').setValue('heroic')
      await wrapper.get('form').trigger('submit')
      await flushPromises()
      refresh.mockImplementation(async () => true)
      await wrapper.setProps({ job: job('failed', { code: 'download_incomplete',
        message: 'The runtime download did not finish. The Space was not created.', action: 'Create the Space again.' }) })
      expect(wrapper.get('[role=alert]').text())
        .toBe('The runtime download did not finish. The Space was not created. Create the Space again.')
      expect(wrapper.find('[data-create-progress]').exists()).toBe(false)
      expect(button('Retry creation').element.disabled).toBe(false)
      await wrapper.get('form').trigger('submit')
      await flushPromises()
      expect(fetch).toHaveBeenCalledTimes(2)
      expect(fetch.mock.calls[1][1].body).toBe(fetch.mock.calls[0][1].body)
      expect(crypto.randomUUID).toHaveBeenCalledTimes(1)
      // This retry failed at once: the job was failed before it and is failed after it, so
      // nothing changed for a watcher to see. The host's reason is still what the form says.
      expect(wrapper.get('[role=alert]').text())
        .toBe('The runtime download did not finish. The Space was not created. Create the Space again.')
      expect(wrapper.text()).not.toContain('has not been confirmed')
    })

    it('lets a failed download be abandoned, but not a Space that may be half made', async () => {
      fetch.mockResolvedValue(reply({ status: false, profile_id: id, job: job('downloading') }, 202))
      refresh.mockImplementation(async () => { await wrapper.setProps({ job: job('downloading') }); return true })
      await open({ launchers: [steam, heroic] })
      await wrapper.get('select').setValue('heroic')
      await wrapper.get('form').trigger('submit')
      await flushPromises()
      expect(wrapper.find('[data-start-over]').exists()).toBe(false)
      refresh.mockImplementation(async () => true)
      // Making the Space itself failed in a way that may have left something behind: the
      // request is kept, because only the same request can confirm what happened to it.
      await wrapper.setProps({ job: job('failed', { code: 'spaces_change_not_saved', message: 'The Space was not created.' }) })
      expect(wrapper.find('[data-start-over]').exists()).toBe(false)
      expect(wrapper.get('input').element.disabled).toBe(true)
      // A download that failed made nothing at all.
      await wrapper.setProps({ job: job('failed', { code: 'download_incomplete', message: 'The runtime download did not finish.' }) })
      await wrapper.get('[data-start-over]').trigger('click')
      expect(wrapper.find('[role=alert]').exists()).toBe(false)
      expect(wrapper.get('input').element.disabled).toBe(false)
      expect(wrapper.get('select').element.disabled).toBe(false)
      expect(sessionStorage.getItem('polaris:spaces:create-request:v1')).toBeNull()
      expect(button('Cancel')).toBeDefined()
      await wrapper.get('form').trigger('submit')
      await flushPromises()
      expect(crypto.randomUUID).toHaveBeenCalledTimes(2)
    })

    it('never has two reads of the job in flight', async () => {
      // The page's loader drops a read that is still running when another starts. On a fixed
      // interval, a host slower than the interval had every read cancelled and the job never moved.
      vi.useFakeTimers()
      fetch.mockResolvedValueOnce(reply({ status: false, profile_id: id, job: job('downloading') }, 202))
      refresh.mockImplementation(async () => { await wrapper.setProps({ job: job('downloading') }); return true })
      await open({ launchers: [steam, heroic], pollMs: 1000 })
      await wrapper.get('select').setValue('heroic')
      await wrapper.get('form').trigger('submit')
      await flushPromises()
      let inFlight = 0, most = 0
      refresh.mockImplementation(() => new Promise(resolve => {
        most = Math.max(most, ++inFlight)
        setTimeout(() => { inFlight--; resolve(true) }, 5000)
      }))
      await vi.advanceTimersByTimeAsync(20000)
      expect(most).toBe(1)
      vi.useRealTimers()
    })

    it('ignores a job that belongs to another request', async () => {
      await open({ launchers: [steam, heroic], job: job('downloading', { request_id: '99999999-1234-4234-8234-123456789abc' }) })
      expect(wrapper.find('[data-create-progress]').exists()).toBe(false)
      expect(wrapper.get('button[type=submit]').element.disabled).toBe(false)
    })
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
