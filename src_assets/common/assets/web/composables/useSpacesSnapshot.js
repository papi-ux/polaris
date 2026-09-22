import { onMounted, onUnmounted, reactive, ref } from 'vue'
import { validSnapshot } from '../spaces-access.js'

/**
 * The Spaces snapshot the console polls: /api/multiseat/profiles, validated,
 * refreshed on a completion-driven timer that pauses while the tab is hidden
 * and backs off while the host is not answering. Mutations call load()
 * themselves for read-back; the poll never overlaps a load in flight and skips
 * while the caller reports itself busy, so a save is never raced by a refresh.
 *
 * @param {{ intervalMs?: number, maxBackoffMs?: number, timeoutMs?: number,
 *   busy?: () => boolean, onSnapshot?: (next: object) => void,
 *   messages?: { load?: string, verify?: string } }} options
 */
export function useSpacesSnapshot(options = {}) {
  const intervalMs = options.intervalMs ?? 10000
  const maxBackoffMs = Math.max(intervalMs, options.maxBackoffMs ?? 60000)
  const timeoutMs = options.timeoutMs ?? 12000
  const messages = {
    load: options.messages?.load || 'Could not load Spaces. Refresh to try again.',
    verify: options.messages?.verify || 'Could not verify Spaces. Refresh to try again.',
  }
  const state = reactive(emptyState())
  const loading = ref(false)
  const loadError = ref('')
  const loaded = ref(false)
  let request = null
  let timer = null
  let failures = 0
  let mounted = false
  let disposed = false

  function emptyState() {
    return {
      enabled: false, available: false, changing: false, failed: false, profiles: [], activity: null,
      creation_available: false, management_available: false, access_available: false, removal_available: false,
      desktop_clients: undefined, desktop_default_clients: undefined, desktop_by_default: undefined, capacity: null,
      runtime_move_available: false, runtime_move_job: null,
    }
  }

  function hidden() {
    return typeof document !== 'undefined' && document.hidden
  }

  function busy() {
    return typeof options.busy === 'function' && Boolean(options.busy())
  }

  async function load() {
    if (disposed) return false
    loading.value = true
    request?.abort()
    const current = new AbortController()
    request = current
    const timeout = setTimeout(() => current.abort(), timeoutMs)
    try {
      const response = await fetch('./api/multiseat/profiles', { credentials: 'include', cache: 'no-store', signal: current.signal })
      if (!response.ok) throw new Error(messages.load)
      const next = await response.json()
      if (disposed || request !== current) return false
      if (!validSnapshot(next)) throw new Error(messages.verify)
      // Keys the host may omit are reset before the merge so a stale grant or
      // an old activity list never survives a snapshot that dropped it.
      Object.assign(state, emptyState(), next)
      loaded.value = true
      loadError.value = ''
      failures = 0
      options.onSnapshot?.(next)
      return true
    } catch (cause) {
      if (disposed || request !== current) return false
      failures += 1
      loadError.value = cause.message || messages.load
      return false
    } finally {
      clearTimeout(timeout)
      if (request === current) loading.value = false
    }
  }

  function nextDelayMs() {
    return failures ? Math.min(maxBackoffMs, intervalMs * (2 ** failures)) : intervalMs
  }

  function stopTimer() {
    if (timer) {
      clearTimeout(timer)
      timer = null
    }
  }

  function schedule() {
    stopTimer()
    if (!mounted || hidden()) return
    timer = setTimeout(poll, nextDelayMs())
  }

  async function poll() {
    timer = null
    if (!mounted || hidden()) return
    if (!busy() && !loading.value) await load()
    schedule()
  }

  function handleVisibility() {
    if (hidden()) stopTimer()
    else poll()
  }

  /** Load now, then keep polling. */
  async function start() {
    const ok = await load()
    schedule()
    return ok
  }

  onMounted(() => {
    mounted = true
    if (typeof document !== 'undefined') document.addEventListener('visibilitychange', handleVisibility)
  })
  onUnmounted(() => {
    mounted = false
    disposed = true
    stopTimer()
    request?.abort()
    if (typeof document !== 'undefined') document.removeEventListener('visibilitychange', handleVisibility)
  })

  return { state, loading, loadError, loaded, load, start }
}
