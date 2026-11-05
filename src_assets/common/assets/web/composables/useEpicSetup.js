import { computed, onMounted, onUnmounted, shallowRef, ref, toValue, watch } from 'vue'
import { webUiAuthenticated } from '../auth-state'
import { readEpicSetup, readEpicSetupError } from '../epic-setup-contract'
import { emptyEpicSetup, reduceEpicSetup } from '../epic-setup-state'

// Passive GETs only. The caller owns explicit scans; no action is ever retried here.
export function useEpicSetup(options = {}) {
  const state = shallowRef(emptyEpicSetup())
  const loading = ref(false)
  const now = options.now || (() => performance.now())
  const origin = () => toValue(options.origin) ?? window.location.origin
  const selection = () => toValue(options.selection) ?? null
  const authenticated = () => toValue(options.authenticated ?? webUiAuthenticated)
  const active = () => toValue(options.active) === true && authenticated() && !document.hidden
  let mounted = false, disposed = false, generation = 0, sequence = 0, failures = 0
  let request = null, pollTimer = null, expiryTimer = null
  let epoch = '', revision = '0', receiptRevision = '0'
  const retiredEpochs = new Set()
  const dispatch = event => { state.value = reduceEpicSetup(state.value, event) }
  const sameTitle = (a, b) => JSON.stringify(a) === JSON.stringify(b)
  function clearTimers() {
    clearTimeout(pollTimer); clearTimeout(expiryTimer)
    pollTimer = expiryTimer = null
  }
  function cancel(phase = 'checking', forget = false) {
    generation++
    clearTimers()
    request?.controller.abort()
    request = null
    loading.value = false
    dispatch({ type: forget ? 'clear' : 'invalidate', phase })
    if (!forget) state.value = { ...state.value, phase }
    if (forget) { epoch = ''; revision = receiptRevision = '0'; retiredEpochs.clear(); failures = 0 }
  }
  function schedule() {
    clearTimeout(pollTimer)
    if (mounted && !disposed && active()) {
      pollTimer = setTimeout(refresh, Math.min(30000, 5000 * 2 ** Math.min(failures, 3)))
    }
  }
  function refresh() {
    if (!mounted || disposed || !active()) return Promise.resolve(false)
    if (request) return request.promise
    clearTimeout(pollTimer)
    const current = { id: ++sequence, generation, title: selection(), origin: origin(),
      start: now(), controller: new AbortController() }
    request = current
    loading.value = true
    current.promise = read(current)
    return current.promise
  }
  async function read(current) {
    let timeout
    const owns = () => !disposed && active() && request === current && current.generation === generation &&
      current.id === sequence && origin() === current.origin && sameTitle(selection(), current.title)
    try {
      const url = new URL('./api/epic/setup', window.location.href)
      if (url.origin !== current.origin) throw { phase: 'superseded' }
      if (current.title) {
        url.searchParams.set('namespace', current.title.namespace_id)
        url.searchParams.set('item_id', current.title.item_id)
        url.searchParams.set('artifact_id', current.title.artifact_id)
      }
      // Bound both the transport and JSON parsing, even if a test/adapter ignores abort.
      const operation = async () => {
        const response = await fetch(url.href, { credentials: 'include', cache: 'no-store', signal: current.controller.signal })
        let body
        try { body = await response.json() } catch { if (response.ok) throw { phase: 'malformed' } }
        if (!response.ok) {
          const error = readEpicSetupError(body)
          throw { phase: [401, 403].includes(response.status) ? 'auth' : response.status === 404 ? 'unsupported'
            : error === 'authentication_unavailable' ? 'auth_store' : 'failed' }
        }
        const facts = readEpicSetup(body)
        if (!facts) throw { phase: 'malformed' }
        return facts
      }
      const facts = await Promise.race([operation(), new Promise((_, reject) => {
        timeout = setTimeout(() => { current.controller.abort(); reject({ phase: 'timeout' }) }, 8000)
        current.controller.signal.addEventListener('abort', () => reject({ phase: 'aborted' }), { once: true })
      })])
      if (!owns()) return false
      if (!sameTitle(facts.selected, current.title) || retiredEpochs.has(facts.epoch) ||
          (facts.epoch === epoch && (BigInt(facts.revision) < BigInt(revision) ||
            (facts.revision === revision && BigInt(facts.receipt_revision) < BigInt(receiptRevision))))) {
        throw { phase: 'superseded' }
      }
      const remaining = 10000 - (now() - current.start) - facts.sample_age_ms
      if (remaining <= 0) throw { phase: 'stale' }
      const restarted = Boolean(epoch && facts.epoch && epoch !== facts.epoch)
      if (restarted) retiredEpochs.add(epoch)
      // Busy/unsupported replies may omit the epoch; they must not erase our revision fence.
      if (facts.epoch) { epoch = facts.epoch; revision = facts.revision; receiptRevision = facts.receipt_revision }
      clearTimeout(expiryTimer)
      failures = 0
      dispatch({ type: 'sample', facts, restarted })
      expiryTimer = setTimeout(() => dispatch({ type: 'error', phase: 'stale' }), remaining)
      return true
    } catch (error) {
      if (!owns()) return false
      failures++
      clearTimeout(expiryTimer)
      dispatch({ type: 'error', phase: error.phase === 'aborted' ? 'timeout' : error.phase || 'offline' })
      return false
    } finally {
      clearTimeout(timeout)
      if (request === current) {
        request = null
        loading.value = false
        schedule()
      }
    }
  }
  function reconcile(forget = false) {
    cancel(!authenticated() ? 'auth' : active() ? 'checking' : 'paused', forget)
    if (mounted && active()) refresh()
  }
  function invalidate() { reconcile() }
  function scanResult(scan) {
    if (disposed) return
    cancel('checking')
    dispatch({ type: 'scan', scan })
    if (active()) refresh()
  }
  watch(() => [toValue(options.active), authenticated(), origin(), JSON.stringify(selection()), toValue(options.context)],
    (value, previous) => reconcile(!previous || value[1] !== previous[1] || value[2] !== previous[2]), { flush: 'sync' })
  const visibility = () => reconcile()
  const pagehide = () => cancel('paused', true)
  onMounted(() => {
    mounted = true
    document.addEventListener('visibilitychange', visibility)
    window.addEventListener('pagehide', pagehide)
    reconcile(true)
  })
  onUnmounted(() => {
    disposed = true; mounted = false; cancel('paused', true)
    document.removeEventListener('visibilitychange', visibility)
    window.removeEventListener('pagehide', pagehide)
  })
  return { state, loading, refresh, invalidate, scanResult, current: computed(() => state.value.facts !== null) }
}
