import { afterEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import HostUpdateFlow from './components/HostUpdateFlow.vue'
import { buildUpdateCenterState, describeHostUpdate, hostUpdatePollDelay, selectedReleaseLabel } from './update-center.js'

const installed = (version = '1.5.0') => ({ polaris: { state: 'installed', version }, 'polaris-kms': { state: 'installed', version } })
const plan = () => ({ route: 'helper', review_id: '12345678-1234-4234-8234-123456789abc', channel: 'stable', version: '1.5.1', update_needed: true,
  release_url: 'https://github.com/papi-ux/polaris/releases/tag/v1.5.1', assets: [
    { package: 'polaris', id: 10, name: 'Polaris-fedora44-x86_64.rpm', digest: `sha256:${'a'.repeat(64)}`, url: 'https://github.com/papi-ux/polaris/releases/download/v1.5.1/Polaris-fedora44-x86_64.rpm' },
    { package: 'polaris-kms', id: 11, name: 'Polaris-kms-fedora44-x86_64.rpm', digest: `sha256:${'b'.repeat(64)}`, url: 'https://github.com/papi-ux/polaris/releases/download/v1.5.1/Polaris-kms-fedora44-x86_64.rpm' },
  ] })
const snapshot = (job = null, current = installed()) => ({ qualified: true, available: true, finish_available: true, busy: { blocked: false, reasons: [] },
  running_version: '1.5.0', installed: current, job })
// The host's own qualification: guided hosts carry no RPM readback and no native update job.
const guided = (extra = {}) => ({ qualified: false, available: false, finish_available: false, busy: { blocked: false, reasons: [] },
  running_version: '1.5.0', job: null, ...extra })
const guidedPlan = (code, message) => ({ route: 'guided', available: false, refusal: { code, message } })
const unqualified = guidedPlan('family_unqualified', 'Polaris can install updates on Fedora 44 x86_64 only. Use the guided steps for this system.')

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

  it('does not let a stale terminal journal hide a newer host-selected release', () => {
    const next = { ...plan(), version: '1.5.2' }
    for (const state of ['installed', 'failed', 'cancelled', 'unknown']) {
      const view = describeHostUpdate({ ...snapshot({ state, version: '1.5.1', message: 'Old result.' }, installed('1.5.1')), running_version: '1.5.1' }, next)
      expect(view.title).toBe('Update available')
      expect(view.hasJob).toBe(false)
      const headline = buildUpdateCenterState({ currentVersion: '1.5.1', hostPlan: next, hostJourney: view })
      expect(headline.status).toBe('update_available')
      expect(headline.primaryActionLabel).toBe('Review update')
      expect(describeHostUpdate({ ...snapshot(null, installed('1.5.1')), running_version: '1.5.1' }, next,
        { finishTarget: '1.5.1' }).finished).toBe(false)
    }
    expect(describeHostUpdate(snapshot({ state: 'running', version: '1.5.1' }), next).active).toBe(true)
    expect(describeHostUpdate(snapshot({ state: 'failed', version: '1.5.2' }), next).title).toBe('Update stopped')
  })

  it('keeps restart-required for external package updates and confirms Finish from fresh state without a journal', () => {
    const current = { ...plan(), update_needed: false }
    const headline = buildUpdateCenterState({ currentVersion: '1.5.0', hostPlan: current,
      latestRelease: { tag_name: 'v1.5.1', assets: [] }, host: { installed_package_version: '1.5.1' } })
    expect(headline.status).toBe('restart_required')
    expect(headline.statusTone).toBe('restart')
    for (const job of [null, { state: 'failed', version: '1.5.0' }, { state: 'installed', version: '1.5.0' }, { state: 'restarting', version: '1.5.1' }]) {
      const fresh = { ...snapshot(job, installed('1.5.1')), running_version: '1.5.1' }
      const view = describeHostUpdate(fresh, current, { restarting: true, finishTarget: '1.5.1' })
      expect(view.finished).toBe(true)
      expect(view.active).toBe(false)
      expect(view.title).toBe('Polaris 1.5.1 is running')
      expect(describeHostUpdate({ ...fresh, installed: { polaris: { state: 'unknown' } } }, current, { finishTarget: '1.5.1' }).finished).toBe(false)
      expect(describeHostUpdate({ ...fresh, installed: undefined, job: { state: 'installed', check: { installed: installed('1.5.1') } } }, current, { finishTarget: '1.5.1' }).finished).toBe(false)
    }
  })

  it('labels selected stable, beta and release candidates and slows idle or timed-out polling', () => {
    expect(selectedReleaseLabel({ channel: 'beta', version: '1.5.1' })).toBe('Stable')
    expect(selectedReleaseLabel({ version: '1.5.1-beta.2' })).toBe('Beta')
    expect(selectedReleaseLabel({ version: '1.5.1-rc.2' })).toBe('Release candidate')
    expect(hostUpdatePollDelay()).toBe(60000)
    expect(hostUpdatePollDelay({ active: true })).toBe(3000)
    expect(hostUpdatePollDelay({ reviewing: true })).toBe(5000)
    expect(hostUpdatePollDelay({ restarting: true, restartTimedOut: true })).toBe(15000)
    expect(hostUpdatePollDelay({ visible: false, active: true })).toBeNull()
    expect(hostUpdatePollDelay({ capable: false })).toBeNull()
  })

  it('separates a confirmed restart from a later offer or active update in another tab', () => {
    const fresh = { ...snapshot(null, installed('1.5.1')), running_version: '1.5.1' }
    const local = { restarting: true, finishTarget: '1.5.1' }
    const next = { ...plan(), version: '1.5.2' }
    const offer = describeHostUpdate(fresh, next, local)
    expect(offer.finishConfirmed).toBe(true)
    expect(offer.active).toBe(false)
    expect(offer.finished).toBe(false)
    expect(offer.hasJob).toBe(false)
    expect(offer.title).toBe('Update available')
    const updating = describeHostUpdate({ ...fresh, job: { state: 'running', version: '1.5.2', check: { stage: 'installing' } } }, plan(), local)
    expect(updating.finishConfirmed).toBe(true)
    expect(updating.active).toBe(true)
    expect(updating.finished).toBe(false)
    expect(updating.title).toBe('Updating this PC')
    expect(updating.message).not.toContain('You can start streams')
  })

  it('does not call an unavailable helper readback up to date or permit Install', () => {
    // helper_update_read's unavailable result has no installed map or release version.
    const value = snapshot({ state: 'failed', version: '', check: { code: 'update_unavailable' } }, {})
    const view = describeHostUpdate(value, { ...plan(), update_needed: false })
    expect(view.needsAttention).toBe(true)
    expect(view.hasJob).toBe(true)
    expect(view.title).toBe('Installed package state needs attention')
    expect(view.installed.every((entry) => entry.state === 'unknown')).toBe(true)
    expect(describeHostUpdate(value, plan(), { reviewing: true }).canInstall).toBe(false)
    expect(view.canFinish).toBe(false)
    const mixed = installed('1.5.1')
    mixed['polaris-kms'].version = '1.5.0'
    expect(describeHostUpdate(snapshot(null, mixed), plan(), { reviewing: true }).canInstall).toBe(true)
  })

  it('keeps guided, image-based, missing-helper and non-RPM hosts on their honest guided headline', () => {
    const unknown = { polaris: { state: 'unknown' }, 'polaris-kms': { state: 'unknown' } }
    const unavailableJob = { state: 'failed', version: '', message: 'Host update information is unavailable. Recheck this PC.',
      check: { state: 'failed', code: 'update_unavailable', message: 'Host update information is unavailable. Recheck this PC.' } }
    const cases = [
      ['Arch/Ubuntu/SteamOS without rpm', guided(), unqualified],
      ['e29 shape: no rpm reads unknown', guided({ installed: unknown }), unqualified],
      ['image based', guided(), guidedPlan('image_based_host', 'This PC runs an image based system, where Polaris does not change host setup. Follow the guide for your system instead.')],
      ['missing helper', guided(), guidedPlan('helper_missing', 'The Polaris update helper is missing. Install the matching native package, then review again.')],
      ['e29 shape: missing helper read became a job', guided({ installed: {}, job: unavailableJob }), unqualified],
      ['non-RPM Fedora', guided({ installed: { polaris: { state: 'absent' }, 'polaris-kms': { state: 'absent' } } }),
        guidedPlan('native_package_absent', 'This Polaris was not installed from the Fedora RPM.')],
    ]
    for (const [name, value, host] of cases) {
      const view = describeHostUpdate(value, host)
      expect(view.needsAttention, name).toBe(false)
      expect(view.hasJob, name).toBe(false)
      expect(view.title, name).not.toContain('needs attention')
      expect(view.title, name).not.toBe('Update stopped')
      expect(`${view.message} ${view.recovery}`, name).not.toContain('Fedora software management')
      const legacy = buildUpdateCenterState({ currentVersion: '1.5.0', hostPlan: host, host: { platform: 'linux' } })
      const headline = buildUpdateCenterState({ currentVersion: '1.5.0', hostPlan: host, hostJourney: view, host: { platform: 'linux' } })
      expect(headline.statusLabel, name).toBe(legacy.statusLabel)
      expect(headline.statusTone, name).toBe(legacy.statusTone)
    }
    // A real update job stays visible even where the host is not otherwise qualified.
    expect(describeHostUpdate(guided({ job: { state: 'running', version: '1.5.1', check: { stage: 'installing' } } }), unqualified).active).toBe(true)
  })

  it('keeps a qualified Fedora read failure and genuine partial or unknown packages visible, never up to date', () => {
    const cached = { ...plan(), update_needed: false }
    const unreadable = { ...snapshot(null, {}), readback: 'unavailable' }
    const view = describeHostUpdate(unreadable, cached)
    expect(view.needsAttention).toBe(true)
    expect(view.hasJob).toBe(true)
    expect(view.title).toBe('Installed package state needs attention')
    expect(view.message).toContain('could not read')
    expect(view.message).not.toContain('Fedora software management')
    expect(describeHostUpdate(unreadable, plan(), { reviewing: true }).canInstall).toBe(false)
    const headline = buildUpdateCenterState({ currentVersion: '1.5.0', hostPlan: cached, hostJourney: view })
    expect(headline.statusTone).toBe('warning')
    expect(headline.statusLabel).not.toBe('Up to date')
    for (const kms of [{ state: 'partial', versions: ['1.5.0', '1.5.1'] }, { state: 'unknown' }]) {
      const genuine = describeHostUpdate(snapshot(null, { polaris: { state: 'installed', version: '1.5.0' }, 'polaris-kms': kms }), cached)
      expect(genuine.needsAttention).toBe(true)
      expect(genuine.title).toBe('Installed package state needs attention')
      expect(genuine.recovery).toContain('Fedora software management')
    }
  })

  it('mounts a guided host with only its guided note and no repeated status reads', async () => {
    vi.useFakeTimers()
    const fetcher = vi.fn(async (url) => ({ ok: true, json: async () => url.includes('/plan?') ? unqualified
      : guided({ installed: { polaris: { state: 'unknown' }, 'polaris-kms': { state: 'unknown' } } }) }))
    vi.stubGlobal('fetch', fetcher)
    const wrapper = mount(HostUpdateFlow)
    await flushPromises()
    expect(wrapper.find('[data-host-update-flow]').exists()).toBe(false)
    expect(wrapper.find('[data-update-guided]').text()).toContain('Fedora 44 x86_64 only')
    expect(wrapper.text()).not.toContain('needs attention')
    expect(wrapper.text()).not.toContain('Fedora software management')
    const reads = fetcher.mock.calls.length
    await vi.advanceTimersByTimeAsync(120000)
    expect(fetcher.mock.calls.length).toBe(reads)
    wrapper.unmount()
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
        const body = JSON.parse(options.body)
        job = { ...snapshot({ state: body.action === 'cancel' ? 'cancelled' : 'waiting_for_approval', version: '1.5.1', request_id: body.request_id,
          message: body.action === 'cancel' ? 'Approval cancelled.' : 'Approve on this PC.' }), accepted: true }
      }
      return { ok: true, json: async () => job }
    }))
    const wrapper = mount(HostUpdateFlow, { attachTo: document.body })
    await flushPromises()
    await wrapper.find('button').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('not an independent publisher signature')
    expect(wrapper.text()).toContain('Updates wait until streams and paused games end.')
    expect(wrapper.text()).toContain('Package setup scripts and cache triggers are skipped')
    const details = wrapper.find('[data-update-technical-details]')
    expect(details.attributes('open')).toBeUndefined()
    expect(details.text()).toContain(plan().assets[0].digest)
    expect(details.text()).toContain('Asset 10')
    expect(wrapper.find('input[type=password]').exists()).toBe(false)
    await wrapper.find('[data-update-install]').trigger('click')
    await flushPromises()
    const mutation = calls.find((call) => call.options.method === 'POST')
    const body = JSON.parse(mutation.options.body)
    expect(Object.keys(body).sort()).toEqual(['action', 'channel', 'request_id', 'review_id', 'version'])
    expect(body.review_id).toBe(plan().review_id)
    expect(mutation.options.headers['X-CSRF-Token']).toBe('test-csrf')
    expect(wrapper.find('[data-update-cancel]').exists()).toBe(true)
    expect(document.activeElement).toBe(wrapper.find('h3').element)
    expect(wrapper.find('[data-update-announcement]').attributes('aria-live')).toBe('polite')
    await wrapper.find('[data-update-cancel]').trigger('click')
    await flushPromises()
    expect(document.activeElement).toBe(wrapper.find('h3').element)
    expect(wrapper.find('h3').text()).toBe('Approval cancelled')
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
    await vi.advanceTimersByTimeAsync(5000)
    await flushPromises()
    expect(wrapper.find('[data-update-busy]').text()).toContain('paused for resume')
    wrapper.unmount()
  })

  it('polls visible idle tabs slowly, pauses hidden tabs and reads immediately on return', async () => {
    vi.useFakeTimers()
    let visible = 'visible'
    vi.spyOn(document, 'visibilityState', 'get').mockImplementation(() => visible)
    const fetcher = vi.fn(async (url) => ({ ok: true, json: async () => url.includes('/plan?') ? plan() : snapshot() }))
    vi.stubGlobal('fetch', fetcher)
    const wrapper = mount(HostUpdateFlow)
    await flushPromises()
    const initial = fetcher.mock.calls.length
    await vi.advanceTimersByTimeAsync(3000)
    expect(fetcher.mock.calls.length).toBe(initial)
    visible = 'hidden'
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(120000)
    expect(fetcher.mock.calls.length).toBe(initial)
    visible = 'visible'
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(0)
    await flushPromises()
    expect(fetcher.mock.calls.length).toBe(initial + 1)
    await vi.advanceTimersByTimeAsync(59999)
    expect(fetcher.mock.calls.length).toBe(initial + 1)
    await vi.advanceTimersByTimeAsync(1)
    await flushPromises()
    expect(fetcher.mock.calls.length).toBe(initial + 2)
    wrapper.unmount()
  })

  it('announces current progress and paused-game reasons in the live region', async () => {
    vi.useFakeTimers()
    let value = snapshot({ state: 'running', version: '1.5.1', check: { stage: 'downloading' } })
    vi.stubGlobal('fetch', vi.fn(async (url) => ({ ok: true, json: async () => url.includes('/plan?') ? plan() : value })))
    const wrapper = mount(HostUpdateFlow)
    await flushPromises()
    expect(wrapper.find('[data-update-announcement]').text()).toContain('Downloading official release assets')
    value = { ...snapshot({ state: 'running', version: '1.5.1', check: { stage: 'installing' } }), busy: { reasons: ['A game is paused for resume.'] } }
    await vi.advanceTimersByTimeAsync(3000)
    await flushPromises()
    expect(wrapper.find('[data-update-announcement]').text()).toContain('Installing RPM packages')
    expect(wrapper.find('[data-update-announcement]').text()).toContain('paused for resume')
    wrapper.unmount()
  })

  it.each([null, { state: 'failed', version: '1.5.0' }])('finishes an external install using readback even with journal %j', async (journal) => {
    vi.useFakeTimers()
    let value = snapshot(journal, installed('1.5.1'))
    let unreachable = false
    const posts = []
    vi.stubGlobal('fetch', vi.fn(async (url, options = {}) => {
      if (url.includes('/plan?')) return { ok: true, json: async () => ({ ...plan(), update_needed: false }) }
      if (options.method === 'POST') {
        posts.push({ url, body: JSON.parse(options.body) })
        unreachable = true
        return { ok: true, json: async () => ({ ...value, accepted: true }) }
      }
      if (unreachable) throw new Error('synthetic reconnect gap')
      return { ok: true, json: async () => value }
    }))
    const wrapper = mount(HostUpdateFlow, { attachTo: document.body })
    await flushPromises()
    await wrapper.find('[data-update-finish]').trigger('click')
    await flushPromises()
    expect(document.activeElement).toBe(wrapper.find('h3').element)
    await vi.advanceTimersByTimeAsync(93001)
    await flushPromises()
    expect(wrapper.text()).toContain('Check the PC running it')
    expect(posts).toHaveLength(1)
    expect(posts[0].url).toBe('./api/update/finish')
    expect(posts[0].body.version).toBe('1.5.1')
    unreachable = false
    value = { ...value, running_version: '1.5.1' }
    await vi.advanceTimersByTimeAsync(15000)
    await flushPromises()
    expect(wrapper.find('h3').text()).toBe('Polaris 1.5.1 is running')
    expect(wrapper.find('[data-update-finish]').exists()).toBe(false)
    expect(posts).toHaveLength(1)
    wrapper.unmount()
  })
})
