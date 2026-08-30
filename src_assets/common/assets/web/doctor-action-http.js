export function isSuccessfulDoctorActionResponse(statusCode, result) {
  return statusCode === 200
    && result?.status === true
    && typeof result?.changed === 'boolean'
    && typeof result?.state === 'string'
    && result.state.length > 0
}

export function isRollbackUnconfirmedResponse(statusCode, result) {
  const undo = result?.undo
  const undoActionAbsent = undo != null
    && (!Object.hasOwn(undo, 'action_id') || undo.action_id === '')
  return statusCode === 409
    && result?.status === false
    && result?.changed === true
    && result?.state === 'rollback_unconfirmed'
    && typeof result?.run_id === 'string'
    && result.run_id.length > 0
    && undo?.available === false
    && undoActionAbsent
}

export function resolveDoctorActionHttpResponse(response, result) {
  if (response?.ok && isSuccessfulDoctorActionResponse(response?.status, result)) return result
  if (isRollbackUnconfirmedResponse(response?.status, result)) return result
  throw new Error(result?.error || `HTTP ${response?.status || 0}`)
}
