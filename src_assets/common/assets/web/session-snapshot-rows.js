// Session Snapshot rows for Doctor & Support. Pure builders so the view stays
// a renderer, the labels live in the locale files, and the host settings
// projection can feed the stream display and provenance rows when it is served.

export function formatNumber(value, digits = 1) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) {
    return '0'
  }
  return Number(value).toFixed(digits)
}

export function formatFps(value) {
  return `${formatNumber(value, 1)} FPS`
}

export function formatResolution(width, height, t) {
  if (!width || !height) return t('troubleshooting.snapshot_unknown')
  return `${width}x${height}`
}

export function yesNo(value, t) {
  return t(value ? 'troubleshooting.snapshot_yes' : 'troubleshooting.snapshot_no')
}

export function summarizeStreamStats(s = {}, t) {
  if (!s.streaming) return t('troubleshooting.snapshot_no_active_stream')
  return t('troubleshooting.snapshot_stream_summary', {
    fps: formatFps(s.fps),
    target: formatFps(s.session_target_fps || s.requested_client_fps),
    kbps: s.bitrate_kbps || 0,
    loss: formatNumber(s.packet_loss, 2),
    encode: formatNumber(s.encode_time_ms, 1),
  })
}

export function hasRuntimeOverride(s = {}) {
  const effectiveKnown = s.runtime_effective_headless !== undefined && s.runtime_effective_headless !== null
  return Boolean(s.runtime_requested_headless) &&
    effectiveKnown &&
    !Boolean(s.runtime_effective_headless) &&
    Boolean(s.runtime_gpu_native_override_active)
}

export function runtimeModeDescription(s = {}, t) {
  if (s.stream_display_mode) return s.stream_display_mode
  return t('troubleshooting.snapshot_runtime_mode_value', {
    effective: yesNo(s.runtime_effective_headless, t),
    requested: yesNo(s.runtime_requested_headless, t),
  })
}

export function runtimeOverrideDescription(s = {}, t) {
  if (hasRuntimeOverride(s)) return t('troubleshooting.snapshot_runtime_override_windowed')
  return s.runtime_gpu_native_override_active
    ? t('troubleshooting.snapshot_runtime_override_active')
    : t('troubleshooting.snapshot_none')
}

// Host capture reasons collapse onto a smaller set of player-facing messages.
export const CAPTURE_REASON_KEYS = Object.freeze({
  gpu_native: 'gpu_native',
  headless_extcopy_dmabuf: 'headless_extcopy_dmabuf',
  windowed_dmabuf_override: 'windowed_dmabuf_override',
  headless_shm_fallback: 'headless_shm',
  headless_shm_default: 'headless_shm',
  gpu_native_requested_shm_fallback: 'gpu_native_requested_shm_fallback',
  gpu_native_requested_cpu_capture: 'gpu_native_requested_cpu_capture',
  gpu_native_requested_cpu_encode_upload: 'gpu_native_requested_cpu_encode_upload',
  encoder_upload_cpu: 'encoder_upload_cpu',
  cpu_capture: 'cpu_capture',
  shm_capture: 'cpu_capture',
  dmabuf_gpu_capture: 'dmabuf_gpu_capture',
  no_capture_metadata: 'no_capture_metadata',
})

export function captureReasonDescription(reason, t) {
  const key = CAPTURE_REASON_KEYS[String(reason || '').toLowerCase()] || 'unknown'
  return t(`troubleshooting.capture_reason_${key}`)
}

export function fpsTargetGapDescription(s = {}, t) {
  const encoded = Number(s.fps)
  const target = Number(s.session_target_fps || s.requested_client_fps)
  if (!Number.isFinite(encoded) || !Number.isFinite(target) || target < 90 || encoded <= 0) {
    return t('troubleshooting.snapshot_none')
  }
  if (encoded >= target * 0.85) {
    return t('troubleshooting.snapshot_none')
  }
  return t('troubleshooting.snapshot_fps_gap_value', { encoded: formatFps(encoded), target: formatFps(target) })
}

function streamDisplayRow(s, t, streamDisplay) {
  const label = t('troubleshooting.snapshot_stream_display_mode')
  const hostLabel = streamDisplay?.effective_label || streamDisplay?.configured_label
  if (!hostLabel) {
    return { label, value: runtimeModeDescription(s, t) }
  }
  return {
    label,
    value: hostLabel,
    note: streamDisplay.relaunch_required === true
      ? t('troubleshooting.snapshot_stream_display_pending', { configured: streamDisplay.configured_label || '' })
      : t('troubleshooting.snapshot_stream_display_synced'),
  }
}

function lastWriteRow(t, provenance) {
  const note = Array.isArray(provenance) && provenance[0] && typeof provenance[0] === 'object' ? provenance[0] : null
  if (!note) return null
  const writerKey = note.writer === 'gamestream' ? 'gamestream' : note.writer === 'web_ui' ? 'web_ui' : 'other'
  const keys = Array.isArray(note.keys) ? note.keys : []
  return {
    label: t('troubleshooting.snapshot_last_write'),
    value: t(`troubleshooting.snapshot_writer_${writerKey}`),
    note: t('troubleshooting.snapshot_last_write_note', { count: keys.length, at: String(note.at || '') }),
  }
}

/**
 * Rows for the Session Snapshot card. `summary` is the four-tile strip,
 * `details` the grid below it. Empty while nothing streams.
 */
export function buildSessionSnapshotRows(stats, t, { streamDisplay = null, provenance = null } = {}) {
  if (!stats || !stats.streaming) return { summary: [], details: [] }
  const s = stats
  const unknown = t('troubleshooting.snapshot_unknown')
  const rows = [
    { label: t('troubleshooting.snapshot_resolution'), value: formatResolution(s.width, s.height, t) },
    { label: t('troubleshooting.snapshot_codec'), value: s.codec || unknown },
    { label: t('troubleshooting.snapshot_fps'), value: t('troubleshooting.snapshot_fps_value', { encoded: formatFps(s.fps), target: formatFps(s.session_target_fps) }) },
    { label: t('troubleshooting.snapshot_bitrate'), value: t('troubleshooting.snapshot_bitrate_value', { kbps: s.bitrate_kbps || 0 }) },
    { label: t('troubleshooting.snapshot_client_ip'), value: s.client_ip || unknown },
    { label: t('troubleshooting.snapshot_active_sessions'), value: `${s.active_sessions ?? 0}` },
    { label: t('troubleshooting.snapshot_requested_fps'), value: formatFps(s.requested_client_fps) },
    { label: t('troubleshooting.snapshot_runtime_backend'), value: s.runtime_backend || unknown },
    streamDisplayRow(s, t, streamDisplay),
    { label: t('troubleshooting.snapshot_runtime_override'), value: runtimeOverrideDescription(s, t) },
    { label: t('troubleshooting.snapshot_fps_target_gap'), value: fpsTargetGapDescription(s, t) },
    { label: t('troubleshooting.snapshot_capture_path'), value: s.capture_path || t('troubleshooting.snapshot_unknown_word') },
    { label: t('troubleshooting.snapshot_capture_reason'), value: captureReasonDescription(s.capture_path_reason, t) },
    { label: t('troubleshooting.snapshot_capture_transport'), value: [s.capture_transport, s.capture_residency, s.capture_format].map((v) => v || t('troubleshooting.snapshot_unknown_word')).join(' / ') },
    { label: t('troubleshooting.snapshot_encode_target'), value: [s.encode_target_device, s.encode_target_residency, s.encode_target_format].map((v) => v || t('troubleshooting.snapshot_unknown_word')).join(' / ') },
    { label: t('troubleshooting.snapshot_gpu_native'), value: yesNo(s.capture_gpu_native, t) },
    { label: t('troubleshooting.snapshot_cpu_copy'), value: yesNo(s.capture_cpu_copy, t) },
    { label: t('troubleshooting.snapshot_pacing_policy'), value: s.pacing_policy || t('troubleshooting.snapshot_none_word') },
    { label: t('troubleshooting.snapshot_optimization_source'), value: s.optimization_source || t('troubleshooting.snapshot_default_word') },
    { label: t('troubleshooting.snapshot_network'), value: t('troubleshooting.snapshot_network_value', { latency: formatNumber(s.latency_ms, 1), loss: formatNumber(s.packet_loss, 2) }) },
    { label: t('troubleshooting.snapshot_frame_delivery'), value: t('troubleshooting.snapshot_frame_delivery_value', { duplicate: formatNumber((s.duplicate_frame_ratio || 0) * 100, 2), dropped: formatNumber((s.dropped_frame_ratio || 0) * 100, 2) }) },
    { label: t('troubleshooting.snapshot_frame_timing'), value: t('troubleshooting.snapshot_frame_timing_value', { age: formatNumber(s.avg_frame_age_ms, 2), error: formatNumber(s.frame_interval_error_ms ?? s.frame_jitter_ms, 2) }) },
  ]
  const lastWrite = lastWriteRow(t, provenance)
  if (lastWrite) rows.push(lastWrite)
  return {
    summary: [{ label: t('troubleshooting.snapshot_client'), value: s.client_name || unknown }, ...rows.slice(0, 3)],
    details: rows.slice(3),
  }
}
