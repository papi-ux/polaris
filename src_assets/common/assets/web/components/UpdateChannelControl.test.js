import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import UpdateChannelControl from './UpdateChannelControl.vue'

const revision = 'a'.repeat(64)
const response = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body })
const mountControl = (props = {}) => mount(UpdateChannelControl, {
  props: { revision, ...props }, global: { mocks: { $t: key => key } },
})
afterEach(() => vi.unstubAllGlobals())

describe('Update Center beta opt-in', () => {
  it('saves only the existing preference with the observed configuration revision', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(200, { status: true })))
    const changed = vi.fn()
    window.addEventListener('polaris-update-channel-changed', changed)
    const wrapper = mountControl()
    expect(wrapper.get('input').element.checked).toBe(false)
    await wrapper.get('input').setValue(true)
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(1)
    const [url, options] = fetch.mock.calls[0]
    expect(url).toBe('./api/config')
    expect(options).toMatchObject({ method: 'PATCH', credentials: 'include', headers: { 'If-Match': `"${revision}"` } })
    expect(JSON.parse(options.body)).toEqual({ notify_pre_releases: 'enabled' })
    expect(wrapper.emitted('saved')).toEqual([[true]])
    expect(changed).toHaveBeenCalledOnce()
    await wrapper.setProps({ enabled: true })
    expect(wrapper.get('input').element.checked).toBe(true)
    window.removeEventListener('polaris-update-channel-changed', changed)
    wrapper.unmount()
  })

  it('opts out through the same preference without sending an install or restart request', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(200, { status: true })))
    const wrapper = mountControl({ enabled: true })
    await wrapper.get('input').setValue(false)
    await flushPromises()
    expect(fetch).toHaveBeenCalledOnce()
    expect(JSON.parse(fetch.mock.calls[0][1].body)).toEqual({ notify_pre_releases: 'disabled' })
    expect(wrapper.emitted('saved')).toEqual([[false]])
    wrapper.unmount()
  })

  it.each([401, 412, 500])('retains the observed selection after an HTTP %s rejection', async status => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(status, { status: false })))
    const wrapper = mountControl()
    await wrapper.get('input').setValue(true)
    await flushPromises()
    expect(wrapper.get('input').element.checked).toBe(false)
    expect(wrapper.emitted('saved')).toBeUndefined()
    expect(wrapper.emitted('refresh')).toHaveLength(1)
    expect(wrapper.get('[role="alert"]').text()).toContain(status === 412 ? 'update_channel_changed' : 'update_channel_failed')
    wrapper.unmount()
  })

  it('rechecks an uncertain save instead of claiming success', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('connection lost')))
    const wrapper = mountControl({ enabled: true })
    await wrapper.get('input').setValue(false)
    await flushPromises()
    expect(wrapper.get('input').element.checked).toBe(true)
    expect(wrapper.emitted('saved')).toBeUndefined()
    expect(wrapper.emitted('refresh')).toHaveLength(1)
    wrapper.unmount()
  })

  it('blocks input until settings load and while a save is outstanding', async () => {
    let resolve
    vi.stubGlobal('fetch', vi.fn(() => new Promise(done => { resolve = done })))
    const wrapper = mountControl({ revision: '' })
    expect(wrapper.get('input').element.disabled).toBe(true)
    await wrapper.setProps({ revision })
    await wrapper.get('input').setValue(true)
    expect(wrapper.get('input').element.disabled).toBe(true)
    await wrapper.get('input').trigger('change')
    expect(fetch).toHaveBeenCalledOnce()
    resolve(response(200, { status: true }))
    await flushPromises()
    expect(wrapper.get('input').element.disabled).toBe(false)
    wrapper.unmount()
  })
})
