const transports = new Set(['direct', 'derp', 'peer_relay', 'unknown'])
const reasons = new Set(['observed', 'peer_idle', 'transport_unknown', 'no_active_stream', 'stream_changed', 'unsupported_platform', 'localapi_unavailable', 'malformed_status', 'peer_not_confirmed'])

// Only retain the daemon's small public vocabulary. No endpoint, identity,
// hostname, peer map, or arbitrary error text can cross into a UI/export.
export function normalizeTailscalePeer(value, { now = Date.now(), generation = '', streaming = false } = {}) {
  const empty = { confirmed: false, transport: 'unknown', scope: 'peer_status', observed_at_ms: 0, reason: 'peer_not_confirmed' }
  if (!streaming) return { ...empty, reason: 'no_active_stream' }
  if (value?.schema_version !== 1 || value?.tailscale?.scope !== 'peer_status') return empty
  if (!generation || value.stream_instance_id !== String(generation)) return { ...empty, reason: 'stream_changed' }
  const evidence = value.tailscale
  if (evidence.confirmed !== true || value.status !== 'tailscale') {
    return { ...empty, reason: reasons.has(evidence.reason) ? evidence.reason : empty.reason }
  }
  if (evidence.source !== 'tailscaled_localapi' || evidence.max_age_ms !== 10000 ||
      !Number.isSafeInteger(evidence.observed_at_ms) || evidence.observed_at_ms <= 0 ||
      !Number.isSafeInteger(now) || now < evidence.observed_at_ms - 1000 || now - evidence.observed_at_ms > 10000 ||
      !transports.has(evidence.transport)) return empty
  return { confirmed: true, transport: evidence.transport, scope: 'peer_status',
    observed_at_ms: evidence.observed_at_ms, reason: reasons.has(evidence.reason) ? evidence.reason : 'transport_unknown' }
}

export function safeTailscalePeerSnapshot(value) {
  const confirmed = value?.scope === 'peer_status' && value?.confirmed === true && transports.has(value.transport) &&
    Number.isSafeInteger(value.observed_at_ms) && value.observed_at_ms > 0
  return { confirmed, transport: confirmed ? value.transport : 'unknown', scope: 'peer_status',
    observed_at_ms: confirmed ? value.observed_at_ms : 0,
    reason: reasons.has(value?.reason) ? value.reason : 'peer_not_confirmed' }
}

export function tailscalePeerSummary(value) {
  const evidence = safeTailscalePeerSnapshot(value)
  const labels = { direct: 'Direct', derp: 'DERP relay', peer_relay: 'Peer relay', unknown: 'Transport unknown' }
  if (evidence?.scope !== 'peer_status' || evidence.confirmed !== true || !transports.has(evidence.transport)) return 'Unknown'
  return `${labels[evidence.transport]} (matched Tailscale peer; video/audio route unverified)`
}
