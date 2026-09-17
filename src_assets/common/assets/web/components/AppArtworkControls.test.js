import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import AppArtworkControls from './AppArtworkControls.vue'
import { artworkLookupOffSet } from '../app-artwork.js'
import { spacesGlobal } from './spaces-test-i18n.js'

const app = { name: 'Low Res Desktop', uuid: 'F727EEEE-A124-040A-6D03-33DF1E45E189' }
const reply = (body, status = 200) => ({ ok: status < 300, status, json: async () => body })
const dialog = () => document.body.querySelector('[role="dialog"]')
let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals() })
function start(props = {}) {
  wrapper = mount(AppArtworkControls, { attachTo: document.body, global: spacesGlobal, props: { app, ...props } })
  return wrapper
}
async function openAndConfirm() {
  await wrapper.get('[data-app-artwork-remove]').trigger('click')
  await flushPromises()
  dialog().querySelector('[data-confirm-confirm]').click()
  await flushPromises()
}

describe('artwork controls in the app editor', () => {
  it('asks before removing, sends only the uuid, and reports lookup as off', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply({ status: true, uuid: app.uuid, automatic_lookup: false })))
    start()
    expect(wrapper.get('[data-app-artwork-state]').text()).toContain('Polaris looks up posters')
    await wrapper.get('[data-app-artwork-remove]').trigger('click')
    await flushPromises()
    expect(fetch).not.toHaveBeenCalled()
    expect(dialog().textContent).toContain('Remove the artwork for Low Res Desktop?')
    expect(dialog().textContent).toContain('Pictures picked in Nova for this entry are deleted too.')
    expect(dialog().textContent).toContain('It does not bring the deleted pictures back.')
    dialog().querySelector('[data-confirm-confirm]').click()
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(1)
    const [url, init] = fetch.mock.calls[0]
    expect(url).toBe('./api/apps/artwork/remove')
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body)).toEqual({ uuid: app.uuid })
    expect(wrapper.emitted('changed').at(-1)).toEqual([{ uuid: app.uuid, lookupOff: true }])
    expect(dialog()).toBeNull()
  })

  it('keeps the dialog open with the host sentence when removal is incomplete', async () => {
    const error = 'Polaris stopped looking up artwork for this entry, but some pictures could not be deleted.'
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply({ status: false, uuid: app.uuid, automatic_lookup: false, error })))
    start()
    await openAndConfirm()
    expect(dialog().textContent).toContain(error)
    expect(wrapper.emitted('changed').at(-1)).toEqual([{ uuid: app.uuid, lookupOff: true }])
  })

  it('never changes the state on a request that did not reach the host', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new Error('offline')))
    start()
    await openAndConfirm()
    expect(dialog().textContent).toContain('Polaris could not remove the artwork. Try again.')
    expect(wrapper.emitted('changed')).toBeUndefined()
  })

  it('offers Find artwork again once lookup is off, without a confirmation', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(reply({ status: true, uuid: app.uuid, automatic_lookup: true })))
    start({ lookupOff: true })
    expect(wrapper.find('[data-app-artwork-remove]').exists()).toBe(false)
    expect(wrapper.get('[data-app-artwork-state]').text()).toContain('Polaris does not look up artwork for Low Res Desktop.')
    await wrapper.get('[data-app-artwork-find]').trigger('click')
    await flushPromises()
    expect(dialog()).toBeNull()
    expect(fetch.mock.calls[0][0]).toBe('./api/apps/artwork/find')
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ uuid: app.uuid })
    expect(wrapper.emitted('changed').at(-1)).toEqual([{ uuid: app.uuid, lookupOff: false }])
  })

  it('reads the lookup state from the app list and ignores anything malformed', () => {
    expect(artworkLookupOffSet({ artwork_lookup_off: [app.uuid, 7, null, ''] })).toEqual(new Set([app.uuid]))
    expect(artworkLookupOffSet({ artwork_lookup_off: 'nope' })).toEqual(new Set())
    expect(artworkLookupOffSet({})).toEqual(new Set())
    expect(artworkLookupOffSet(null)).toEqual(new Set())
  })
})
