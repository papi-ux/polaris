export const AUTO_QUALITY_STATES = Object.freeze({
  OFF: 'off',
  WATCHING: 'watching',
  OPTIMIZING: 'optimizing',
  STABLE: 'stable',
  RECOVERING: 'recovering',
  MANUAL_OVERRIDE: 'manual_override',
  NEEDS_ATTENTION: 'needs_attention',
})

function lower(value) {
  return String(value || '').toLowerCase()
}

function numberValue(value) {
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : 0
}

function enabledValue(value) {
  return value === true || value === 'enabled' || value === 'true' || value === 1
}

function firstNumber(...values) {
  for (const value of values) {
    const parsed = numberValue(value)
    if (parsed > 0) return parsed
  }
  return 0
}

function codecLabel(value) {
  const raw = String(value || '')
  const token = raw.toLowerCase()
  if (token.includes('av1')) return 'AV1'
  if (token.includes('hevc') || token.includes('h265')) return 'HEVC'
  if (token.includes('h264') || token.includes('avc')) return 'H.264'
  return raw ? raw.toUpperCase() : ''
}

function buildTargetSummary(stats, adaptiveTargetKbps) {
  const width = numberValue(stats?.width)
  const height = numberValue(stats?.height)
  const resolution = stats?.capture?.resolution || (width > 0 && height > 0 ? `${width}x${height}` : '')
  const fps = firstNumber(
    stats?.session_target_fps,
    stats?.encode_target_fps,
    stats?.requested_client_fps,
    stats?.encoder?.session_target_fps,
    stats?.encoder?.encode_target_fps,
    stats?.encoder?.requested_client_fps,
  )
  const display = resolution && fps > 0
    ? `${resolution}@${Math.round(fps)}`
    : resolution || (fps > 0 ? `${Math.round(fps)} FPS` : '')
  const codec = codecLabel(stats?.codec || stats?.encoder?.codec)
  const bitrate = firstNumber(adaptiveTargetKbps, stats?.bitrate_kbps, stats?.encoder?.bitrate_kbps)
  const bitrateLabel = bitrate > 0 ? `up to ${Math.round(bitrate / 1000)} Mbps` : ''
  return [display, codec, bitrateLabel].filter(Boolean).join(' · ')
}

function issueIncludes(health, ...tokens) {
  const issues = Array.isArray(health?.issues) ? health.issues : []
  const haystack = [
    health?.primary_issue,
    health?.limiting_factor,
    health?.recovery_profile,
    ...issues,
  ].map(lower)
  return tokens.some((token) => haystack.includes(token))
}

function toneClasses(tone) {
  switch (tone) {
    case 'stable':
      return {
        toneClass: 'border-green-400/30 bg-green-400/10 text-green-300',
        panelClass: 'border-green-400/20 bg-green-400/5',
      }
    case 'warning':
      return {
        toneClass: 'border-amber-300/30 bg-amber-300/10 text-amber-200',
        panelClass: 'border-amber-300/25 bg-amber-300/10',
      }
    case 'danger':
      return {
        toneClass: 'border-red-400/30 bg-red-400/10 text-red-300',
        panelClass: 'border-red-400/25 bg-red-400/10',
      }
    case 'info':
      return {
        toneClass: 'border-ice/25 bg-ice/10 text-ice',
        panelClass: 'border-ice/20 bg-ice/5',
      }
    default:
      return {
        toneClass: 'border-storm/40 bg-storm/10 text-storm',
        panelClass: 'border-storm/30 bg-void/25',
      }
  }
}

function result({
  state,
  label,
  compactLabel,
  detail,
  targetSummary,
  tone,
  enabled,
  badges = [],
}) {
  return {
    state,
    label,
    compactLabel,
    detail,
    targetSummary,
    tone,
    enabled,
    badges,
    ...toneClasses(tone),
  }
}

export function resolveAutoQualityState(stats = {}, sync = {}) {
  const health = stats?.health || {}
  const tuning = stats?.tuning || {}
  const capture = stats?.capture || {}
  const encoder = stats?.encoder || {}
  const policy = stats?.auto_quality || health?.recovery_policy || {}
  const streaming = Boolean(stats?.streaming || stats?.streaming_active)
  const adaptiveEnabled = enabledValue(stats?.adaptive_bitrate_enabled ?? tuning?.adaptive_bitrate_enabled)
  const aiEnabled = enabledValue(stats?.ai_enabled ?? stats?.ai_optimizer_enabled ?? tuning?.ai_optimizer_enabled)
  const autoEnabled = enabledValue(
    stats?.ai_auto_quality_enabled ??
    tuning?.ai_auto_quality_enabled ??
    policy?.enabled ??
    (adaptiveEnabled || aiEnabled),
  )
  const syncState = lower(sync?.state || stats?.sync_status?.state)
  const adaptiveTarget = firstNumber(stats?.adaptive_target_bitrate_kbps, tuning?.adaptive_target_bitrate_kbps)
  const adaptiveBase = firstNumber(tuning?.adaptive_base_bitrate_kbps, stats?.adaptive_base_bitrate_kbps, stats?.bitrate_kbps)
  const adaptiveLowered = adaptiveEnabled && adaptiveTarget > 0 && adaptiveBase > 0 && adaptiveTarget < adaptiveBase
  const targetFps = firstNumber(stats?.session_target_fps, stats?.encode_target_fps, stats?.requested_client_fps, encoder?.session_target_fps)
  const fps = firstNumber(stats?.fps, encoder?.fps)
  const fpsTargetMiss = targetFps > 0 && fps > 0 && fps < targetFps * 0.85
  const captureCpuCopy = Boolean(
    stats?.capture_cpu_copy ||
    capture?.cpu_copy ||
    lower(stats?.capture_transport || capture?.transport) === 'shm' ||
    lower(stats?.capture_residency || capture?.residency) === 'cpu' ||
    lower(stats?.encode_target_residency || encoder?.target_residency) === 'cpu',
  )
  const manualOverride = Boolean(
    sync?.manualOverride ||
    syncState === 'manual_override' ||
    stats?.display_mode?.explicit_choice ||
    stats?.manual_override,
  )
  const syncFailed = syncState === 'failed' || syncState === 'blocked' || sync?.failed
  const degraded = lower(health?.grade) === 'degraded' || lower(health?.decoder_risk) === 'elevated'
  const hostRenderLimited = Boolean(
    health?.host_render_limited ||
    issueIncludes(health, 'host_render_limited', 'host_render'),
  )
  const healthGrade = lower(health?.grade)
  const healthAutoAction = lower(health?.auto_action)
  const healthSuggestsRecovery = ['watch', 'degraded'].includes(healthGrade) ||
    Boolean(health?.recovery_profile || health?.relaunch_recommended) ||
    ['lower_bitrate', 'apply_recovery', 'suggest_recovery'].includes(healthAutoAction)
  const currentBitrate = firstNumber(adaptiveTarget, stats?.bitrate_kbps, encoder?.bitrate_kbps)
  const safeBitrate = numberValue(health?.safe_bitrate_kbps)
  const safeBitrateApplied = healthSuggestsRecovery &&
    safeBitrate > 0 &&
    currentBitrate > 0 &&
    safeBitrate < currentBitrate
  const safeProfileApplied = Boolean(
    safeBitrateApplied ||
    (healthSuggestsRecovery && health?.safe_target_fps) ||
    (healthSuggestsRecovery && health?.safe_display_mode) ||
    health?.recovery_profile ||
    health?.relaunch_recommended,
  )
  const optimizing = ['initializing', 'cage_starting', 'game_launching'].includes(lower(stats?.state)) ||
    lower(stats?.optimization_cache_status || encoder?.optimization_cache_status) === 'miss' ||
    syncState === 'applying'
  const targetSummary = buildTargetSummary(stats, adaptiveTarget)
  const badges = [
    autoEnabled ? 'AI Auto Quality' : '',
    adaptiveLowered ? 'Bitrate recovery' : '',
    stats?.capture_gpu_native || capture?.gpu_native ? 'GPU-native path' : '',
  ].filter(Boolean)

  if (!autoEnabled && !manualOverride) {
    return result({
      state: AUTO_QUALITY_STATES.OFF,
      label: 'Auto Quality Off',
      compactLabel: 'Off',
      detail: 'Manual stream tuning is active.',
      targetSummary,
      tone: 'muted',
      enabled: false,
      badges,
    })
  }

  if (manualOverride) {
    return result({
      state: AUTO_QUALITY_STATES.MANUAL_OVERRIDE,
      label: 'Manual Override',
      compactLabel: 'Manual',
      detail: sync?.message || 'Manual client or display settings are overriding Auto Quality.',
      targetSummary,
      tone: 'warning',
      enabled: autoEnabled,
      badges,
    })
  }

  if (syncFailed || captureCpuCopy || degraded) {
    const detail = health?.summary || sync?.message || (captureCpuCopy
      ? 'The active capture path is crossing CPU/system memory.'
      : 'Auto Quality needs a stream setting check.')
    return result({
      state: AUTO_QUALITY_STATES.NEEDS_ATTENTION,
      label: captureCpuCopy ? 'Capture Attention' : 'Needs Attention',
      compactLabel: 'Attention',
      detail,
      targetSummary,
      tone: 'danger',
      enabled: true,
      badges,
    })
  }

  if (optimizing) {
    return result({
      state: AUTO_QUALITY_STATES.OPTIMIZING,
      label: 'Auto Quality Optimizing',
      compactLabel: 'Optimizing',
      detail: 'Selecting the best launch profile for this stream.',
      targetSummary,
      tone: 'info',
      enabled: true,
      badges,
    })
  }

  if (
    adaptiveLowered ||
    hostRenderLimited ||
    safeProfileApplied ||
    fpsTargetMiss ||
    ['lower_bitrate', 'apply_recovery', 'suggest_recovery'].includes(healthAutoAction)
  ) {
    const label = hostRenderLimited
      ? 'Host Render Limited'
      : adaptiveLowered
        ? 'Recovering Bitrate'
        : fpsTargetMiss
          ? 'Recovering FPS'
          : 'Auto Quality Recovering'
    const detail = health?.summary || (hostRenderLimited
      ? 'The host render path is missing the stream FPS target; lower game quality, render resolution, or FPS before tuning bitrate.'
      : adaptiveLowered
        ? `Bitrate is temporarily lowered to ${Math.round(adaptiveTarget / 1000)} Mbps while conditions recover.`
        : 'Applying a safer stream profile.')
    return result({
      state: AUTO_QUALITY_STATES.RECOVERING,
      label,
      compactLabel: hostRenderLimited ? 'Host' : adaptiveLowered ? `${Math.round(adaptiveTarget / 1000)} Mbps` : 'Recovery',
      detail,
      targetSummary,
      tone: 'warning',
      enabled: true,
      badges,
    })
  }

  if (!streaming) {
    return result({
      state: AUTO_QUALITY_STATES.WATCHING,
      label: 'Auto Quality Watching',
      compactLabel: 'Ready',
      detail: 'Ready to choose a balanced profile for the next stream.',
      targetSummary,
      tone: 'info',
      enabled: true,
      badges,
    })
  }

  return result({
    state: AUTO_QUALITY_STATES.STABLE,
    label: 'Auto Quality Stable',
    compactLabel: 'Stable',
    detail: health?.summary || 'Performance and quality are holding steady.',
    targetSummary,
    tone: 'stable',
    enabled: true,
    badges,
  })
}
