// What a Doctor finding rests on, for the level below what is happening and what to try. Wire fields
// are read as the host sends them and nothing here invents a finding.
const STATUS_KEYS = new Set(['pass', 'info', 'watch', 'warning', 'fail', 'unknown'])

function evidenceValue(value, unit) {
  if (typeof value === 'number' && Number.isFinite(value)) return `${Number(value.toFixed(1))}${unit ? ` ${unit}` : ''}`
  return typeof value === 'string' ? value : ''
}

// A row without a label says nothing to a reader, and a value that is not a plain reading is left
// to the row's own detail rather than printed raw.
function evidenceRows(evidence) {
  if (!Array.isArray(evidence)) return []
  return evidence
    .filter(row => row && typeof row === 'object' && typeof row.label === 'string' && row.label)
    .map(row => ({
      id: String(row.id || row.label),
      label: row.label,
      value: evidenceValue(row.value, typeof row.unit === 'string' ? row.unit : ''),
      status: STATUS_KEYS.has(row.status) ? row.status : 'unknown',
      detail: typeof row.detail === 'string' ? row.detail : '',
    }))
}

export function doctorDetails(doctor) {
  const recommendation = doctor?.recommendation
  const reason = doctor?.safe_recovery_action?.unavailable_reason
  return {
    expected: typeof recommendation?.expected_effect === 'string' ? recommendation.expected_effect : '',
    // Why nothing can be pressed, when that says more than the step above it.
    actionNote: typeof reason === 'string' && reason !== recommendation?.body ? reason : '',
    confidence: typeof doctor?.confidence?.level === 'string' ? doctor.confidence.level : '',
    evidence: evidenceRows(doctor?.evidence),
  }
}
