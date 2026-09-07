import { effectScope, nextTick, ref } from 'vue'
import { flushPromises } from '@vue/test-utils'
import { usePendingPairings } from './usePendingPairings'

const first = { id: 'a'.repeat(32), name: 'Same client name', address: '192.0.2.1' }
const second = { id: 'b'.repeat(32), name: 'Same client name', address: '192.0.2.2' }

describe('manual pairing request selection', () => {
  let scope
  let active
  let state
  let requests
  beforeEach(() => {
    vi.useFakeTimers()
    requests = [first, second]
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ pairings: requests }) })))
    scope = effectScope()
    active = ref(true)
    scope.run(() => { state = usePendingPairings(active) })
  })
  afterEach(() => {
    scope.stop()
    vi.unstubAllGlobals()
    vi.useRealTimers()
  })

  it('requires explicit selection even with one pending request', async () => {
    await flushPromises()
    expect(state.pendingPairings.value).toEqual([first, second])
    expect(state.selectedPairingId.value).toBe('')
    requests = [first]
    await state.refreshPairings()
    expect(state.selectedPairingId.value).toBe('')
  })

  it('preserves the selected ID across reorder and clears it on expiration without selecting a replacement', async () => {
    await flushPromises()
    state.selectedPairingId.value = first.id
    requests = [second, first]
    await state.refreshPairings()
    expect(state.selectedPairingId.value).toBe(first.id)
    requests = [second]
    await state.refreshPairings()
    expect(state.selectedPairingId.value).toBe('')
  })

  it('clears stale requests and selection if authentication or loading fails', async () => {
    await flushPromises()
    state.selectedPairingId.value = first.id
    fetch.mockResolvedValueOnce({ ok: false })
    await state.refreshPairings()
    expect(state.pendingPairings.value).toEqual([])
    expect(state.selectedPairingId.value).toBe('')
    expect(state.pairingListError.value).toBeTruthy()
  })

  it('polls only while manual pairing is active and stops after disposal', async () => {
    await flushPromises()
    await vi.advanceTimersByTimeAsync(3000)
    expect(fetch).toHaveBeenCalledTimes(2)
    active.value = false
    await nextTick()
    await vi.advanceTimersByTimeAsync(6000)
    expect(fetch).toHaveBeenCalledTimes(2)
    active.value = true
    await nextTick()
    await flushPromises()
    expect(fetch).toHaveBeenCalledTimes(3)
    scope.stop()
    await vi.advanceTimersByTimeAsync(6000)
    expect(fetch).toHaveBeenCalledTimes(3)
  })
})
