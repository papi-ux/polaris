import { computed, shallowRef, watch } from 'vue'
import { buildPostSessionStreamReport, captureLiveSessionEvidence } from '../diagnostics-export.js'

// onCompleted hears each completion as it is recorded, with the detached snapshot and the idle
// report that ended it; a page uses it to mark a stream it watched end, and nothing else does.
export function usePostSessionReport(stats, connected, logs, { onCompleted } = {}) {
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
        onCompleted?.(completed.value, next)
      }
      active = null
    }
  }, { immediate: true, deep: true })
  return computed(() => buildPostSessionStreamReport({ snapshot: completed.value, lastSession: stats.value?.last_session }))
}
