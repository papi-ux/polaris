import { ref, onMounted, onUnmounted, watch } from 'vue'

/**
 * Composable for polling system hardware stats (GPU telemetry).
 *
 * Fetches from /api/stats/system at a configurable interval.
 *
 * @param {number} intervalMs - Poll interval in ms (default: 3000)
 * @param {{ shouldPoll?: (() => boolean) | { value: boolean }, pauseWhenHidden?: boolean }} options
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
    if (!resolveShouldPoll()) return

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

  function handleVisibilityChange() {
    if (resolveShouldPoll()) {
      fetchStats()
      startTimer()
    } else {
      stopTimer()
    }
  }

  onMounted(() => {
    if (resolveShouldPoll()) {
      fetchStats()
      startTimer()
    }

    if (options.pauseWhenHidden !== false && typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', handleVisibilityChange)
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
    if (options.pauseWhenHidden !== false && typeof document !== 'undefined') {
      document.removeEventListener('visibilitychange', handleVisibilityChange)
    }
    if (stopPollingWatcher) {
      stopPollingWatcher()
      stopPollingWatcher = null
    }
  })

  return { gpu, displays, audio, sessionType, loading }
}
