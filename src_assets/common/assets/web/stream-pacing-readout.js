const REQUESTS = { true: 'requested', false: 'not_requested', absent: 'not_sent', malformed: 'malformed' }
const REASONS = new Set(['watch_or_input_only', 'request_malformed', 'not_requested', 'route_unreported',
  'route_fixed_interval', 'clock_domain_unverified', 'source_time_unproven', 'stream_ended',
  'authority_lost', 'pacing_evidence_stale', 'host_permission_off', 'unsupported_codec', 'unsupported_route', 'unsupported_loop'])

// What the stream's frame interval error (frame_interval_error_ms) is measured against. The host
// serves frame_pacing_measurement only while it source-paces an opted-in client: the error is then
// encode handoff against the source's own intervals, and stale timing reads as pending. Without it the
// error is against the requested, fixed interval. Neither verifies the client display's refresh or VRR.
// The values name config.stream_pacing_* labels, as streamPacingReadouts' effective pacing does.
export function frameIntervalBasis(stats = {}) {
  const measurement = stats?.frame_pacing_measurement
  if (measurement === undefined || measurement === null) return 'fixed_grid'
  if (measurement?.basis !== 'source_relative_encode_handoff') return 'unknown'
  return measurement.available === true ? 'source_time' : 'pending'
}

// The Mission Control frame-health pill for that error: its text and tooltip name the basis, and a
// source-paced error without current timing evidence shows no figure.
export function frameIntervalReadout(stats = {}, t) {
  const basis = frameIntervalBasis(stats)
  const error = basis === 'pending' ? NaN : Number(stats?.frame_interval_error_ms ?? stats?.frame_jitter_ms)
  const value = Number.isFinite(error) ? `${error.toFixed(1)} ms` : '--'
  const params = { basis: t(`config.stream_pacing_${basis}`), source: t('config.stream_pacing_source_time'), value }
  return { basis, value, label: t(`mission_control.diag_interval_error_${basis}`, params),
    title: t(`mission_control.diag_interval_basis_${basis}`, params),
    tone: Number.isFinite(error) && error > 4 ? 'text-warning' : '' }
}

// Each receipt belongs to its client's opaque stream identity. A global label
// cannot stand in for another client's timing or a previous generation.
export function streamPacingReadouts(stats = {}, confirmed = true) {
  if (stats.streaming !== true) return []
  return (Array.isArray(stats.clients) ? stats.clients : []).map(client => {
    const receipt = client?.vrr
    const bound = confirmed && receipt?.schema === 1 && typeof client.stream_instance_id === 'string' &&
      client.stream_instance_id.length > 0 && receipt.stream_instance_id === client.stream_instance_id
    const requested = bound && typeof receipt.requested === 'string' && Object.hasOwn(REQUESTS, receipt.requested) ? REQUESTS[receipt.requested] : 'unknown'
    let effective = 'unknown'
    if (bound && requested === 'requested' && receipt.effective === true && receipt.pacing_mode === 'source_time' &&
        receipt.host_permission !== false && receipt.host_pacing?.fresh === true && receipt.host_pacing?.state === 'source_time') effective = 'source_time'
    else if (bound && receipt.effective === false && receipt.pacing_mode === 'fixed_grid') effective = 'fixed_grid'
    else if (bound && receipt.effective === false && receipt.pacing_mode === 'source_time_pending') effective = 'pending'
    const reportedReason = bound && typeof receipt.fallback_reason === 'string' ? receipt.fallback_reason : ''
    const reason = bound && requested === 'requested' && receipt.host_permission === false && reportedReason !== 'watch_or_input_only'
      ? 'host_permission_off' : reportedReason
    return { client: String(client?.name || ''), requested, effective, reason,
      reasonLabel: REASONS.has(reason) ? `reason_${reason}` : 'fallback_used',
      fallbackKnown: bound && (reason.length > 0 || (effective === 'source_time' && receipt.fallback_reason === null)) }
  })
}
