const REQUESTS = { true: 'requested', false: 'not_requested', absent: 'not_sent', malformed: 'malformed' }
const REASONS = new Set(['watch_or_input_only', 'request_malformed', 'not_requested', 'route_unreported',
  'route_fixed_interval', 'clock_domain_unverified', 'source_time_unproven', 'stream_ended',
  'authority_lost', 'pacing_evidence_stale'])

// Each receipt belongs to its client's opaque stream identity. A global label
// cannot stand in for another client's timing or a previous generation.
export function streamPacingReadouts(stats = {}, confirmed = true) {
  if (stats.streaming !== true) return []
  return (Array.isArray(stats.clients) ? stats.clients : []).map(client => {
    const receipt = client?.vrr
    const bound = confirmed && receipt?.schema === 1 && typeof client.stream_instance_id === 'string' &&
      client.stream_instance_id.length > 0 && receipt.stream_instance_id === client.stream_instance_id
    const requested = bound && typeof receipt.requested === 'string' ? (REQUESTS[receipt.requested] || 'unknown') : 'unknown'
    let effective = 'unknown'
    if (bound && requested === 'requested' && receipt.effective === true && receipt.pacing_mode === 'source_time' &&
        receipt.host_pacing?.fresh === true && receipt.host_pacing?.state === 'source_time') effective = 'source_time'
    else if (bound && receipt.effective === false && receipt.pacing_mode === 'fixed_grid') effective = 'fixed_grid'
    else if (bound && receipt.effective === false && receipt.pacing_mode === 'source_time_pending') effective = 'pending'
    const reason = bound && typeof receipt.fallback_reason === 'string' ? receipt.fallback_reason : ''
    return { client: String(client?.name || ''), requested, effective, reason,
      reasonLabel: REASONS.has(reason) ? `reason_${reason}` : 'fallback_used',
      fallbackKnown: bound && (reason.length > 0 || (effective === 'source_time' && receipt.fallback_reason === null)) }
  })
}
