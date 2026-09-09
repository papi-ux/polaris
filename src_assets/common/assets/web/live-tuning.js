export function parseLiveTuning(value) {
  if (!value || value.version !== 1 || typeof value.enabled !== 'boolean'
      || value.scope !== 'host' || typeof value.supported !== 'boolean'
      || !Number.isSafeInteger(value.sequence) || value.sequence < 1
      || !Number.isSafeInteger(value.session_generation) || value.session_generation < 0
      || ['quality_limit_kbps', 'requested_bitrate_kbps', 'applied_bitrate_kbps'].some(key =>
        !Number.isInteger(value[key]) || value[key] < 0 || value[key] > 2147483647)
      || typeof value.app_session_id !== 'string'
      || typeof value.host_instance !== 'string' || !value.host_instance
      || !/^[a-f0-9]{64}$/i.test(value.configuration_revision || '')
      || !['off', 'waiting', 'unavailable', 'applying', 'measuring', 'adjusting', 'stable'].includes(value.state)) return null
  return value
}

export function liveTuningLabel(value, confirmed = true) {
  if (!value || !confirmed) return 'Live Tuning: Unknown'
  if (!value.enabled) return 'Live Tuning Off'
  const labels = { waiting: 'On — waiting for a stream', unavailable: 'On — unavailable for this stream',
    applying: 'On — applying bitrate', measuring: 'On — measuring', adjusting: 'On — adjusting', stable: 'Live Tuning On — stable' }
  return labels[value.state] || 'Live Tuning: Unknown'
}

export function createLiveTuningReducer() {
  let current = null
  const retired = new Set()
  return (raw) => {
    const next = parseLiveTuning(raw)
    if (!next || retired.has(next.host_instance)) return null
    if (current?.host_instance === next.host_instance && next.sequence <= current.sequence) return null
    if (current && current.host_instance !== next.host_instance) retired.add(current.host_instance)
    current = next
    return next
  }
}
