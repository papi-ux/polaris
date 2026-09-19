import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import SpaceRuntimeMove from './SpaceRuntimeMove.vue'
import SpacesList from './SpacesList.vue'
import { validSnapshot } from '../spaces-access.js'
import { spacesGlobal } from './spaces-test-i18n.js'

let wrapper
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/u
const target = (installed = false) => ({ available: true, runtime_id: 'steam-nvidia-cb1dd831b125de6c', nvidia_driver: '615.71.09',
  installed, code: installed ? 'runtime_ready' : 'not_downloaded' })
const mismatched = (move = target()) => ({ id: 'space-a', name: 'Alex', clients: ['handheld'], steam: true, archived: false,
  runtime_driver: '610.57.04', runtime_id: '', host_driver: '615.71.09', runtime_mismatch: true, runtime_move: move })
const current = () => ({ ...mismatched(), runtime_driver: '615.71.09', runtime_id: 'steam-nvidia-cb1dd831b125de6c',
  runtime_mismatch: false, runtime_move: null })
const job = (state, extra = {}) => ({ request_id: '12345678-1234-4234-8234-123456789abc', profile_id: 'space-a',
  runtime_id: 'steam-nvidia-cb1dd831b125de6c', nvidia_driver: '615.71.09', state, code: state, message: '', action: '', ...extra })
const reply = (body, status = 200) => ({ ok: status < 300, status, json: async () => body })
const dialog = () => document.body.querySelector('[role="dialog"]')

function start(props = {}) {
  wrapper = mount(SpaceRuntimeMove, { attachTo: document.body, global: spacesGlobal,
    props: { space: mismatched(), available: true, ready: true, refresh: async () => true, ...props } })
  return wrapper
}
const button = () => wrapper.find('[data-runtime-move-button]')

afterEach(() => { wrapper?.unmount(); wrapper = null; vi.unstubAllGlobals(); vi.useRealTimers() })

describe('Moving a Space to the runtime for this driver', () => {
  it('says why in plain words and offers the move that keeps the Steam home', () => {
    start()
    expect(wrapper.get('[data-runtime-detail]').text()).toBe('Made for NVIDIA driver 610.57.04. This PC runs 615.71.09.')
    expect(wrapper.text()).toContain('Made For Another NVIDIA Driver')
    expect(wrapper.text()).toContain('Devices cannot open this Space until it moves')
    expect(wrapper.get('[data-runtime-keeps]').text()).toBe('Moving keeps its Steam sign-in, installed games and saves.')
    expect(wrapper.get('[data-runtime-download]').text()).toContain('The download is several gigabytes.')
    expect(button().text()).toBe('Move To The Runtime For Driver 615.71.09')
    expect(button().attributes('aria-label')).toBe('Move Alex to the runtime for NVIDIA driver 615.71.09')
    expect(button().attributes('disabled')).toBeUndefined()
  })

  it('does not mention a download once the runtime is on this PC', async () => {
    start({ space: mismatched(target(true)) })
    expect(wrapper.find('[data-runtime-download]').exists()).toBe(false)
    await button().trigger('click')
    await flushPromises()
    expect(dialog().textContent).not.toContain('several gigabytes')
  })

  it('confirms first, then follows the host through downloading, moving and done', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: true, profile_id: 'space-a', message: 'Moving the Space',
      job: job('downloading') }, 202)))
    const refresh = vi.fn(async () => { await wrapper.setProps({ job: job('downloading') }); return true })
    start({ refresh })
    await button().trigger('click')
    await flushPromises()
    expect(fetch).not.toHaveBeenCalled()
    expect(dialog().textContent).toContain('Move Alex to the runtime for driver 615.71.09?')
    expect(dialog().textContent).toContain('The Steam sign-in, installed games and saves stay')
    expect(dialog().textContent).toContain('Its name, devices and Default Space settings stay')
    expect(dialog().textContent).toContain('Polaris downloads the runtime first, several gigabytes')
    expect(dialog().textContent).toContain('Stop Space streams before making this change')
    expect(dialog().querySelector('[data-confirm-confirm]').textContent.trim()).toBe('Move Space')
    dialog().querySelector('[data-confirm-confirm]').click()
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(1)
    const [url, options] = fetch.mock.calls[0]
    expect(url).toBe('./api/multiseat/profiles/runtime')
    expect(options.method).toBe('POST')
    const body = JSON.parse(options.body)
    expect(Object.keys(body).sort()).toEqual(['profile_id', 'request_id', 'runtime_id'])
    expect(body).toMatchObject({ profile_id: 'space-a', runtime_id: 'steam-nvidia-cb1dd831b125de6c' })
    expect(body.request_id).toMatch(uuid)
    expect(dialog()).toBeNull()
    expect(refresh).toHaveBeenCalled()
    expect(wrapper.get('[data-runtime-progress]').text())
      .toBe('Downloading the runtime for driver 615.71.09. You can leave this page and come back.')
    expect(button().exists()).toBe(false)
    await wrapper.setProps({ job: job('moving') })
    expect(wrapper.get('[data-runtime-progress]').text())
      .toBe('Moving Alex to the runtime for driver 615.71.09. Keep Space streams stopped until it finishes.')
    await wrapper.setProps({ job: job('done', { code: 'space_runtime_moved' }), space: current() })
    expect(wrapper.get('[data-runtime-progress]').text())
      .toBe('Alex now uses the runtime for driver 615.71.09. Its Steam sign-in and games are unchanged.')
    expect(wrapper.find('[data-runtime-detail]').exists()).toBe(false)
    expect(wrapper.find('[role=alert]').exists()).toBe(false)
  })

  it('reads the job back sooner while it runs and stops once it ends', async () => {
    vi.useFakeTimers()
    const refresh = vi.fn(async () => true)
    start({ refresh, job: job('downloading'), pollMs: 1000 })
    await vi.advanceTimersByTimeAsync(3100)
    expect(refresh).toHaveBeenCalledTimes(3)
    await wrapper.setProps({ job: job('failed', { code: 'download_incomplete' }) })
    await vi.advanceTimersByTimeAsync(5000)
    expect(refresh).toHaveBeenCalledTimes(3)
  })

  it('shows each refusal in the console words, and the host words for one it does not know', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ status: false, profile_id: 'space-a', code: 'space_active',
      message: 'This Space is open on a device.', action: 'End that stream, then move the Space.', job: null }, 409)))
    start()
    await button().trigger('click')
    await flushPromises()
    dialog().querySelector('[data-confirm-confirm]').click()
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toBe('This Space is open on a device. End that stream, then move it.')
    expect(dialog()).toBeNull()
    expect(button().exists()).toBe(true)

    fetch.mockImplementation(async () => reply({ status: false, profile_id: 'space-a', code: 'some_new_reason',
      message: 'A newer host refused.', action: 'Do the new thing.' }, 409))
    await button().trigger('click')
    await flushPromises()
    dialog().querySelector('[data-confirm-confirm]').click()
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toBe('A newer host refused. Do the new thing.')

    fetch.mockImplementation(async () => ({ ok: false, status: 502, json: async () => { throw new Error('not json') } }))
    await button().trigger('click')
    await flushPromises()
    dialog().querySelector('[data-confirm-confirm]').click()
    await flushPromises()
    expect(wrapper.get('[role=alert]').text()).toBe('The move did not finish. The Space was not changed.')
  })

  it('names every refusal code the host can send', async () => {
    const { translate } = await import('./spaces-test-i18n.js')
    for (const code of ['space_unknown', 'space_active', 'spaces_streaming', 'spaces_change_running', 'spaces_setup_running',
      'spaces_host_setup_running', 'space_move_running', 'move_request_in_use', 'spaces_admin_unavailable', 'space_runtime_unknown',
      'space_runtime_current', 'space_runtime_not_published', 'space_runtime_changed', 'space_runtime_profile_mismatch',
      'space_runtime_media_mismatch', 'space_runtime_identity_mismatch', 'space_storage_unverified', 'docker_unavailable',
      'download_incomplete', 'download_cancelled', 'runtime_verification_failed', 'runtime_identity_mismatch', 'spaces_stopping',
      'spaces_change_not_saved']) {
      const text = translate(`spaces.runtime_refusal.${code}`)
      expect(text, code).not.toBe(`spaces.runtime_refusal.${code}`)
      expect(text, code).not.toMatch(/[–—]| - /u)
    }
  })

  it('keeps the reason of a failed move while the Space still needs moving', () => {
    start({ space: mismatched(target(true)), job: job('failed', { code: 'download_incomplete',
      message: 'The runtime download did not finish.', action: 'Try the move again.' }) })
    expect(wrapper.get('[role=alert]').text())
      .toBe('The runtime download did not finish, so the Space was not moved. Try again to reuse what was downloaded.')
    expect(button().exists()).toBe(true)
  })

  it('says there is nothing to move to when this build has no runtime for the driver', () => {
    start({ space: { ...mismatched({ available: false, code: 'runtime_not_published' }), host_driver: '620.10.01' } })
    expect(wrapper.get('[data-runtime-unpublished]').text())
      .toBe('This Polaris build has no gaming runtime for driver 620.10.01. Update Polaris, or go back to NVIDIA driver 610.57.04.')
    expect(button().exists()).toBe(false)
  })

  it('waits for streams, another move and a host that cannot move Spaces', async () => {
    start({ locked: true, lockReasonId: 'spaces-stream-lock' })
    expect(button().attributes('disabled')).toBeDefined()
    expect(button().attributes('aria-describedby')).toBe('spaces-stream-lock')
    await wrapper.setProps({ locked: false, job: { ...job('downloading'), profile_id: 'space-b' } })
    expect(button().attributes('disabled')).toBeDefined()
    expect(wrapper.get('[data-runtime-move-other]').text()).toContain('Another Space is moving')
    expect(button().attributes('aria-describedby')).toBe(wrapper.get('[data-runtime-move-other]').attributes('id'))
    expect(wrapper.find('[data-runtime-progress]').exists(), 'another Space\'s progress is not this one\'s').toBe(false)
    await wrapper.setProps({ job: null, available: false })
    expect(button().exists()).toBe(false)
    expect(wrapper.find('[data-runtime-move-unavailable]').exists()).toBe(true)
    await wrapper.setProps({ available: true, ready: false })
    expect(button().attributes('disabled')).toBeDefined()
  })

  it('shows nothing for a Space whose runtime matches, or a move finished before this visit', () => {
    start({ space: current() })
    expect(wrapper.find('[data-runtime-move]').exists()).toBe(false)
    wrapper.unmount()
    start({ space: current(), job: job('done', { code: 'space_runtime_moved' }) })
    expect(wrapper.find('[data-runtime-move]').exists()).toBe(false)
  })

  it('sits on the Space card in the list, which no longer calls the Space ready', () => {
    wrapper = mount(SpacesList, { attachTo: document.body, global: spacesGlobal, props: { profiles: [mismatched(), { ...current(), id: 'space-b', name: 'Sam', clients: [] }],
      manageable: true, ready: true, runtimeMoveAvailable: true, clients: [], activity: [], refresh: async () => true } })
    expect(wrapper.get('[data-space="space-a"] [data-runtime-move-button]').text()).toBe('Move To The Runtime For Driver 615.71.09')
    expect(wrapper.find('[data-space="space-b"] [data-runtime-move]').exists()).toBe(false)
    const badge = space => wrapper.get(`[data-space="${space}"] [role=status]`)
    expect(badge('space-a').text()).toBe('Needs attention')
    expect(badge('space-a').classes()).not.toContain('text-success')
    expect(badge('space-b').text()).toBe('Ready')
  })

  it('reads the runtime fields a host sends and refuses malformed ones', () => {
    const snapshot = profiles => ({ enabled: true, available: true, changing: false, failed: false, profiles })
    expect(validSnapshot(snapshot([mismatched()]))).toBe(true)
    expect(validSnapshot(snapshot([current()]))).toBe(true)
    expect(validSnapshot(snapshot([{ ...mismatched(), runtime_move: { available: false, code: 'runtime_not_published' } }]))).toBe(true)
    expect(validSnapshot({ ...snapshot([mismatched()]), runtime_move_available: true, runtime_move_job: job('moving') })).toBe(true)
    expect(validSnapshot({ ...snapshot([mismatched()]), runtime_move_job: null })).toBe(true)
    // A host from before 1.4.11 sends none of it.
    expect(validSnapshot(snapshot([{ id: 'space-a', name: 'Alex', clients: [] }]))).toBe(true)
    for (const profile of [
      { ...mismatched(), runtime_mismatch: 'yes' },
      { ...mismatched(), runtime_driver: '610.57.04; rm' },
      { ...mismatched(), host_driver: null },
      { ...mismatched(), runtime_move: null },
      { ...mismatched(), runtime_move: { ...target(), runtime_id: '../other' } },
      { ...mismatched(), runtime_move: { ...target(), installed: 'no' } },
      { ...current(), runtime_move: target() },
    ]) expect(validSnapshot(snapshot([profile]))).toBe(false)
    expect(validSnapshot({ ...snapshot([mismatched()]), runtime_move_available: 'yes' })).toBe(false)
    expect(validSnapshot({ ...snapshot([mismatched()]), runtime_move_job: job('paused') })).toBe(false)
    expect(validSnapshot({ ...snapshot([mismatched()]), runtime_move_job: { ...job('moving'), nvidia_driver: 'latest' } })).toBe(false)
  })
})
