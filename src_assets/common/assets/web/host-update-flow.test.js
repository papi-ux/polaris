import { afterEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import HostUpdateFlow from './components/HostUpdateFlow.vue'
import { buildUpdateCenterState, describeHostUpdate } from './update-center.js'

const installed = (version = '1.5.0') => ({ polaris: { state: 'installed', version }, 'polaris-kms': { state: 'installed', version } })
const plan = () => ({ route: 'helper', review_id: '12345678-1234-4234-8234-123456789abc', channel: 'stable', version: '1.5.1', update_needed: true,
  release_url: 'https://github.com/papi-ux/polaris/releases/tag/v1.5.1', assets: [
    { package: 'polaris', id: 10, name: 'Polaris-fedora44-x86_64.rpm', digest: `sha256:${'a'.repeat(64)}`, url: 'https://github.com/papi-ux/polaris/releases/download/v1.5.1/Polaris-fedora44-x86_64.rpm' },
    { package: 'polaris-kms', id: 11, name: 'Polaris-kms-fedora44-x86_64.rpm', digest: `sha256:${'b'.repeat(64)}`, url: 'https://github.com/papi-ux/polaris/releases/download/v1.5.1/Polaris-kms-fedora44-x86_64.rpm' },
  ] })
const snapshot = (job = null, current = installed()) => ({ available: true, finish_available: true, busy: { blocked: false, reasons: [] },
  running_version: '1.5.0', installed: current, job })

describe('Host update journey', () => {
  afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.useRealTimers() })

  it('uses the host-selected release even when browser metadata selects a different release', () => {
    const state = buildUpdateCenterState({ currentVersion: '1.5.0', hostPlan: plan(), latestRelease: { tag_name: 'v9.0.0', assets: [] } })
    expect(state.latestVersion).toBe('1.5.1')
    expect(state.primaryActionKind).toBe('review_host_update')
    expect(state.installCommand).toBe('')
    expect(state.statusTone).toBe('update')
    expect(state.asset.name).toBe('Polaris-fedora44-x86_64.rpm')
    expect(state.kmsAsset.id).toBe(11)
    const journey = describeHostUpdate(snapshot({ state: 'installed' }, installed('1.5.1')))
    const headline = buildUpdateCenterState({ currentVersion: '1.5.0', hostPlan: plan(), hostJourney: journey })
    expect(headline.statusLabel).toBe('Polaris 1.5.1 is installed')
    expect(headline.primaryActionKind).toBe('scroll_to_update_center')
  })

  it('covers review, approval, progress, cancelled, failed, installed, Finish and reconnect states', () => {
    expect(describeHostUpdate(snapshot(), plan(), { reviewing: true }).canInstall).toBe(true)
    for (const state of ['waiting_for_approval', 'running', 'restarting']) {
      const view = describeHostUpdate(snapshot({ state, request_id: 'one' }), plan(), { reviewing: true })
      expect(view.canInstall).toBe(false)
      expect(view.canFinish).toBe(false)
      expect(view.canCancel).toBe(state === 'waiting_for_approval')
    }
    expect(describeHostUpdate(snapshot({ state: 'cancelled' })).title).toBe('Approval cancelled')
    expect(describeHostUpdate(snapshot({ state: 'timed_out' })).title).toBe('Approval timed out')
    expect(describeHostUpdate(snapshot({ state: 'failed', check: { transaction_attempted: false }, message: 'Digest changed.' })).message).toContain('no packages were changed')
    const afterTransaction = describeHostUpdate(snapshot({ state: 'failed', check: { transaction_attempted: true }, message: 'RPM stopped.' },
      { polaris: { state: 'installed', version: '1.5.1' }, 'polaris-kms': { state: 'partial' } }))
    expect(afterTransaction.message).not.toContain('no packages were changed')
    expect(afterTransaction.installed[1].state).toBe('partial')
    const done = snapshot({ state: 'installed' }, installed('1.5.1'))
    expect(describeHostUpdate(done).canFinish).toBe(true)
    expect(describeHostUpdate({ ...done, finish_available: false, busy: { reasons: ['A game is paused for resume.'] } }).canFinish).toBe(false)
    expect(describeHostUpdate(done, null, { disconnected: true }).canFinish).toBe(false)
    expect(describeHostUpdate(done, null, { disconnected: true }).title).toBe('Connection lost')
    expect(describeHostUpdate(done, null, { restarting: true, restartTimedOut: true }).message).toContain('Check the PC')
    expect(describeHostUpdate({ ...done, running_version: '1.5.1' }).finished).toBe(true)
  })

  it('keeps missing/unknown/absent readback distinct and prefers fresh database readback over the journal', () => {
    const view = describeHostUpdate(snapshot({ state: 'failed', check: { installed: installed('1.5.1') } },
      { polaris: { state: 'unknown' }, 'polaris-kms': { state: 'absent' } }))
    expect(view.installed.map((entry) => entry.state)).toEqual(['unknown', 'absent'])
    expect(view.canFinish).toBe(false)
    const mixed = describeHostUpdate({ ...snapshot({ state: 'installed' }, { polaris: { state: 'installed', version: '1.5.0' }, 'polaris-kms': { state: 'partial' } }) })
    expect(mixed.finished).toBeFalsy()
    expect(mixed.title).toContain('needs attention')
  })

  it('requires a new review after failure and exposes the paused-game reason before install', () => {
    const value = snapshot({ state: 'failed', message: 'Release changed.' })
    expect(describeHostUpdate(value, plan()).canInstall).toBe(false)
    expect(describeHostUpdate(value, plan(), { reviewing: true }).title).toBe('Review Polaris 1.5.1')
    value.available = false
    value.busy = { blocked: true, reasons: ['A game is paused for resume on this PC.'] }
    const view = describeHostUpdate(value, plan(), { reviewing: true })
    expect(view.canInstall).toBe(false)
    expect(view.busyReason).toContain('paused for resume')
  })

  it('mounts the review/approval journey, sends only the nonce and closed fields, and never requests /api/restart', async () => {
    vi.useFakeTimers()
    document.head.innerHTML = '<meta name="csrf-token" content="test-csrf">'
    let job = snapshot()
    const calls = []
    vi.stubGlobal('fetch', vi.fn(async (url, options = {}) => {
      calls.push({ url, options })
      if (url.includes('/plan?')) return { ok: true, json: async () => plan() }
      if (options.method === 'POST') {
        job = { ...snapshot({ state: 'waiting_for_approval', request_id: JSON.parse(options.body).request_id, message: 'Approve on this PC.' }), accepted: true }
      }
      return { ok: true, json: async () => job }
    }))
    const wrapper = mount(HostUpdateFlow)
    await flushPromises()
    await wrapper.find('button').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('not an independent publisher signature')
    expect(wrapper.text()).toContain('Updates wait until streams and paused games end.')
    expect(wrapper.find('input[type=password]').exists()).toBe(false)
    await wrapper.find('[data-update-install]').trigger('click')
    await flushPromises()
    const mutation = calls.find((call) => call.options.method === 'POST')
    const body = JSON.parse(mutation.options.body)
    expect(Object.keys(body).sort()).toEqual(['action', 'channel', 'request_id', 'review_id', 'version'])
    expect(body.review_id).toBe(plan().review_id)
    expect(mutation.options.headers['X-CSRF-Token']).toBe('test-csrf')
    expect(wrapper.find('[data-update-cancel]').exists()).toBe(true)
    expect(calls.some((call) => call.url === './api/restart')).toBe(false)
    wrapper.unmount()
  })

  it('renders failed mixed readback, obtains a fresh review, and disables Finish while busy', async () => {
    vi.useFakeTimers()
    let value = snapshot({ state: 'failed', message: 'RPM stopped.', check: { transaction_attempted: true } },
      { polaris: { state: 'installed', version: '1.5.1' }, 'polaris-kms': { state: 'partial' } })
    value.finish_available = false
    value.busy = { blocked: true, reasons: ['A game is paused for resume on this PC.'] }
    value.available = false
    let reviews = 0
    vi.stubGlobal('fetch', vi.fn(async (url) => {
      if (url.includes('/plan?')) { reviews++; return { ok: true, json: async () => plan() } }
      return { ok: true, json: async () => value }
    }))
    const wrapper = mount(HostUpdateFlow)
    await flushPromises()
    expect(wrapper.find('[data-update-installed]').text()).toContain('polaris-kms: partial')
    expect(wrapper.find('[data-update-finish]').attributes('disabled')).toBeDefined()
    expect(wrapper.text()).not.toContain('no packages were changed')
    await wrapper.find('button').trigger('click')
    await flushPromises()
    expect(reviews).toBe(2)
    expect(wrapper.find('[data-update-install]').attributes('disabled')).toBeDefined()
    value = { ...snapshot({ state: 'installed' }, installed('1.5.1')), finish_available: false, busy: { blocked: true, reasons: ['A game is paused for resume on this PC.'] } }
    await vi.advanceTimersByTimeAsync(3000)
    await flushPromises()
    expect(wrapper.find('[data-update-busy]').text()).toContain('paused for resume')
    wrapper.unmount()
  })
})
