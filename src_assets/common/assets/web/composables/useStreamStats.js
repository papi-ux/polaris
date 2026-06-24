import { ref, onMounted, onUnmounted } from 'vue'

/**
 * Composable for real-time stream statistics using Server-Sent Events (SSE).
 *
 * Connects to the SSE endpoint for push-based updates.
 * Falls back to HTTP polling if SSE fails to connect after the first attempt.
 * The fallback loop waits for each request to settle and backs off after failures.
 *
 * @param {number} pollFallbackMs - Polling interval in ms if SSE is unavailable (default: 1000)
 * @param {{ pauseWhenHidden?: boolean, maxFallbackBackoffMs?: number }} options
 * @returns {{ stats: Ref, connected: Ref<boolean> }}
 */
export function useStreamStats(pollFallbackMs = 1000, options = {}) {
  const stats = ref(null)
  const connected = ref(false)

  const maxFallbackBackoffMs = Math.max(
    pollFallbackMs,
    options.maxFallbackBackoffMs ?? pollFallbackMs * 8,
  )

  let eventSource = null
  let pollTimer = null
  let fallbackInFlight = null
  let useFallback = false
  let consecutiveFallbackFailures = 0

  function shouldPauseForVisibility() {
    return options.pauseWhenHidden !== false && typeof document !== 'undefined' && document.hidden
  }

  function fallbackDelayMs() {
    if (consecutiveFallbackFailures <= 0) {
      return pollFallbackMs
    }
    return Math.min(maxFallbackBackoffMs, pollFallbackMs * (2 ** consecutiveFallbackFailures))
  }

  /**
   * Start SSE connection to the stream-sse endpoint.
   */
  function connectSSE() {
    if (eventSource || shouldPauseForVisibility()) return

    // EventSource sends cookies automatically for the already-loaded Polaris UI origin.
    eventSource = new EventSource('./api/stats/stream-sse')

    eventSource.onopen = () => {
      connected.value = true
    }

    eventSource.onmessage = (event) => {
      try {
        stats.value = JSON.parse(event.data)
        connected.value = true
      } catch {
        // Ignore malformed messages.
      }
    }

    let sseErrorCount = 0
    eventSource.onerror = () => {
      connected.value = false
      sseErrorCount++

      // If there are too many errors, give up on SSE and fall back to polling.
      // This prevents hammering the server when auth has expired or it is under load.
      if (sseErrorCount > 5 || (!stats.value && !useFallback)) {
        useFallback = true
        cleanupSSE()
        if (!shouldPauseForVisibility()) {
          startPolling()
        }
      }
    }
  }

  /**
   * Clean up the SSE connection.
   */
  function cleanupSSE() {
    if (eventSource) {
      eventSource.close()
      eventSource = null
    }
  }

  function cleanupPolling() {
    if (pollTimer) {
      clearTimeout(pollTimer)
      pollTimer = null
    }
  }

  function scheduleNextPoll(delayMs = fallbackDelayMs()) {
    cleanupPolling()
    if (!useFallback || shouldPauseForVisibility()) {
      return
    }

    pollTimer = setTimeout(async () => {
      pollTimer = null
      const keepPolling = await fetchStats()
      if (keepPolling) {
        scheduleNextPoll()
      }
    }, delayMs)
  }

  /**
   * Fallback: poll the JSON endpoint after each prior request settles.
   */
  async function fetchStats() {
    if (shouldPauseForVisibility()) return false
    if (fallbackInFlight) return fallbackInFlight

    fallbackInFlight = (async () => {
      let ok = false
      let keepPolling = true

      try {
        const res = await fetch('./api/stats/stream', { credentials: 'include' })
        if (res.ok) {
          stats.value = await res.json()
          connected.value = true
          ok = true
        } else if (res.status === 401) {
          // Session expired so stop polling and redirect to login.
          cleanupPolling()
          cleanupSSE()
          window.location.hash = '#/login'
          keepPolling = false
        } else {
          connected.value = false
        }
      } catch {
        connected.value = false
      } finally {
        if (keepPolling) {
          consecutiveFallbackFailures = ok ? 0 : consecutiveFallbackFailures + 1
        }
        fallbackInFlight = null
      }

      return keepPolling
    })()

    return fallbackInFlight
  }

  function startPolling() {
    if (pollTimer || shouldPauseForVisibility()) return
    fetchStats().then((keepPolling) => {
      if (keepPolling) {
        scheduleNextPoll()
      }
    })
  }

  function start() {
    if (shouldPauseForVisibility()) return
    if (useFallback) startPolling()
    else connectSSE()
  }

  function stop() {
    cleanupSSE()
    cleanupPolling()
    connected.value = false
  }

  function handleVisibilityChange() {
    if (shouldPauseForVisibility()) {
      stop()
    } else {
      start()
    }
  }

  onMounted(() => {
    start()
    if (options.pauseWhenHidden !== false && typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', handleVisibilityChange)
    }
  })

  onUnmounted(() => {
    stop()
    if (options.pauseWhenHidden !== false && typeof document !== 'undefined') {
      document.removeEventListener('visibilitychange', handleVisibilityChange)
    }
  })

  return { stats, connected }
}
