import { ref, onMounted, onUnmounted, watch } from 'vue'

/**
 * Composable for polling system hardware stats (GPU telemetry).
 *
 * Fetches from /api/stats/system at a configurable interval.
 *
 * @param {number} intervalMs - Poll interval in ms (default: 3000)
 * @param {{ shouldPoll?: (() => boolean) | { value: boolean } }} options
 * @returns {{ gpu: Ref, loading: Ref<boolean> }}
 */
export function useSystemStats(intervalMs = 3000, options = {}) {
  const gpu = ref(null)
  const displays = ref([])
  const audio = ref(null)
  const sessionType = ref(null)
  const loading = ref(true)

  let timer = null
  let stopPollingWatcher = null

  function resolveShouldPoll() {
    if (typeof options.shouldPoll === 'function') {
      return Boolean(options.shouldPoll())
    }
    if (options.shouldPoll && typeof options.shouldPoll === 'object' && 'value' in options.shouldPoll) {
      return Boolean(options.shouldPoll.value)
    }
    return true
  }

  function stopTimer() {
    if (timer) {
      clearInterval(timer)
      timer = null
    }
  }

  function startTimer() {
    if (timer) return
    timer = setInterval(() => {
      if (resolveShouldPoll()) {
        fetchStats()
      }
    }, intervalMs)
  }

  async function fetchStats() {
    try {
      const res = await fetch('./api/stats/system', { credentials: 'include' })
      if (res.ok) {
        const data = await res.json()
        gpu.value = data.gpu
        displays.value = data.displays || []
        audio.value = data.audio || null
        sessionType.value = data.session_type || null
      }
    } catch {
      // Silently fail — GPU stats are non-critical
    }
    loading.value = false
  }

  onMounted(() => {
    if (resolveShouldPoll()) {
      fetchStats()
      startTimer()
    }

    if (options.shouldPoll) {
      stopPollingWatcher = watch(resolveShouldPoll, (enabled) => {
        if (enabled) {
          fetchStats()
          startTimer()
        } else {
          stopTimer()
        }
      })
    }
  })

  onUnmounted(() => {
    stopTimer()
    if (stopPollingWatcher) {
      stopPollingWatcher()
      stopPollingWatcher = null
    }
  })

  return { gpu, displays, audio, sessionType, loading }
}
