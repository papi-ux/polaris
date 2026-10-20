// A failed start or configuration finding must not become an idle success.
export function doctorIsWaitingForStream(doctor, stats) {
  return stats?.streaming === false &&
    doctor?.primary_issue === 'no_active_stream' &&
    doctor?.status === 'unknown' && doctor?.severity === 'info'
}

// The cards of a Fix My Stream checklist that are current before any stream: the host's own
// configuration findings. Every other row measures a live session and has nothing to say while
// Doctor waits for one (#152).
export function idleHostFindings(checklist) {
  if (!Array.isArray(checklist)) return []
  return checklist.filter((item) => item?.key === 'host-config' || String(item?.key || '').startsWith('host-config-'))
}
