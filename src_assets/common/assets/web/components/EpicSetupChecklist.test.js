import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import EpicSetupChecklist from './EpicSetupChecklist.vue'
import { markWebUiAuthenticated, markWebUiUnauthenticated } from '../auth-state'
import { epicSample, epicReply, deferredEpic } from '../epic-setup-fixtures'

let wrapper, fetchMock, scan
const game = { name: '<script>Game</script>', provider: 'official', namespace: 'ns', item_id: 'item', artifact_id: 'game', selected: true, install: 'install-a' }
beforeEach(() => {
  vi.useFakeTimers()
  vi.spyOn(document, 'hidden', 'get').mockReturnValue(false)
  markWebUiAuthenticated({ platform: 'linux' })
  scan = vi.fn(async () => undefined)
  fetchMock = vi.fn(async () => epicReply())
  vi.stubGlobal('fetch', fetchMock)
})
afterEach(() => { wrapper?.unmount(); markWebUiUnauthenticated(); vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.useRealTimers() })
function host(props = {}) {
  wrapper = mount(EpicSetupChecklist, { props: { games: [], scan, ...props }, attachTo: document.body })
  return wrapper
}
async function open() {
  wrapper.find('details').element.open = true
  await wrapper.find('details').trigger('toggle')
  await flushPromises()
}
const button = name => wrapper.findAll('button').find(b => b.text() === name)

describe('lazy Epic checklist', () => {
  it('has an explicit zero-title entry, and opening reads status without scanning', async () => {
    host()
    expect(wrapper.find('summary').text()).toBe('Epic Private Stream setup')
    expect(fetchMock).not.toHaveBeenCalled()
    await open()
    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(scan).not.toHaveBeenCalled()
    expect(wrapper.find('[role="region"]').attributes('aria-labelledby')).toBe('epic-setup-title')
    expect(wrapper.findAll('ol > li').map(li => li.attributes('data-epic-row'))).toEqual(['observed', 'receipt', 'closed'])
    expect(wrapper.text()).toContain('choose a game to check its setup')
    expect(wrapper.text()).toContain('Polaris checks again when you launch')
  })
  it('retains focus and emits no repeated live-summary mutation when only sample age/revision changes', async () => {
    host(); await open()
    const retry = button('Check again'); retry.element.focus()
    const summary = wrapper.find('[aria-live="polite"]'), changed = vi.fn()
    const observer = new MutationObserver(changed); observer.observe(summary.element, { childList: true, characterData: true, subtree: true })
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ revision: '5', sample_age_ms: 20 })))
    await vi.advanceTimersByTimeAsync(5000); await flushPromises()
    expect(document.activeElement).toBe(retry.element)
    expect(changed).not.toHaveBeenCalled()
    expect(wrapper.findAll('[aria-live]')).toHaveLength(1)
    observer.disconnect()
  })
  it('keeps focused controls mounted and named while requests are pending, then Escape returns to the entry', async () => {
    host(); await open()
    const slow = deferredEpic(); fetchMock.mockReturnValueOnce(slow.promise)
    const retry = button('Check again'); retry.element.focus(); await retry.trigger('click')
    expect(retry.attributes('aria-disabled')).toBe('true')
    expect(retry.attributes('disabled')).toBeUndefined()
    expect(document.activeElement).toBe(retry.element)
    await retry.trigger('keydown', { key: 'Escape' })
    expect(wrapper.find('details').element.open).toBe(false)
    expect(document.activeElement).toBe(wrapper.find('summary').element)
    expect(fetchMock.mock.calls.at(-1)[1].signal.aborted).toBe(true)
    slow.resolve(epicReply()); await flushPromises()
  })
  it('renders hostile title text literally, checks a separate picker, and never changes import staging', async () => {
    host({ games: [game] }); await open()
    expect(wrapper.findAll('option')[1].text()).toContain('<script>Game</script>')
    const selected = wrapper.findAll('option')[1].attributes('value')
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ selected: { namespace_id: 'ns', item_id: 'item', artifact_id: 'game' }, runtime: 'absent' })))
    await wrapper.find('select').setValue(selected); await flushPromises()
    expect(wrapper.find('[data-epic-row="closed"]').text()).toContain("can't confirm all Private Stream prerequisites")
    expect(wrapper.find('[data-epic-row="closed"]').text()).not.toContain('ready for Private Stream')
    expect(game.selected).toBe(true)
    expect(wrapper.find('script').exists()).toBe(false)
  })
  it('uses the scan fragment for notification but obtains receipt facts from the following GET', async () => {
    scan.mockResolvedValue({ status: true, epic_setup: { version: 1, capture: 'unavailable', scan: 'metadata_unavailable', metadata_available: false, receipt_revision: '2' } })
    host(); await open()
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ receipt: 'previous', last_scan: 'metadata_unavailable' })))
    await button('Rescan Sources').trigger('click'); await flushPromises()
    expect(scan).toHaveBeenCalledTimes(1)
    expect(wrapper.text()).toContain('No new setup receipt was created')
    expect(wrapper.find('[data-epic-row="receipt"]').text()).toContain('Previous successful scan (historical)')
  })
  it('does not invent a receipt from a legacy scan or an invalid fragment', async () => {
    host(); await open()
    scan.mockResolvedValue({ status: true, epic_setup: { scan: 'complete' } })
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ revision: '5', receipt: 'required', receipt_revision: '0', last_scan: 'required' })))
    await button('Rescan Sources').trigger('click'); await flushPromises()
    expect(wrapper.find('[data-epic-row="receipt"]').text()).toContain('Needs action')
  })
  it('pauses for scanning and provider changes, discards installations, and preserves staging changes', async () => {
    host({ games: [{ ...game }] }); await open()
    const firstCount = fetchMock.mock.calls.length
    await wrapper.setProps({ games: [{ ...game, selected: false }] })
    expect(fetchMock).toHaveBeenCalledTimes(firstCount)
    await wrapper.setProps({ scanning: true }); expect(wrapper.text()).toContain('paused')
    const count = fetchMock.mock.calls.length
    await wrapper.setProps({ games: [{ ...game, selected: false }] })
    expect(fetchMock).toHaveBeenCalledTimes(count)
    await wrapper.setProps({ scanning: false }); await flushPromises()
    await wrapper.setProps({ switching: true }); expect(wrapper.text()).toContain('paused')
    await wrapper.setProps({ switching: false }); await flushPromises()
    const slow = deferredEpic(); fetchMock.mockReturnValueOnce(slow.promise)
    await button('Check again').trigger('click')
    const signal = fetchMock.mock.calls.at(-1)[1].signal
    await wrapper.setProps({ games: [{ ...game, install: 'changed-install' }] })
    expect(signal.aborted).toBe(true)
    slow.resolve(epicReply()); await flushPromises()
  })
  it.each([
    { revision: '5', state: 'needs_action', reason: 'launcher_changed', proof: 'changed', receipt: 'required', receipt_revision: '0' },
    { epoch: 'new-epoch', receipt: 'required', receipt_revision: '0' },
    { epoch: '', revision: '0', state: 'busy', reason: 'session_busy', desktop: 'not_checked', receipt: 'required', receipt_revision: '0' },
  ])('keeps update, restart and active-session outcomes visible: %j', async override => {
    host(); await open()
    fetchMock.mockResolvedValueOnce(epicReply(epicSample(override)))
    await button('Check again').trigger('click'); await flushPromises()
    expect(wrapper.text()).toMatch(/changed or updated|restarted|stream is active or changing/)
  })
  it('clears host facts immediately on logout', async () => {
    host(); await open(); markWebUiUnauthenticated(); await flushPromises()
    expect(wrapper.text()).toContain('Sign in to this PC')
    expect(wrapper.find('[data-epic-row="receipt"]').text()).not.toContain('Complete')
  })
})
