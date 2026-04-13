import { ref, watch } from 'vue'

const STORAGE_KEY = 'polaris_session_history'
const MAX_SESSIONS = 50

/**
 * Composable for tracking streaming session history.
 *
 * Watches a stats ref and automatically records sessions:
 * - When streaming starts: records start time, client, codec, resolution
 * - While streaming: accumulates FPS, bitrate, latency, encode time samples
 * - When streaming ends: computes averages, quality grade, saves to localStorage
 *
 * @param {Ref} stats - Reactive stream stats ref from useStreamStats
 * @returns {{ sessions: Ref, clearHistory: Function }}
 */
export function useSessionHistory(stats) {
  const sessions = ref(loadSessions())

  let currentSession = null
  let samples = null

  function loadSessions() {
    try {
      return JSON.parse(localStorage.getItem(STORAGE_KEY) || '[]')
    } catch {
      return []
    }
  }

  function saveSessions() {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(sessions.value.slice(0, MAX_SESSIONS)))
    } catch {}
  }

  function startSession(s) {
    currentSession = {
      started_at: Date.now(),
      client_name: s.client_name || 'Unknown',
      client_ip: s.client_ip || '',
      codec: s.codec || '',
      width: s.width || 0,
      height: s.height || 0,
    }
    samples = { fps: [], bitrate: [], latency: [], encode: [], packet_loss: [] }
  }

  function recordSample(s) {
    if (!samples) return
    if (s.fps > 0) samples.fps.push(s.fps)
    if (s.bitrate_kbps > 0) samples.bitrate.push(s.bitrate_kbps)
    if (s.latency_ms >= 0) samples.latency.push(s.latency_ms)
    if (s.encode_time_ms >= 0) samples.encode.push(s.encode_time_ms)
    if (s.packet_loss >= 0) samples.packet_loss.push(s.packet_loss)
  }

  function endSession() {
    if (!currentSession || !samples) return

    const avg = (arr) => arr.length ? arr.reduce((a, b) => a + b, 0) / arr.length : 0
    const duration_s = Math.round((Date.now() - currentSession.started_at) / 1000)

    // Don't save very short sessions (< 5 seconds)
    if (duration_s < 5) {
      currentSession = null
      samples = null
      return
    }

    const avgFps = avg(samples.fps)
    const avgEncode = avg(samples.encode)
    const avgLatency = avg(samples.latency)
    const avgLoss = avg(samples.packet_loss)

    // Compute quality score (same algorithm as dashboard)
    let score = 100
    if (avgFps < 55) score -= Math.min(30, (60 - avgFps) * 2)
    else if (avgFps < 58) score -= 5
    if (avgEncode > 16) score -= 25
    else if (avgEncode > 8) score -= Math.min(15, (avgEncode - 8) * 2)
    if (avgLatency > 50) score -= 20
    else if (avgLatency > 20) score -= Math.min(10, (avgLatency - 20) / 3)
    if (avgLoss > 5) score -= 30
    else if (avgLoss > 1) score -= avgLoss * 5
    score = Math.max(0, Math.min(100, Math.round(score)))

    const grade = score >= 90 ? 'A' : score >= 75 ? 'B' : score >= 55 ? 'C' : score >= 35 ? 'D' : 'F'

    const session = {
      ...currentSession,
      ended_at: Date.now(),
      duration_s,
      avg_fps: Math.round(avgFps * 10) / 10,
      avg_bitrate_kbps: Math.round(avg(samples.bitrate)),
      avg_latency_ms: Math.round(avgLatency * 10) / 10,
      avg_encode_ms: Math.round(avgEncode * 10) / 10,
      avg_packet_loss: Math.round(avgLoss * 100) / 100,
      quality_score: score,
      quality_grade: grade,
    }

    sessions.value.unshift(session)
    if (sessions.value.length > MAX_SESSIONS) sessions.value.length = MAX_SESSIONS
    saveSessions()

    currentSession = null
    samples = null
  }

  function clearHistory() {
    sessions.value = []
    saveSessions()
  }

  // Watch stats for session start/end
  watch(stats, (newStats, oldStats) => {
    if (!newStats) return

    if (newStats.streaming && (!oldStats || !oldStats.streaming)) {
      startSession(newStats)
    }

    if (newStats.streaming && currentSession) {
      recordSample(newStats)
    }

    if (!newStats.streaming && oldStats?.streaming) {
      endSession()
    }
  })

  return { sessions, clearHistory }
}

/**
 * Format seconds into human-readable duration.
 */
export function formatDuration(seconds) {
  if (seconds < 60) return `${seconds}s`
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ${seconds % 60}s`
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  return `${h}h ${m}m`
}
