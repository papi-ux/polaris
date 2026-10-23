import { mount } from '@vue/test-utils'
import { reactive, ref } from 'vue'
import { afterEach, describe, expect, it, vi } from 'vitest'

// The composable is unit-tested on its own; here we mock it to drive the panel's UI logic.
const h = vi.hoisted(() => ({ api: null }))
vi.mock('../composables/useEpicSession.js', () => ({ useEpicSession: () => h.api }))

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

let wrapper
afterEach(() => { wrapper?.unmount() })

describe('EpicSessionControls', () => {
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
