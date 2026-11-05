import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { normalizeTailscalePeer } from '../tailscale-connection-readout.js'

// The host's Tailscale evidence for the stream in `stats`, refreshed while it streams. An answer
// for another stream, or one older than the host's own limit, reads as not confirmed.
export function useTailscalePeer(stats) {
  const raw = ref(null)
  const now = ref(Date.now())
  let receivedAt = 0
  let activeRequest = null
  let disposed = false
  const evidence = computed(() => normalizeTailscalePeer(
    performance.now() - receivedAt <= 10000 ? raw.value : null,
    { now: now.value, generation: stats.value?.stream_instance_id, streaming: stats.value?.streaming === true },
  ))

  async function refresh({ restart = false } = {}) {
    if (disposed) return
    if (activeRequest) {
      // A periodic refresh waits for the request already out; a new stream does not.
      if (!restart) return
      activeRequest.abort()
      activeRequest = null
    }
    if (!stats.value?.streaming) { raw.value = null; return }
    const controller = new AbortController()
    activeRequest = controller
    const current = () => !disposed && activeRequest === controller
    const timeout = setTimeout(() => controller.abort(), 3000)
    try {
      const response = await fetch('./api/network/connection', { credentials: 'include', signal: controller.signal, cache: 'no-store' })
      const value = response.ok ? await response.json() : null
      if (current()) { raw.value = value; receivedAt = performance.now(); now.value = Date.now() }
    } catch {
      if (current()) raw.value = null
    } finally {
      clearTimeout(timeout)
      if (activeRequest === controller) activeRequest = null
    }
  }
  // A stream change abandons any request made for the previous stream and asks again at once.
  watch(() => [stats.value?.streaming, stats.value?.stream_instance_id], () => { raw.value = null; refresh({ restart: true }) }, { immediate: true })
  const refreshTimer = setInterval(refresh, 5000)
  const expiryTimer = setInterval(() => { now.value = Date.now() }, 1000)
  onBeforeUnmount(() => {
    disposed = true
    activeRequest?.abort()
    clearInterval(refreshTimer)
    clearInterval(expiryTimer)
  })
  return evidence
}
