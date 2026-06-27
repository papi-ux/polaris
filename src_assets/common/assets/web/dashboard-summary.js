export function metricNumber(value) {
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : 0
}

export function formatLiveNumber(value, digits = 1, suffix = '') {
  const parsed = Number(value)
  if (!Number.isFinite(parsed)) return '--'
  return `${parsed.toFixed(digits)}${suffix}`
}

export function buildQualityScore(statsPayload = {}) {
  if (!statsPayload.streaming) return 0
  let score = 100

  if (statsPayload.fps < 55) score -= Math.min(30, (60 - statsPayload.fps) * 2)
  else if (statsPayload.fps < 58) score -= 5

  if (statsPayload.encode_time_ms > 16) score -= 25
  else if (statsPayload.encode_time_ms > 8) score -= Math.min(15, (statsPayload.encode_time_ms - 8) * 2)

  if (statsPayload.latency_ms > 50) score -= 20
  else if (statsPayload.latency_ms > 20) score -= Math.min(10, (statsPayload.latency_ms - 20) / 3)

  if (statsPayload.packet_loss > 5) score -= 30
  else if (statsPayload.packet_loss > 1) score -= statsPayload.packet_loss * 5
  else if (statsPayload.packet_loss > 0) score -= 3

  return Math.max(0, Math.min(100, Math.round(score)))
}

export function buildQualityGrade(score) {
  if (score >= 90) return 'A'
  if (score >= 75) return 'B'
  if (score >= 55) return 'C'
  if (score >= 35) return 'D'
  return 'F'
}

export function buildFpsTargetGap(statsPayload = {}) {
  if (!statsPayload.streaming) return null
  const encoded = metricNumber(statsPayload.fps)
  const target = metricNumber(statsPayload.session_target_fps || statsPayload.requested_client_fps)

  if (target < 90 || encoded <= 0 || encoded >= target * 0.85) return null
  return { encoded, target }
}

export function buildLiveSummary({ stats = {}, qualityGrade = 'F', qualityScore = 0, gradeTone = '' } = {}) {
  const fps = metricNumber(stats.fps)
  const targetFps = metricNumber(stats.session_target_fps || stats.requested_client_fps)
  const latency = Number(stats.latency_ms)
  const loss = Number(stats.packet_loss)
  const bitrate = Number(stats.bitrate_kbps)

  return {
    quality: `${qualityGrade} · ${qualityScore}`,
    qualityDetail: qualityScore >= 90 ? 'Locked in' : qualityScore >= 75 ? 'Playable' : 'Needs tuning',
    qualityTone: gradeTone,
    latency: formatLiveNumber(latency, 0, ' ms'),
    latencyTone: Number.isFinite(latency)
      ? (latency <= 20 ? 'text-green-400' : latency <= 50 ? 'text-yellow-400' : 'text-red-400')
      : 'text-storm',
    fps: formatLiveNumber(fps, 1),
    fpsTone: fps > 0 ? (fps >= 55 ? 'text-green-400' : fps >= 30 ? 'text-yellow-400' : 'text-red-400') : 'text-storm',
    fpsDetail: targetFps > 0 ? `${targetFps.toFixed(0)} target` : 'Encoded',
    loss: formatLiveNumber(loss, 1, '%'),
    lossTone: Number.isFinite(loss)
      ? (loss < 0.5 ? 'text-green-400' : loss < 2 ? 'text-yellow-400' : 'text-red-400')
      : 'text-storm',
    bitrate: Number.isFinite(bitrate) ? `${(bitrate / 1000).toFixed(1)} Mbps` : '--',
  }
}

export function buildTelemetryGuidance({ stats = {}, gpu = null, fpsTargetGap = null, captureReason = '', autoQuality = {}, headlessGpuNativeOverrideActive = false } = {}) {
  if (!stats.streaming) return { concerns: [], recommendations: [] }

  const concerns = []
  const recommendations = []
  const encodeTime = Number(stats.encode_time_ms)
  const packetLoss = Number(stats.packet_loss)
  const latency = Number(stats.latency_ms)

  if (Number.isFinite(packetLoss) && packetLoss > 2) {
    concerns.push({ key: 'network-risk', label: 'Network risk', tone: 'text-red-400', detail: `Packet loss is ${packetLoss.toFixed(1)}%. Lower bitrate, try wired, or enable FEC before blaming the encoder.` })
  } else if ((Number.isFinite(packetLoss) && packetLoss > 0.5) || (Number.isFinite(latency) && latency > 40)) {
    const latencyCopy = Number.isFinite(latency) ? `${latency.toFixed(0)} ms latency` : 'latency is high'
    concerns.push({ key: 'network-risk', label: 'Network risk', tone: 'text-yellow-400', detail: `${latencyCopy}${packetLoss > 0 ? ` with ${packetLoss.toFixed(1)}% loss` : ''}. Watch for artifacts or input delay.` })
  }

  if (Number.isFinite(encodeTime) && encodeTime > 12) {
    concerns.push({ key: 'encoder-pressure', label: 'Encoder pressure', tone: 'text-yellow-400', detail: `${encodeTime.toFixed(1)} ms encode time. Drop resolution/FPS or switch to the low-latency hardware preset.` })
  } else if (Number.isFinite(encodeTime) && encodeTime > 8) {
    concerns.push({ key: 'encoder-pressure', label: 'Encoder pressure', tone: 'text-ice', detail: `${encodeTime.toFixed(1)} ms encode time is close to the 120 FPS budget.` })
  }

  if (stats.capture_cpu_copy) {
    concerns.push({ key: 'cpu-copy-path', label: 'CPU copy path', tone: 'text-orange-300', detail: captureReason || 'Frames are crossing system memory. Prefer the DMA-BUF/GPU-native capture path for high-FPS Linux streaming.' })
  }

  if (fpsTargetGap) {
    const gpuHeadroom = gpu && Number(gpu.utilization_pct) < 30 && Number(stats.encode_time_ms) < 4
    concerns.push({
      key: 'frame-cap-likely',
      label: 'Frame cap likely',
      tone: gpuHeadroom ? 'text-green-400' : 'text-ice',
      detail: `Client asked for ${fpsTargetGap.target.toFixed(0)} FPS but encode is ${fpsTargetGap.encoded.toFixed(1)} FPS. Check in-game caps, VSync, menus, or launch flags first.`,
    })
  }

  if (headlessGpuNativeOverrideActive) {
    concerns.push({ key: 'gpu-native-override', label: 'GPU-native override', tone: 'text-amber-300', detail: 'Polaris is using windowed labwc to keep capture GPU-resident instead of forcing true headless SHM.' })
  }

  if (concerns.length === 0) {
    recommendations.push({ color: 'text-green-400', message: 'Stream signals look clean. Keep playing; only tune if the game feels off.' })
  } else {
    recommendations.push(...concerns.slice(0, 3).map((concern) => ({ color: concern.tone, message: `${concern.label}: ${concern.detail}` })))
  }

  if (!autoQuality.enabled && stats.streaming) {
    recommendations.push({ color: 'text-accent', message: 'Enable Auto Quality in Audio/Video settings so Polaris can balance bitrate, profile choice, and recovery behavior automatically.' })
  } else if (autoQuality.detail && autoQuality.state === 'recovering') {
    recommendations.push({ color: 'text-amber-300', message: autoQuality.detail })
  }

  return { concerns, recommendations }
}

export function buildMissionControlStrip({
  statsLoaded = false,
  stats = {},
  pairedClients = 0,
  appCatalogCount = 0,
  readyCheckDisplay = {},
  liveSummary = {},
  telemetryConcerns = [],
  primaryRecommendation = null,
  runtimePathNote = '',
} = {}) {
  if (!statsLoaded) {
    return [
      { key: 'now', label: 'Now', title: 'Booting telemetry', detail: 'Waiting for host stats before calling the play.', tone: 'text-storm' },
      { key: 'next', label: 'Next', title: 'Stand by', detail: 'Mission Control will rank launch blockers as soon as the host answers.', tone: 'text-silver' },
      { key: 'fix', label: 'Fix', title: 'No call yet', detail: 'Nothing actionable until telemetry loads.', tone: 'text-storm' },
    ]
  }

  if (stats.streaming) {
    const urgentConcern = telemetryConcerns[0]
    return [
      { key: 'now', label: 'Now', title: `Streaming ${liveSummary.quality || ''}`.trim(), detail: runtimePathNote || 'Live session is active; watch player feel first, graphs second.', tone: liveSummary.qualityTone || 'text-silver' },
      { key: 'next', label: 'Next', title: urgentConcern ? urgentConcern.label : 'Keep playing', detail: urgentConcern?.detail || 'No immediate tuning move. Let Auto Quality and session history collect signal.', tone: urgentConcern?.tone || 'text-green-400' },
      { key: 'fix', label: 'Fix', title: primaryRecommendation ? 'Top cue' : 'Nothing hot', detail: primaryRecommendation?.message || 'No live cues are asking for attention right now.', tone: primaryRecommendation?.color || 'text-green-400' },
    ]
  }

  const primaryIssue = readyCheckDisplay.primaryIssue
  return [
    {
      key: 'now',
      label: 'Now',
      title: pairedClients > 0 ? 'Host on standby' : 'Pair a client',
      detail: pairedClients > 0 ? `${pairedClients} paired client${pairedClients === 1 ? '' : 's'} ready to launch.` : 'No paired client yet, so Moonlight/Nova cannot start a stream.',
      tone: pairedClients > 0 ? 'text-green-400' : 'text-amber-300',
    },
    {
      key: 'next',
      label: 'Next',
      title: appCatalogCount > 0 ? 'Launch from Library' : 'Import games',
      detail: appCatalogCount > 0 ? `${appCatalogCount} app${appCatalogCount === 1 ? '' : 's'} available. Pick a game and send it.` : 'Scan/import games so this cockpit becomes a launcher, not a settings shrine.',
      tone: appCatalogCount > 0 ? 'text-ice' : 'text-amber-300',
    },
    {
      key: 'fix',
      label: 'Fix',
      title: primaryIssue ? primaryIssue.label : 'Launch checks clear',
      detail: primaryIssue ? primaryIssue.detail : 'Pairing, library, discovery, display, and audio checks are green.',
      tone: primaryIssue ? 'text-amber-300' : 'text-green-400',
      to: primaryIssue?.to || null,
    },
  ]
}

export function buildSecondScreenOverlayRecommendation() {
  return {
    title: 'Later lane: second-screen overlay',
    detail: 'After the base cockpit is proven, split a TV/handheld-safe overlay mode that keeps Now / Next / Fix, stream grade, latency, FPS, loss, and one top cue visible without dragging in full charts or admin controls.',
  }
}
