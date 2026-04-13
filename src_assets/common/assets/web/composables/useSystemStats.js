import { ref, onMounted, onUnmounted } from 'vue'

/**
 * Composable for polling system hardware stats (GPU telemetry).
 *
 * Fetches from /api/stats/system at a configurable interval.
 *
 * @param {number} intervalMs - Poll interval in ms (default: 3000)
 * @returns {{ gpu: Ref, loading: Ref<boolean> }}
 */
export function useSystemStats(intervalMs = 3000) {
  const gpu = ref(null)
  const displays = ref([])
  const audio = ref(null)
  const sessionType = ref(null)
  const loading = ref(true)

  let timer = null

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
    fetchStats()
    timer = setInterval(fetchStats, intervalMs)
  })

  onUnmounted(() => {
    if (timer) {
      clearInterval(timer)
      timer = null
    }
  })

  return { gpu, displays, audio, sessionType, loading }
}
