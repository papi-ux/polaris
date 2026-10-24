import { onMounted, onUnmounted, reactive, ref } from 'vue'

// The management states the backend may report for an active Epic session.
const VALID_STATES = new Set(['unmanaged', 'confirmable', 'managed', 'closing', 'unavailable', 'ended'])

/**
 * The owner-confirmed Epic Preview session the Mission Control console polls:
 * GET ./api/epic/session on a bounded, completion-driven 5s timer that pauses
 * while the tab is hidden and backs off while the host is not answering.
 *
 * Ownership is never automatic. adopt() sends the explicit owner grant
 * (confirm:true with the current session/revision); release() leaves the game
 * running and only clears the grant. Neither the poll nor a read-back ever
 * replays an action: a refused, errored or stale POST is surfaced, not retried.
 *
 * @param {{ intervalMs?: number, maxBackoffMs?: number, timeoutMs?: number,
 *   shouldPoll?: () => boolean }} options
 */
export function useEpicSession(options = {}) {
  const intervalMs = options.intervalMs ?? 5000
  const maxBackoffMs = Math.max(intervalMs, options.maxBackoffMs ?? 30000)
  const timeoutMs = options.timeoutMs ?? 8000
  const state = reactive(emptyState())
  const loading = ref(false)
  // Honest transport / non-2xx error from the GET. Distinct from a backend
  // "unavailable" state, which means an active game whose state could not be read.
  const loadError = ref('')
  // The result of the owner's last adopt/release; never auto-replayed.
  const actionError = ref('')
  const acting = ref(false)
  const loaded = ref(false)
  let request = null
  let timer = null
  let failures = 0
  let mounted = false
  let disposed = false
  let started = false
  // The session a surfaced actionError belongs to. The error outlasts revision
  // and unavailable flaps within that session; only a new action, a success or a
  // different session (new stream, host restart, session ended) drops it.
  let errorSession = null

  function emptyState() {
    return {
      available: false, app_uuid: '', app_name: '', session_id: '', revision: '',
      state: '', runtime: 'unknown', can_adopt: false, can_release: false, message: '',
      uninspected_process_count: 0,
    }
  }

  function hidden() {
    return typeof document !== 'undefined' && document.hidden
  }

  async function load() {
    if (disposed) return false
    loading.value = true
    request?.abort()
    const current = new AbortController()
    request = current
    const timeout = setTimeout(() => current.abort(), timeoutMs)
    try {
      const response = await fetch('./api/epic/session', { credentials: 'include', cache: 'no-store', signal: current.signal })
      if (disposed || request !== current) return false
      if (!response.ok) {
        let message = ''
        try { const body = await response.json(); message = body?.error || '' } catch (e) { /* non-JSON error body */ }
        failures += 1
        loadError.value = message || ('Could not read the Epic session (' + response.status + ').')
        return false
      }
      const data = await response.json()
      if (disposed || request !== current) return false
      failures = 0
      loadError.value = ''
      loaded.value = true
      if (!data || data.available !== true) {
        Object.assign(state, emptyState())
      } else {
        const count = data.uninspected_process_count === undefined ? 0 : data.uninspected_process_count
        const validCount = Number.isSafeInteger(count) && count >= 0 && count <= 8192
        Object.assign(state, emptyState(), {
          available: true,
          app_uuid: data.app_uuid || '',
          app_name: data.app_name || '',
          session_id: data.session_id || '',
          revision: data.revision || '',
          // An unrecognized state is shown as unavailable, never guessed as managed.
          state: validCount && VALID_STATES.has(data.state) ? data.state : 'unavailable',
          runtime: data.runtime || 'unknown',
          uninspected_process_count: validCount ? count : 0,
          can_adopt: validCount && Boolean(data.can_adopt),
          can_release: data.can_release === true,
          message: data.message || '',
        })
      }
      // Drop a stale action error once the session itself has moved on.
      if (actionError.value && errorSession !== null && errorSession !== state.session_id) {
        actionError.value = ''
        errorSession = null
      }
      return true
    } catch (cause) {
      if (disposed || request !== current) return false
      failures += 1
      if (cause?.name !== 'AbortError') loadError.value = 'Epic session status is not available right now.'
      return false
    } finally {
      clearTimeout(timeout)
      if (request === current) loading.value = false
    }
  }

  async function post(payload) {
    acting.value = true
    actionError.value = ''
    errorSession = null
    try {
      const response = await fetch('./api/epic/session', {
        credentials: 'include',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
      })
      let body = null
      try { body = await response.json() } catch (e) { /* tolerate empty/non-JSON body */ }
      if (response.ok && body && body.status) {
        // Read back the backend's honest state; never replay the action itself.
        await load()
        return true
      }
      actionError.value = (body && body.error) || ('Polaris could not apply that (' + response.status + ').')
      errorSession = payload.session_id
      // Reflect the real current state after a refusal; still no replay.
      await load()
      return false
    } catch (e) {
      actionError.value = 'That request could not be sent. Try again.'
      errorSession = payload.session_id
      return false
    } finally {
      acting.value = false
    }
  }

  /**
   * The explicit owner grant. Only an adoptable, confirmable, fully identified
   * session is sent, with confirm:true and the exact current session/revision.
   * The two-step confirmation lives in the panel; this is the confirmed step.
   */
  async function adopt() {
    if (acting.value) return false
    if (!state.available || !state.can_adopt || state.state !== 'confirmable' || !state.session_id || !state.revision) {
      actionError.value = 'This game can no longer be adopted. Refresh and try again.'
      errorSession = state.session_id
      return false
    }
    return post({ action: 'adopt', session_id: state.session_id, revision: state.revision, confirm: true })
  }

  /** Leave the game running and clear the grant only. Never signals the game. */
  async function release() {
    if (acting.value) return false
    if (!state.available || !state.session_id || !state.revision) {
      actionError.value = 'There is no current Epic session to leave.'
      errorSession = state.session_id
      return false
    }
    return post({ action: 'release', session_id: state.session_id, revision: state.revision })
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
    if (!mounted || !started || hidden() || options.shouldPoll?.() === false) return
    timer = setTimeout(poll, nextDelayMs())
  }

  async function poll() {
    timer = null
    if (!mounted || !started || hidden() || options.shouldPoll?.() === false) return
    if (!loading.value) await load()
    schedule()
  }

  function handleVisibility() {
    if (hidden()) stopTimer()
    else poll()
  }

  async function start() {
    if (disposed) return false
    started = true
    const ok = await load()
    schedule()
    return ok
  }

  function stop() {
    started = false
    stopTimer()
    request?.abort()
    request = null
    loading.value = false
  }

  onMounted(() => {
    mounted = true
    if (typeof document !== 'undefined') document.addEventListener('visibilitychange', handleVisibility)
  })
  onUnmounted(() => {
    mounted = false
    disposed = true
    stop()
    if (typeof document !== 'undefined') document.removeEventListener('visibilitychange', handleVisibility)
  })

  return { state, loading, loadError, actionError, acting, loaded, load, adopt, release, start, stop }
}
