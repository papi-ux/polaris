import { ref, onMounted, onUnmounted } from 'vue'

/**
 * Composable for real-time stream statistics using Server-Sent Events (SSE).
 *
 * Connects to the SSE endpoint for push-based updates (~1s interval, no polling overhead).
 * Falls back to HTTP polling if SSE fails to connect after the first attempt.
 *
 * @param {number} pollFallbackMs - Polling interval in ms if SSE is unavailable (default: 1000)
 * @param {{ pauseWhenHidden?: boolean }} options
 * @returns {{ stats: Ref, connected: Ref<boolean> }}
 */
export function useStreamStats(pollFallbackMs = 1000, options = {}) {
  const stats = ref(null)
  const connected = ref(false)

  let eventSource = null
  let pollTimer = null
  let useFallback = false

  function shouldPauseForVisibility() {
    return options.pauseWhenHidden !== false && typeof document !== 'undefined' && document.hidden
  }

  /**
   * Start SSE connection to the stream-sse endpoint.
   */
  function connectSSE() {
    if (eventSource || shouldPauseForVisibility()) return

    // EventSource does not send cookies over HTTPS with self-signed certs in some browsers,
    // but since the Polaris web UI is already loaded over HTTPS with the cert accepted,
    // cookies (including the auth session cookie) are sent automatically.
    eventSource = new EventSource('./api/stats/stream-sse')

    eventSource.onopen = () => {
      connected.value = true
    }

    eventSource.onmessage = (event) => {
      try {
        stats.value = JSON.parse(event.data)
        connected.value = true
      } catch {
        // Ignore malformed messages
      }
    }

    let sseErrorCount = 0
    eventSource.onerror = () => {
      connected.value = false
      sseErrorCount++

      // If we've had too many errors, give up on SSE and fall back to polling.
      // This prevents hammering the server when auth has expired or it's under load.
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

  /**
   * Fallback: poll the JSON endpoint at a regular interval.
   */
  async function fetchStats() {
    if (shouldPauseForVisibility()) return

    try {
      const res = await fetch('./api/stats/stream', { credentials: 'include' })
      if (res.ok) {
        stats.value = await res.json()
        connected.value = true
      } else if (res.status === 401) {
        // Session expired — stop polling, redirect to login
        cleanupPolling()
        cleanupSSE()
        window.location.hash = '#/login'
        return
      } else {
        connected.value = false
      }
    } catch {
      connected.value = false
    }
  }

  function startPolling() {
    if (pollTimer || shouldPauseForVisibility()) return
    fetchStats()
    pollTimer = setInterval(fetchStats, pollFallbackMs)
  }

  function cleanupPolling() {
    if (pollTimer) {
      clearInterval(pollTimer)
      pollTimer = null
    }
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
