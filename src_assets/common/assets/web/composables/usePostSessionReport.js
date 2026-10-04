import { computed, shallowRef, watch } from 'vue'
import { buildPostSessionStreamReport, captureLiveSessionEvidence } from '../diagnostics-export.js'

export function usePostSessionReport(stats, connected, logs) {
  const completed = shallowRef(null)
  let active = null
  watch(connected, (isConnected) => { if (!isConnected) active = null }, { flush: 'sync' })
  watch([stats, connected], ([next, isConnected]) => {
    if (!isConnected) {
      // Reconnection cannot prove that the cached stream was the one that ended.
      active = null
      return
    }
    if (next?.streaming === true) {
      active = captureLiveSessionEvidence({
        stats: next, connected: isConnected, logs: logs.value,
        capturedAt: new Date().toISOString(), previous: active,
      })
    } else {
      if (active && next?.streaming === false && next.last_session?.stream_instance_id === active.streamId) {
        completed.value = { ...active, origin: 'completed',
          disconnectReason: 'Stream telemetry changed from active to idle.' }
      }
      active = null
    }
  }, { immediate: true, deep: true })
  return computed(() => buildPostSessionStreamReport({ snapshot: completed.value, lastSession: stats.value?.last_session }))
}
