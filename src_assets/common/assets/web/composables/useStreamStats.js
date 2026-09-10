import { ref, onMounted, onUnmounted } from 'vue'

// One transport per open host UI, shared by dashboard, Doctor and tuning controls.
let shared = null
function createChannel(pollMs, options) {
  const stats = ref(null)
  const connected = ref(false)
  let users = 0
  let source = null
  let timer = null
  let watchdog = null
  let controller = null
  let epoch = 0
  let failures = 0
  let fallback = false
  const paused = () => options.pauseWhenHidden !== false && document.hidden
  const active = () => users > 0 && !paused()
  const maxDelay = Math.max(pollMs, options.maxFallbackBackoffMs ?? pollMs * 8)

  function stop() {
    ++epoch
    source?.close()
    source = null
    controller?.abort()
    controller = null
    clearTimeout(timer)
    clearTimeout(watchdog)
    timer = watchdog = null
    connected.value = false
  }
  function receive(value) {
    stats.value = value
    connected.value = true
    failures = 0
    clearTimeout(watchdog)
    watchdog = setTimeout(() => {
      stop()
      fallback = true
      start()
    }, Math.max(5000, pollMs * 3))
  }
  async function poll() {
    if (!active() || controller) return
    const current = epoch
    const abort = new AbortController()
    controller = abort
    try {
      const response = await fetch('./api/stats/stream', { credentials: 'include', signal: abort.signal })
      if (current !== epoch || !active()) return
      if (response.status === 401) {
        stop()
        window.location.hash = '#/login'
        return
      }
      if (!response.ok) throw new Error('stats-unavailable')
      const value = await response.json()
      if (current === epoch && active()) receive(value)
    } catch {
      if (current === epoch) { connected.value = false; failures++ }
    } finally {
      if (controller === abort) controller = null
      if (current === epoch && active()) {
        timer = setTimeout(poll, Math.min(maxDelay, pollMs * (2 ** failures)))
      }
    }
  }
  function start() {
    if (!active() || source || controller) return
    if (fallback || typeof EventSource !== 'function') { poll(); return }
    const current = epoch
    const connection = new EventSource('./api/stats/stream-sse')
    source = connection
    connection.onmessage = event => {
      if (current !== epoch || !active()) return
      try { receive(JSON.parse(event.data)) } catch { connected.value = false }
    }
    connection.onerror = () => {
      if (current !== epoch) return
      stop()
      fallback = true
      start()
    }
    watchdog = setTimeout(() => { stop(); fallback = true; start() }, 5000)
  }
  function visibility() { stop(); fallback = false; start() }
  return {
    stats, connected,
    retain() {
      if (++users === 1) {
        document.addEventListener('visibilitychange', visibility)
        start()
      }
    },
    release() {
      if (--users === 0) {
        stop()
        document.removeEventListener('visibilitychange', visibility)
        shared = null
      }
    },
  }
}
export function useStreamStats(pollFallbackMs = 1000, options = {}) {
  const channel = shared || (shared = createChannel(pollFallbackMs, options))
  onMounted(channel.retain)
  onUnmounted(channel.release)
  return { stats: channel.stats, connected: channel.connected }
}
