export function detectBrowserStreamSupport(scope = globalThis) {
  return {
    webTransport: typeof scope.WebTransport === 'function',
    videoDecoder: typeof scope.VideoDecoder === 'function',
    audioDecoder: typeof scope.AudioDecoder === 'function',
  }
}

export function browserStreamUnsupportedReasons(support) {
  const reasons = []
  if (!support.webTransport) reasons.push('WebTransport')
  if (!support.videoDecoder) reasons.push('WebCodecs VideoDecoder')
  if (!support.audioDecoder) reasons.push('WebCodecs AudioDecoder')
  return reasons
}

export function browserStreamIsSupported(support) {
  return browserStreamUnsupportedReasons(support).length === 0
}

export function browserStreamReadiness(support, status = {}) {
  const missing = browserStreamUnsupportedReasons(support)
  const buildEnabled = Boolean(status.build_enabled)
  const configEnabled = Boolean(status.config_enabled)

  if (missing.length) {
    return {
      ready: false,
      tone: 'warning',
      label: 'Browser unsupported',
      message: `Missing ${missing.join(', ')}.`,
    }
  }
  if (!buildEnabled) {
    return {
      ready: false,
      tone: 'danger',
      label: 'Not built',
      message: 'This Polaris build does not include Browser Stream.',
    }
  }
  if (!configEnabled) {
    return {
      ready: false,
      tone: 'warning',
      label: 'Disabled',
      message: 'Enable Browser Stream in settings before starting a session.',
    }
  }
  return {
    ready: true,
    tone: 'success',
    label: status.available ? 'Ready' : 'Standby',
    message: status.message || 'Ready for an instant LAN browser session.',
  }
}

export function browserStreamInputSummary(input = {}) {
  return [
    { key: 'touch', label: 'Touch', supported: input.touch !== false },
    { key: 'pointer', label: 'Mouse', supported: input.pointer !== false },
    { key: 'keyboard', label: 'Keyboard', supported: input.keyboard !== false },
    { key: 'wheel', label: 'Wheel', supported: input.wheel !== false },
    {
      key: 'gamepad',
      label: 'Browser controller',
      supported: Boolean(input.gamepad),
      deferred: input.gamepad_reserved !== false && !input.gamepad,
    },
  ]
}

export const browserStreamProfiles = Object.freeze([
  {
    id: 'low_latency',
    label: 'Low Latency',
    detail: '720p60',
    description: 'Keeps decoder queues tight for direct touch and mouse feel.',
    backend: {
      width: 1280,
      height: 720,
      fps: 60,
      bitrateKbps: 8000,
    },
    frameAssembler: {
      maxFrameAgeMs: 180,
      maxPendingFrames: 6,
    },
    maxVideoDecodeQueue: 2,
    maxAudioDecodeQueue: 8,
    audioLatencyHint: 'interactive',
    audioMinLeadSeconds: 0.015,
    audioStartLeadSeconds: 0.04,
    audioMaxLeadSeconds: 0.18,
  },
  {
    id: 'balanced',
    label: 'Balanced',
    detail: '720p60',
    description: 'Keeps the current quality target with a little more jitter margin.',
    backend: {
      width: 1280,
      height: 720,
      fps: 60,
      bitrateKbps: 10000,
    },
    frameAssembler: {
      maxFrameAgeMs: 250,
      maxPendingFrames: 8,
    },
    maxVideoDecodeQueue: 4,
    maxAudioDecodeQueue: 12,
    audioLatencyHint: 'interactive',
    audioMinLeadSeconds: 0.02,
    audioStartLeadSeconds: 0.06,
    audioMaxLeadSeconds: 0.25,
  },
  {
    id: 'battery_saver',
    label: 'Battery Saver',
    detail: '540p30',
    description: 'Reduces decode work and network use for handheld browser sessions.',
    backend: {
      width: 960,
      height: 540,
      fps: 30,
      bitrateKbps: 4500,
    },
    frameAssembler: {
      maxFrameAgeMs: 350,
      maxPendingFrames: 10,
    },
    maxVideoDecodeQueue: 5,
    maxAudioDecodeQueue: 16,
    audioLatencyHint: 'balanced',
    audioMinLeadSeconds: 0.03,
    audioStartLeadSeconds: 0.08,
    audioMaxLeadSeconds: 0.32,
  },
])

export function browserStreamProfileById(profileId) {
  return browserStreamProfiles.find((profile) => profile.id === profileId) ||
    browserStreamProfiles.find((profile) => profile.id === 'balanced')
}

export function browserStreamAppFit(app = {}) {
  const category = app['game-category'] || app.game_category || ''
  const genres = Array.isArray(app.genres)
    ? app.genres.map((genre) => String(genre).toLowerCase())
    : []
  const name = String(app.name || '').toLowerCase()

  if (category === 'desktop') {
    return {
      key: 'best',
      label: 'Best in browser',
      tone: 'success',
      description: 'Good for desktop, launchers, and touch or mouse workflows.',
    }
  }
  if (category === 'cinematic' || genres.some((genre) => (
    genre.includes('strategy') ||
    genre.includes('card') ||
    genre.includes('turn-based') ||
    genre.includes('simulation') ||
    genre.includes('puzzle')
  )) || name.includes('slay the spire')) {
    return {
      key: 'good',
      label: 'Good fit',
      tone: 'success',
      description: 'Good for slower games where touch, mouse, or keyboard are enough.',
    }
  }
  if (category === 'fast_action' || category === 'vr') {
    return {
      key: 'nova',
      label: 'Nova recommended',
      tone: 'warning',
      description: 'Use Nova or Moonlight for controller-first or timing-sensitive play.',
    }
  }
  return {
    key: 'try',
    label: 'Try in browser',
    tone: 'neutral',
    description: 'Works best when the game does not depend on a controller.',
  }
}

export function formatBrowserStreamAppOption(app = {}) {
  const name = app.name || app.uuid || 'Unknown app'
  return `${name} - ${browserStreamAppFit(app).label}`
}
