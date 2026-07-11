import { ref, onMounted, onUnmounted, watch } from 'vue'

/**
 * Composable for polling system hardware stats (GPU telemetry).
 *
 * Fetches from /api/stats/system at a configurable interval.
 * Uses a completion-driven timeout loop so slow or failing requests do not overlap.
 *
 * @param {number} intervalMs - Poll interval in ms (default: 3000)
 * @param {{ shouldPoll?: (() => boolean) | { value: boolean }, pauseWhenHidden?: boolean, maxBackoffMs?: number }} options
 * @returns {{ gpu: Ref, displays: Ref, audio: Ref, sessionType: Ref, displaySession: Ref, loading: Ref<boolean> }}
 */
export function useSystemStats(intervalMs = 3000, options = {}) {
  const gpu = ref(null)
  const displays = ref([])
  const audio = ref(null)
  const sessionType = ref(null)
  const displaySession = ref(null)
  const loading = ref(true)

  const maxBackoffMs = Math.max(intervalMs, options.maxBackoffMs ?? intervalMs * 8)

  let timer = null
  let inFlight = null
  let stopPollingWatcher = null
  let consecutiveFailures = 0
  let mounted = false

  function shouldPauseForVisibility() {
    return options.pauseWhenHidden !== false && typeof document !== 'undefined' && document.hidden
  }

  function resolveShouldPoll() {
    if (shouldPauseForVisibility()) {
      return false
    }
    if (typeof options.shouldPoll === 'function') {
      return Boolean(options.shouldPoll())
    }
    if (options.shouldPoll && typeof options.shouldPoll === 'object' && 'value' in options.shouldPoll) {
      return Boolean(options.shouldPoll.value)
    }
    return true
  }

  function nextDelayMs() {
    if (consecutiveFailures <= 0) {
      return intervalMs
    }
    return Math.min(maxBackoffMs, intervalMs * (2 ** consecutiveFailures))
  }

  function stopTimer() {
    if (timer) {
      clearTimeout(timer)
      timer = null
    }
  }

  function scheduleNextPoll(delayMs = nextDelayMs()) {
    stopTimer()
    if (!mounted || !resolveShouldPoll()) {
      return
    }

    timer = setTimeout(async () => {
      timer = null
      await pollNow()
    }, delayMs)
  }

  async function fetchStats() {
    if (!resolveShouldPoll()) return false
    if (inFlight) return inFlight

    inFlight = (async () => {
      let ok = false
      try {
        const res = await fetch('./api/stats/system', { credentials: 'include' })
        if (res.ok) {
          const data = await res.json()
          gpu.value = data.gpu
          displays.value = data.displays || []
          audio.value = data.audio || null
          sessionType.value = data.session_type || null
          displaySession.value = data.display_session || null
          ok = true
        }
      } catch {
        // Silently fail because GPU stats are non-critical.
      } finally {
        consecutiveFailures = ok ? 0 : consecutiveFailures + 1
        loading.value = false
        inFlight = null
      }

      return ok
    })()

    return inFlight
  }

  async function pollNow() {
    stopTimer()
    if (!mounted || !resolveShouldPoll()) return

    await fetchStats()
    scheduleNextPoll()
  }

  function handleVisibilityChange() {
    if (resolveShouldPoll()) {
      pollNow()
    } else {
      stopTimer()
    }
  }

  onMounted(() => {
    mounted = true
    if (resolveShouldPoll()) {
      pollNow()
    }

    if (options.pauseWhenHidden !== false && typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', handleVisibilityChange)
    }

    if (options.shouldPoll) {
      stopPollingWatcher = watch(resolveShouldPoll, (enabled) => {
        if (enabled) {
          pollNow()
        } else {
          stopTimer()
        }
      })
    }
  })

  onUnmounted(() => {
    mounted = false
    stopTimer()
    if (options.pauseWhenHidden !== false && typeof document !== 'undefined') {
      document.removeEventListener('visibilitychange', handleVisibilityChange)
    }
    if (stopPollingWatcher) {
      stopPollingWatcher()
      stopPollingWatcher = null
    }
  })

  return { gpu, displays, audio, sessionType, displaySession, loading }
}
