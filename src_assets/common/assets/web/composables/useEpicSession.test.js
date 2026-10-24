import { mount } from '@vue/test-utils'
import { defineComponent } from 'vue'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { useEpicSession } from './useEpicSession.js'

const reply = (body, ok = true, status = ok ? 200 : 409) => ({ ok, status, json: async () => body })
const active = extra => ({
  status: true, version: 1, available: true, app_uuid: 'u1', app_name: 'Alan Wake 2',
  session_id: 's1', revision: 'r1', state: 'confirmable', runtime: 'proton', can_adopt: true, message: '', ...extra,
})

let wrapper
afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.useRealTimers() })

function host(options = {}, { autostart = false } = {}) {
  const Host = defineComponent({
    setup() {
      const api = useEpicSession({ intervalMs: 5000, maxBackoffMs: 20000, ...options })
      if (autostart) api.start()
      return api
    },
    template: '<div>{{ state.state }}</div>',
  })
  wrapper = mount(Host)
  return wrapper
}

const posts = m => m.mock.calls.filter(c => c[1]?.method === 'POST')
const lastPost = m => { const p = posts(m); return p.length ? JSON.parse(p[p.length - 1][1].body) : null }

describe('useEpicSession', () => {
  it('retains the disclosed inspection count and clears it when the session ends', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(reply(active({ uninspected_process_count: 3 })))
      .mockResolvedValueOnce(reply({ available: false }))
    vi.stubGlobal('fetch', fetchMock)
    host()
    await wrapper.vm.load()
    expect(wrapper.vm.state.uninspected_process_count).toBe(3)
    await wrapper.vm.load()
    expect(wrapper.vm.state.uninspected_process_count).toBe(0)
  })

  it.each([-1, 1.5, '3', 8193, true, null])('refuses consent with an invalid inspection count %s', async count => {
    const fetchMock = vi.fn(async () => reply(active({ uninspected_process_count: count })))
    vi.stubGlobal('fetch', fetchMock)
    host()
    await wrapper.vm.load()
    expect(wrapper.vm.state.state).toBe('unavailable')
    expect(await wrapper.vm.adopt()).toBe(false)
    expect(posts(fetchMock)).toHaveLength(0)
  })

  it('adopt sends confirm:true with the exact current session and revision', async () => {
    const fetchMock = vi.fn(async (_url, opt) => (opt?.method === 'POST' ? reply({ status: true }) : reply(active())))
    vi.stubGlobal('fetch', fetchMock)
    host()
    await wrapper.vm.load()
    expect(wrapper.vm.state.state).toBe('confirmable')
    expect(await wrapper.vm.adopt()).toBe(true)
    expect(lastPost(fetchMock)).toEqual({ action: 'adopt', session_id: 's1', revision: 'r1', confirm: true })
  })

  it('does not adopt (no POST) when the session is not confirmable or not adoptable', async () => {
    const fetchMock = vi.fn(async () => reply(active({ state: 'unmanaged', can_adopt: false })))
    vi.stubGlobal('fetch', fetchMock)
    host()
    await wrapper.vm.load()
    expect(await wrapper.vm.adopt()).toBe(false)
    expect(posts(fetchMock)).toHaveLength(0)
    expect(wrapper.vm.actionError).toContain('no longer be adopted')
  })

  it('release leaves the game running: action release with ids and no confirm flag', async () => {
    const fetchMock = vi.fn(async (_url, opt) => (opt?.method === 'POST' ? reply({ status: true }) : reply(active({ state: 'managed' }))))
    vi.stubGlobal('fetch', fetchMock)
    host()
    await wrapper.vm.load()
    expect(await wrapper.vm.release()).toBe(true)
    const body = lastPost(fetchMock)
    expect(body).toEqual({ action: 'release', session_id: 's1', revision: 'r1' })
    expect(body).not.toHaveProperty('confirm')
  })

  it('surfaces a refused adopt and never auto-replays it on later polls', async () => {
    vi.useFakeTimers()
    const fetchMock = vi.fn(async (_url, opt) => (opt?.method === 'POST'
      ? reply({ status: false, code: 'stale', error: 'Session moved on.' }, false, 409)
      : reply(active())))
    vi.stubGlobal('fetch', fetchMock)
    host({}, { autostart: true })
    await vi.advanceTimersByTimeAsync(0)
    expect(await wrapper.vm.adopt()).toBe(false)
    expect(wrapper.vm.actionError).toBe('Session moved on.')
    const before = posts(fetchMock).length
    await vi.advanceTimersByTimeAsync(20000) // several GET polls
    expect(posts(fetchMock)).toHaveLength(before) // no POST replay
  })

  it('drops a stale action error once the session identity changes', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(reply(active())) // load #1
      .mockResolvedValueOnce(reply({ status: false, error: 'No.' }, false, 409)) // adopt POST refused
      .mockResolvedValueOnce(reply(active())) // read-back, same identity -> keep error
      .mockResolvedValueOnce(reply(active({ session_id: 's2', revision: 'r2' }))) // new session -> clear
    vi.stubGlobal('fetch', fetchMock)
    host()
    await wrapper.vm.load()
    await wrapper.vm.adopt()
    expect(wrapper.vm.actionError).toBe('No.')
    await wrapper.vm.load()
    expect(wrapper.vm.actionError).toBe('')
  })

  it('keeps a refused action visible across same-session flaps until the session changes', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(reply(active())) // load #1
      .mockResolvedValueOnce(reply({ status: false, code: 'adoption_refused', error: 'No permission was granted.' }, false, 409))
      .mockResolvedValueOnce(reply(active({ state: 'unavailable', revision: 's1:', can_adopt: false }))) // inconclusive read-back
      .mockResolvedValueOnce(reply(active({ revision: 'r2' }))) // same session, another revision
      .mockResolvedValueOnce(reply(active({ session_id: 's2', revision: 'r3' }))) // a different session
    vi.stubGlobal('fetch', fetchMock)
    host()
    await wrapper.vm.load()
    expect(await wrapper.vm.adopt()).toBe(false)
    expect(wrapper.vm.state.state).toBe('unavailable')
    expect(wrapper.vm.actionError).toBe('No permission was granted.')
    await wrapper.vm.load()
    expect(wrapper.vm.actionError).toBe('No permission was granted.')
    await wrapper.vm.load()
    expect(wrapper.vm.actionError).toBe('')
    expect(posts(fetchMock)).toHaveLength(1)
  })

  it('shows an unreadable active game honestly, distinct from no active session', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(reply(active({ state: 'unavailable', can_adopt: false })))
      .mockResolvedValueOnce(reply({ status: true, version: 1, available: false }))
    vi.stubGlobal('fetch', fetchMock)
    host()
    await wrapper.vm.load()
    expect(wrapper.vm.state.available).toBe(true)
    expect(wrapper.vm.state.state).toBe('unavailable')
    await wrapper.vm.load()
    expect(wrapper.vm.state.available).toBe(false)
    expect(wrapper.vm.state.state).toBe('')
  })

  it('reports a non-2xx GET as a load error rather than as no active game', async () => {
    const fetchMock = vi.fn(async () => reply({ status: false, code: 'forbidden', error: 'Host owner sign-in required.' }, false, 403))
    vi.stubGlobal('fetch', fetchMock)
    host()
    expect(await wrapper.vm.load()).toBe(false)
    expect(wrapper.vm.loadError).toBe('Host owner sign-in required.')
    expect(wrapper.vm.state.available).toBe(false)
  })
})
