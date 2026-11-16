import { mount } from '@vue/test-utils'
import { reactive, ref } from 'vue'
import { afterEach, describe, expect, it, vi } from 'vitest'

// The composable is unit-tested on its own; here we mock it to drive the panel's UI logic.
// A test that leaves h.api null drives the panel through the real composable instead.
const h = vi.hoisted(() => ({ api: null }))
vi.mock('../composables/useEpicSession.js', async importOriginal => {
  const actual = await importOriginal()
  return { useEpicSession: options => h.api ?? actual.useEpicSession(options) }
})

import EpicSessionControls from './EpicSessionControls.vue'

function makeApi(stateOverrides = {}) {
  return {
    state: reactive({
      available: true, app_uuid: 'u1', app_name: 'Alan Wake 2', session_id: 's1', revision: 'r1',
      state: 'confirmable', runtime: 'proton', can_adopt: true, message: '', ...stateOverrides,
    }),
    loading: ref(false), loadError: ref(''), actionError: ref(''), acting: ref(false), loaded: ref(true),
    load: vi.fn(), adopt: vi.fn().mockResolvedValue(true), release: vi.fn().mockResolvedValue(true),
    start: vi.fn(), stop: vi.fn(),
  }
}

const reply = (body, ok = true, status = ok ? 200 : 409) => ({ ok, status, json: async () => body })
const active = extra => ({
  status: true, version: 1, available: true, app_uuid: 'u1', app_name: 'Alan Wake 2',
  session_id: 's1', revision: 's1:a', state: 'confirmable', runtime: 'proton', can_adopt: true, message: '', ...extra,
})
const unverified = { state: 'unavailable', revision: 's1:', can_adopt: false }

let wrapper
afterEach(() => { wrapper?.unmount(); h.api = null; vi.unstubAllGlobals(); vi.useRealTimers() })

describe('EpicSessionControls', () => {
  it('explains Flatpak stream End without offering game adoption or close permission', () => {
    h.api = makeApi({ state: 'unavailable', code: 'epic_flatpak_game_control_unavailable', runtime: 'unknown',
      can_adopt: false, can_release: false })
    wrapper = mount(EpicSessionControls)
    const text = wrapper.get('[data-epic-flatpak-limit]').text()
    expect(text).toContain('Mirror Desktop only')
    expect(text).toContain('End Session ends the stream and leaves the desktop game and launcher running')
    expect(text).toContain('Disconnect and Resume leave them running too')
    expect(wrapper.find('[data-epic-adopt-start]').exists()).toBe(false)
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(false)
    expect(wrapper.find('[data-epic-release]').exists()).toBe(false)
    expect(h.api.adopt).not.toHaveBeenCalled()
    expect(h.api.release).not.toHaveBeenCalled()
  })
  it('drops Mirror consent for a private session and explains End without consent controls', async () => {
    h.api = makeApi()
    wrapper = mount(EpicSessionControls)
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    Object.assign(h.api.state, {
      state: 'session_managed', game_state: 'waiting', can_adopt: true, can_release: true,
      message: 'End closes the whole private session. Cloud-sync completion is not known.',
    })
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(false)
    expect(wrapper.find('[data-epic-adopt-start]').exists()).toBe(false)
    expect(wrapper.find('[data-epic-release]').exists()).toBe(false)
    expect(wrapper.find('[data-epic-unmanaged]').exists()).toBe(false)
    expect(wrapper.text()).toContain('Private Stream')
    expect(wrapper.text()).not.toContain('Mirror Desktop')
    expect(wrapper.get('[data-epic-private-session]').text()).toContain('including Epic')
    expect(wrapper.get('[data-epic-private-details]').text()).toContain('Cloud-sync completion is not known')
    expect(wrapper.get('[data-epic-state]').text()).toBe('Waiting for game')
    h.api.state.game_state = 'managed'
    await wrapper.vm.$nextTick()
    expect(wrapper.get('[data-epic-state]').text()).toBe('Session-managed')
    h.api.state.game_state = 'unavailable'
    await wrapper.vm.$nextTick()
    expect(wrapper.get('[data-epic-state]').text()).toBe('Game unverified')
    expect(wrapper.get('[data-epic-private-unavailable]').text()).toContain('End still closes')
    expect(h.api.adopt).not.toHaveBeenCalled()
    expect(h.api.release).not.toHaveBeenCalled()
  })

  it('renders nothing when there is no active Epic session', () => {
    h.api = makeApi({ available: false, state: '' })
    wrapper = mount(EpicSessionControls)
    expect(wrapper.find('[data-epic-session-controls]').exists()).toBe(false)
  })

  it('starts polling on mount and shows the unmanaged default with a two-step adopt entry', () => {
    h.api = makeApi()
    wrapper = mount(EpicSessionControls)
    expect(h.api.start).toHaveBeenCalled()
    expect(wrapper.find('[data-epic-unmanaged]').exists()).toBe(true)
    expect(wrapper.find('[data-epic-adopt-start]').exists()).toBe(true)
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(false)
  })

  it('cancelling the two-step consent sends no request', async () => {
    h.api = makeApi()
    wrapper = mount(EpicSessionControls)
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(true)
    await wrapper.get('[data-epic-adopt-cancel]').trigger('click')
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(false)
    expect(h.api.adopt).not.toHaveBeenCalled()
  })

  it('confirming calls adopt exactly once', async () => {
    h.api = makeApi()
    wrapper = mount(EpicSessionControls)
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    await wrapper.get('[data-epic-adopt-confirm]').trigger('click')
    expect(h.api.adopt).toHaveBeenCalledTimes(1)
  })

  it('discloses partial inspection before consent and resets confirmation with its revision', async () => {
    h.api = makeApi({ uninspected_process_count: 3 })
    wrapper = mount(EpicSessionControls)
    expect(wrapper.get('[data-epic-inspection-limit]').text()).toContain('3 processes could not be inspected')
    expect(wrapper.get('[data-epic-inspection-limit]').text()).toContain('only to this verified game')
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(true)
    h.api.state.uninspected_process_count = 4
    h.api.state.revision = 'changed-inspection-context'
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(false)
    expect(h.api.adopt).not.toHaveBeenCalled()
  })

  it('does not offer adoption when the game is not confirmable/adoptable', () => {
    h.api = makeApi({ state: 'unmanaged', can_adopt: false })
    wrapper = mount(EpicSessionControls)
    expect(wrapper.find('[data-epic-unmanaged]').exists()).toBe(true)
    expect(wrapper.find('[data-epic-adopt-start]').exists()).toBe(false)
  })

  it('requires a fresh two-step confirmation when the observed game changes', async () => {
    h.api = makeApi()
    wrapper = mount(EpicSessionControls)
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    const previousConfirm = wrapper.get('[data-epic-adopt-confirm]')
    h.api.state.revision = 'new-game'
    await previousConfirm.trigger('click')
    expect(h.api.adopt).not.toHaveBeenCalled()
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(false)
    expect(wrapper.find('[data-epic-consent-reset]').exists()).toBe(true)
  })

  it('holds consent across polls, pauses it while unverified and posts only the same verified revision', async () => {
    vi.useFakeTimers()
    const gets = [active(), active(), active(unverified), active()]
    const fetchMock = vi.fn(async (_url, opt) => (opt?.method === 'POST'
      ? reply({ status: true })
      : reply(gets.length > 1 ? gets.shift() : gets[0])))
    vi.stubGlobal('fetch', fetchMock)
    wrapper = mount(EpicSessionControls) // real composable
    await vi.advanceTimersByTimeAsync(0)
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    await vi.advanceTimersByTimeAsync(5000) // the same verified revision
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(true)
    expect(wrapper.get('[data-epic-adopt-confirm]').attributes('disabled')).toBeUndefined()
    await vi.advanceTimersByTimeAsync(5000) // an inconclusive census
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(true)
    expect(wrapper.find('[data-epic-consent-paused]').exists()).toBe(true)
    expect(wrapper.get('[data-epic-adopt-confirm]').attributes('disabled')).toBeDefined()
    await wrapper.get('[data-epic-adopt-confirm]').trigger('click')
    expect(fetchMock.mock.calls.filter(c => c[1]?.method === 'POST')).toHaveLength(0)
    await vi.advanceTimersByTimeAsync(5000) // the same verified revision returns
    expect(wrapper.find('[data-epic-consent-paused]').exists()).toBe(false)
    await wrapper.get('[data-epic-adopt-confirm]').trigger('click')
    await vi.advanceTimersByTimeAsync(0)
    const sent = fetchMock.mock.calls.filter(c => c[1]?.method === 'POST')
    expect(sent).toHaveLength(1)
    expect(JSON.parse(sent[0][1].body)).toEqual({ action: 'adopt', session_id: 's1', revision: 's1:a', confirm: true })
  })

  it('pauses consent while the status cannot be read', async () => {
    h.api = makeApi()
    wrapper = mount(EpicSessionControls)
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    h.api.loadError.value = 'Epic session status is not available right now.'
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-epic-consent-paused]').exists()).toBe(true)
    expect(wrapper.get('[data-epic-adopt-confirm]').attributes('disabled')).toBeDefined()
    h.api.loadError.value = ''
    await wrapper.vm.$nextTick()
    await wrapper.get('[data-epic-adopt-confirm]').trigger('click')
    expect(h.api.adopt).toHaveBeenCalledOnce()
  })

  it.each([
    ['revision', { revision: 'r2' }],
    ['session', { session_id: 's2', revision: 's2:a' }],
  ])('a different verified %s after a pause visibly requires a fresh confirmation', async (_kind, next) => {
    h.api = makeApi()
    wrapper = mount(EpicSessionControls)
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    Object.assign(h.api.state, unverified)
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-epic-consent-paused]').exists()).toBe(true)
    Object.assign(h.api.state, { state: 'confirmable', can_adopt: true, ...next })
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-epic-adopt-consent]').exists()).toBe(false)
    expect(wrapper.find('[data-epic-consent-reset]').exists()).toBe(true)
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    expect(wrapper.find('[data-epic-consent-reset]').exists()).toBe(false)
    await wrapper.get('[data-epic-adopt-confirm]').trigger('click')
    expect(h.api.adopt).toHaveBeenCalledOnce()
  })

  it('labels the grant as permission to close on End Session and says where End is', async () => {
    h.api = makeApi()
    wrapper = mount(EpicSessionControls)
    expect(wrapper.get('[data-epic-adopt-start]').text()).toContain('End Session')
    await wrapper.get('[data-epic-adopt-start]').trigger('click')
    const consent = wrapper.get('[data-epic-adopt-consent]').text()
    expect(consent).toContain('does not close the game now')
    expect(consent).toContain('End Session in your streaming client')
    expect(consent).toContain('Disconnect Client')
    expect(consent).toContain('Disconnect and Resume in your streaming client keep the game running')
    h.api.state.state = 'managed'
    await wrapper.vm.$nextTick()
    expect(wrapper.text()).not.toContain('Disconnect leaves it running')
    expect(wrapper.text()).toContain('Disconnect Client here in Mission Control')
  })

  it('can clear retained permission when the current game cannot be verified', async () => {
    h.api = makeApi({ state: 'unavailable', can_adopt: false, can_release: true })
    wrapper = mount(EpicSessionControls)
    expect(wrapper.text()).not.toContain('It is still running')
    await wrapper.get('[data-epic-release]').trigger('click')
    expect(h.api.release).toHaveBeenCalledOnce()
  })

  it('shows a managed game with a leave-running (release) action', async () => {
    h.api = makeApi({ state: 'managed' })
    wrapper = mount(EpicSessionControls)
    expect(wrapper.find('[data-epic-release]').exists()).toBe(true)
    await wrapper.get('[data-epic-release]').trigger('click')
    expect(h.api.release).toHaveBeenCalledTimes(1)
  })

  it('shows an unreadable active game honestly, with no adopt control', () => {
    h.api = makeApi({ state: 'unavailable', can_adopt: false })
    wrapper = mount(EpicSessionControls)
    expect(wrapper.find('[data-epic-unavailable]').exists()).toBe(true)
    expect(wrapper.find('[data-epic-adopt-start]').exists()).toBe(false)
  })

  it('surfaces a refused action error', () => {
    h.api = makeApi()
    h.api.actionError.value = 'Session moved on.'
    wrapper = mount(EpicSessionControls)
    expect(wrapper.get('[data-epic-action-error]').text()).toContain('Session moved on.')
  })
})
