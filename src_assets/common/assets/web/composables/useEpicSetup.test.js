import { mount, flushPromises } from '@vue/test-utils'
import { defineComponent, ref } from 'vue'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useEpicSetup } from './useEpicSetup'
import { epicSample, epicReply, epicTitle, deferredEpic } from '../epic-setup-fixtures'

let wrapper, api, active, auth, title, context, origin, fetchMock
function host() {
  active = ref(true); auth = ref(true); title = ref(null); context = ref('install-a'); origin = ref(window.location.origin)
  wrapper = mount(defineComponent({ setup() {
    api = useEpicSetup({ active, authenticated: auth, selection: title, context, origin, now: () => Date.now() })
    return () => null
  } }))
}
beforeEach(() => {
  vi.useFakeTimers()
  vi.spyOn(document, 'hidden', 'get').mockReturnValue(false)
  fetchMock = vi.fn(async () => epicReply())
  vi.stubGlobal('fetch', fetchMock)
})
afterEach(() => { wrapper?.unmount(); vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.useRealTimers() })

describe('passive Epic setup polling and custody', () => {
  it('coalesces a read, starts the 5 second timer on completion, and never sends a mutation', async () => {
    const slow = deferredEpic(); fetchMock.mockReturnValueOnce(slow.promise)
    host(); const shared = api.refresh()
    expect(api.refresh()).toBe(shared)
    await vi.advanceTimersByTimeAsync(4000)
    expect(fetchMock).toHaveBeenCalledTimes(1)
    slow.resolve(epicReply()); await flushPromises()
    expect(api.state.value.phase).toBe('current')
    await vi.advanceTimersByTimeAsync(4999); expect(fetchMock).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1); expect(fetchMock).toHaveBeenCalledTimes(2)
    for (const [url, options] of fetchMock.mock.calls) {
      expect(new URL(url).pathname).toBe('/api/epic/setup')
      expect(options).toMatchObject({ credentials: 'include', cache: 'no-store' })
      expect(options.method).toBeUndefined()
    }
  })
  it('expires current green by 10 seconds from request start while the next request stalls', async () => {
    host(); await flushPromises()
    const slow = deferredEpic(); fetchMock.mockReturnValueOnce(slow.promise)
    await vi.advanceTimersByTimeAsync(5000)
    expect(api.loading.value).toBe(true)
    await vi.advanceTimersByTimeAsync(5000)
    expect(api.current.value).toBe(false)
    expect(api.state.value.phase).toBe('stale')
    await vi.advanceTimersByTimeAsync(3000)
    expect(api.state.value.phase).toBe('timeout')
    expect(fetchMock.mock.calls[1][1].signal.aborted).toBe(true)
    slow.resolve(epicReply()); await flushPromises()
    expect(api.current.value).toBe(false)
  })
  it('subtracts host sample age and elapsed transport time and rejects already stale samples', async () => {
    const slow = deferredEpic(); fetchMock.mockReturnValueOnce(slow.promise)
    host(); await vi.advanceTimersByTimeAsync(4000)
    slow.resolve(epicReply(epicSample({ sample_age_ms: 3000 }))); await flushPromises()
    await vi.advanceTimersByTimeAsync(3000)
    expect(api.state.value.phase).toBe('stale')
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ sample_age_ms: 10000 })))
    await api.refresh(); expect(api.state.value.phase).toBe('stale')
  })
  it('bounds a JSON body that stalls as well as a fetch that stalls', async () => {
    const json = deferredEpic(); fetchMock.mockResolvedValueOnce({ ok: true, status: 200, json: () => json.promise })
    host(); await vi.advanceTimersByTimeAsync(8000)
    expect(api.state.value.phase).toBe('timeout')
    json.resolve(epicSample()); await flushPromises(); expect(api.current.value).toBe(false)
  })
  it('backs off completion-driven reads at 10, 20, then at most 30 seconds, and resets after success', async () => {
    fetchMock.mockRejectedValue(new Error('offline'))
    host(); await flushPromises()
    for (const delay of [10000, 20000, 30000, 30000]) {
      const count = fetchMock.mock.calls.length
      await vi.advanceTimersByTimeAsync(delay - 1); expect(fetchMock).toHaveBeenCalledTimes(count)
      await vi.advanceTimersByTimeAsync(1); expect(fetchMock).toHaveBeenCalledTimes(count + 1)
    }
    fetchMock.mockResolvedValue(epicReply()); await api.refresh()
    const count = fetchMock.mock.calls.length
    await vi.advanceTimersByTimeAsync(5000); expect(fetchMock).toHaveBeenCalledTimes(count + 1)
  })
  it.each(['hidden', 'closed', 'logout', 'title', 'provider-install', 'origin', 'unmounted'])('aborts and clears on %s; a deferred old response cannot revive facts', async event => {
    host(); await flushPromises()
    const slow = deferredEpic(); fetchMock.mockReturnValueOnce(slow.promise)
    api.refresh(); const signal = fetchMock.mock.calls.at(-1)[1].signal
    if (event === 'hidden') { vi.spyOn(document, 'hidden', 'get').mockReturnValue(true); document.dispatchEvent(new Event('visibilitychange')) }
    if (event === 'closed') active.value = false
    if (event === 'logout') auth.value = false
    if (event === 'title') title.value = epicTitle
    if (event === 'provider-install') context.value = 'install-b-provider-official'
    if (event === 'origin') origin.value = 'https://different-host.invalid'
    if (event === 'unmounted') wrapper.unmount()
    expect(signal.aborted).toBe(true)
    expect(api.current.value).toBe(false)
    slow.resolve(epicReply(epicSample({ epoch: 'obsolete', revision: '999' }))); await flushPromises()
    expect(api.state.value.facts?.epoch).not.toBe('obsolete')
    if (['hidden', 'closed', 'logout', 'unmounted'].includes(event)) {
      const count = fetchMock.mock.calls.length
      await vi.advanceTimersByTimeAsync(60000); expect(fetchMock).toHaveBeenCalledTimes(count)
    }
  })
  it('requires a fresh sample on visible return and after re-opening; never scans automatically', async () => {
    host(); await flushPromises()
    active.value = false; expect(api.current.value).toBe(false)
    active.value = true; await flushPromises(); expect(api.current.value).toBe(true)
    vi.spyOn(document, 'hidden', 'get').mockReturnValue(true); document.dispatchEvent(new Event('visibilitychange'))
    expect(api.current.value).toBe(false)
    vi.spyOn(document, 'hidden', 'get').mockReturnValue(false); document.dispatchEvent(new Event('visibilitychange'))
    await flushPromises(); expect(api.current.value).toBe(true)
    expect(fetchMock.mock.calls.every(([url]) => !url.includes('scan'))).toBe(true)
  })
  it('serializes only a validated exact title query and rejects a different returned selection', async () => {
    host(); await flushPromises(); title.value = epicTitle; await flushPromises()
    const url = new URL(fetchMock.mock.calls.at(-1)[0])
    expect(Object.fromEntries(url.searchParams)).toEqual({ namespace: 'ns', item_id: 'item', artifact_id: 'game' })
    expect(api.state.value.phase).toBe('superseded')
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ selected: epicTitle, runtime: 'absent' })))
    expect(await api.refresh()).toBe(true)
  })
  it('rejects old revisions, old receipt revisions at the same revision and retired epochs', async () => {
    host(); await flushPromises()
    for (const body of [epicSample({ revision: '3' }), epicSample({ receipt_revision: '1' })]) {
      fetchMock.mockResolvedValueOnce(epicReply(body)); await api.refresh()
      expect(api.state.value.phase).toBe('superseded')
    }
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ epoch: 'epoch-b', revision: '1', receipt: 'required', receipt_revision: '0' })))
    await api.refresh(); expect(api.state.value.notice).toBe('restart')
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ revision: '999' })))
    await api.refresh(); expect(api.state.value.phase).toBe('superseded')
  })
  it('allows an authoritative newer revision to invalidate a receipt after a file update', async () => {
    host(); await flushPromises()
    fetchMock.mockResolvedValueOnce(epicReply(epicSample({ revision: '5', state: 'needs_action', reason: 'launcher_changed', proof: 'changed', receipt: 'required', receipt_revision: '0' })))
    expect(await api.refresh()).toBe(true)
    expect(api.state.value.history).toBeNull()
    expect(api.state.value.notice).toBe('update')
  })
  it.each([[401, 'auth'], [403, 'auth'], [404, 'unsupported'], [500, 'failed'], [503, 'auth_store']])('clears and distinguishes HTTP %s', async (status, phase) => {
    host(); await flushPromises()
    fetchMock.mockResolvedValueOnce(epicReply({ status: false, error: status === 503 ? 'authentication_unavailable' : 'authentication_required' }, status))
    await api.refresh(); expect(api.state.value.phase).toBe(phase); expect(api.current.value).toBe(false)
  })
  it.each([401, 403, 404])('retains the HTTP %s distinction with a non-JSON error body', async status => {
    fetchMock.mockResolvedValue({ ok: false, status, json: async () => { throw Error('HTML') } })
    host(); await flushPromises(); expect(api.state.value.phase).toBe(status === 404 ? 'unsupported' : 'auth')
  })
  it.each([{ version: 2 }, { desktop: 'future' }, { receipt: 'future' }, { prerequisites: 'future' }, { state: 'future' },
    { selected: {} }, { sample_age_ms: -1 }, { proof: 'future' }])('rejects malformed or unknown facts %j', async override => {
    fetchMock.mockResolvedValue(epicReply(epicSample(override)))
    host(); await flushPromises(); expect(api.state.value.phase).toBe('malformed')
  })
  it('does not render arbitrary backend exception text and never persists truth flags', async () => {
    const storage = vi.spyOn(Storage.prototype, 'setItem')
    fetchMock.mockRejectedValue(new Error('<script>/private/path pid=42</script>'))
    host(); await flushPromises()
    expect(api.state.value.phase).toBe('offline')
    expect(JSON.stringify(api.state.value)).not.toContain('/private/path')
    expect(storage).not.toHaveBeenCalled()
  })
})
