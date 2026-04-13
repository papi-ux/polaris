import { ref, onMounted, onUnmounted } from 'vue'

/**
 * Composable for real-time stream statistics using Server-Sent Events (SSE).
 *
 * Connects to the SSE endpoint for push-based updates (~1s interval, no polling overhead).
 * Falls back to HTTP polling if SSE fails to connect after the first attempt.
 *
 * @param {number} pollFallbackMs - Polling interval in ms if SSE is unavailable (default: 1000)
 * @returns {{ stats: Ref, connected: Ref<boolean> }}
 */
export function useStreamStats(pollFallbackMs = 1000) {
  const stats = ref(null)
  const connected = ref(false)

  let eventSource = null
  let pollTimer = null
  let useFallback = false

  /**
   * Start SSE connection to the stream-sse endpoint.
   */
  function connectSSE() {
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
        startPolling()
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
    try {
      const res = await fetch('./api/stats/stream', { credentials: 'include' })
      if (res.ok) {
        stats.value = await res.json()
        connected.value = true
      } else if (res.status === 401) {
        // Session expired — stop polling, redirect to login
        stopPolling()
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
    fetchStats()
    pollTimer = setInterval(fetchStats, pollFallbackMs)
  }

  function cleanupPolling() {
    if (pollTimer) {
      clearInterval(pollTimer)
      pollTimer = null
    }
  }

  onMounted(() => {
    connectSSE()
  })

  onUnmounted(() => {
    cleanupSSE()
    cleanupPolling()
  })

  return { stats, connected }
}
