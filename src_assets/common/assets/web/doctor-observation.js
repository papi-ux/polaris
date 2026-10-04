// A failed start or configuration finding must not become an idle success.
export function doctorIsWaitingForStream(doctor, stats) {
  return stats?.streaming === false &&
    doctor?.primary_issue === 'no_active_stream' &&
    doctor?.status === 'unknown' && doctor?.severity === 'info'
}
