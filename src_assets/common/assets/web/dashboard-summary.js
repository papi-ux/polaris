import { judgedVideoFrameLoss } from './diagnostics-export.js'

export function metricNumber(value) {
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : 0
}

export function formatLiveNumber(value, digits = 1, suffix = '') {
  const parsed = Number(value)
  if (!Number.isFinite(parsed)) return '--'
  return `${parsed.toFixed(digits)}${suffix}`
}

// The loss the Dashboard shows, charts and grades: the share of video frames Doctor judged lost after
// FEC over the host's window, the figure Doctor and the session status quote, or null
// until the host has judged any. The newest one second report, packet_loss, swings between 0 and 7%
// on a Wi-Fi burst, and moved the tile, its colour and the grade with it.
export function dashboardLossPct(statsPayload = {}) {
  return judgedVideoFrameLoss(statsPayload)?.pct ?? null
}

export function buildQualityScore(statsPayload = {}) {
  if (!statsPayload.streaming) return 0
  let score = 100
  const loss = dashboardLossPct(statsPayload) ?? 0

  if (statsPayload.fps < 55) score -= Math.min(30, (60 - statsPayload.fps) * 2)
  else if (statsPayload.fps < 58) score -= 5

  if (statsPayload.encode_time_ms > 16) score -= 25
  else if (statsPayload.encode_time_ms > 8) score -= Math.min(15, (statsPayload.encode_time_ms - 8) * 2)

  if (statsPayload.latency_ms > 50) score -= 20
  else if (statsPayload.latency_ms > 20) score -= Math.min(10, (statsPayload.latency_ms - 20) / 3)

  if (loss > 5) score -= 30
  else if (loss > 1) score -= loss * 5
  else if (loss > 0) score -= 3

  return Math.max(0, Math.min(100, Math.round(score)))
}

export function buildQualityGrade(score) {
  if (score >= 90) return 'A'
  if (score >= 75) return 'B'
  if (score >= 55) return 'C'
  if (score >= 35) return 'D'
  return 'F'
}

export function buildLiveSummary({ stats = {}, qualityGrade = 'F', qualityScore = 0, gradeTone = '' } = {}) {
  const fps = metricNumber(stats.fps)
  const targetFps = metricNumber(stats.session_target_fps || stats.requested_client_fps)
  const latency = Number(stats.latency_ms)
  const judgedLoss = judgedVideoFrameLoss(stats)
  const bitrate = Number(stats.bitrate_kbps)

  return {
    quality: `${qualityGrade} · ${qualityScore}`,
    qualityDetail: qualityScore >= 90 ? 'Locked in' : qualityScore >= 75 ? 'Playable' : 'Needs tuning',
    qualityTone: gradeTone,
    latency: formatLiveNumber(latency, 0, ' ms'),
    latencyTone: Number.isFinite(latency)
      ? (latency <= 20 ? 'text-success' : latency <= 50 ? 'text-warning' : 'text-danger')
      : 'text-storm',
    fps: formatLiveNumber(fps, 1),
    fpsTone: fps > 0 ? (fps >= 55 ? 'text-success' : fps >= 30 ? 'text-warning' : 'text-danger') : 'text-storm',
    fpsDetail: targetFps > 0 ? `${targetFps.toFixed(0)} target` : 'Encoded',
    // Coloured as Doctor judges it: pressure, light loss below it, or clean.
    loss: judgedLoss ? formatLiveNumber(judgedLoss.pct, 1, '%') : '--',
    lossTone: judgedLoss
      ? (judgedLoss.state === 'elevated' ? 'text-danger' : judgedLoss.state === 'light' ? 'text-warning' : 'text-success')
      : 'text-storm',
    lossTitle: judgedLoss
      ? `Video frames lost after FEC over the last ${judgedLoss.windowSeconds} s`
      : 'Video frame loss is not judged yet',
    bitrate: Number.isFinite(bitrate) ? `${(bitrate / 1000).toFixed(1)} Mbps` : '--',
  }
}


