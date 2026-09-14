const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/
const runtimeId = /^[a-z0-9][a-z0-9-]{0,63}$/
const states = ['downloading', 'preparing', 'prepared', 'cancelled', 'interrupted', 'failed', 'recovery_required']
const retryStates = ['cancelled', 'interrupted', 'failed']
const text = value => typeof value === 'string' && value.length <= 1024
export function validSetupStart(value) {
  return value?.operation === 'start' && uuid.test(value.request_id) && runtimeId.test(value.runtime_id) &&
    typeof value.name === 'string' && value.name.length > 0 && value.name === value.name.trim() &&
    new TextEncoder().encode(value.name).length <= 128 && !/[\x00-\x1f\x7f]/.test(value.name)
}
export function validJobSnapshot(value) {
  if (!value || value.version !== 1 || typeof value.available !== 'boolean' || !text(value.message) ||
      !Array.isArray(value.runtimes) || value.runtimes.length > 16) return false
  const ids = new Set()
  for (const runtime of value.runtimes) {
    if (!runtime || !runtimeId.test(runtime.id) || ids.has(runtime.id) ||
        !['default', 'nvidia'].includes(runtime.variant) || typeof runtime.nvidia_driver !== 'string') return false
    if (runtime.variant === 'default' ? runtime.nvidia_driver !== '' :
        runtime.nvidia_driver.length > 32 || !/^[0-9]+(?:\.[0-9]+)+$/.test(runtime.nvidia_driver)) return false
    ids.add(runtime.id)
  }
  if (value.available && !ids.size) return false
  if (value.job === null) return true
  const job = value.job
  return !!job && validSetupStart({ ...job, operation: 'start' }) && states.includes(job.state) &&
    text(job.message) && typeof job.can_retry === 'boolean' && typeof job.can_cancel === 'boolean' &&
    (!job.can_retry || (value.available && retryStates.includes(job.state))) &&
    (!job.can_cancel || (value.available && job.state === 'downloading'))
}
export function requestForJob(job) {
  return { operation: 'start', request_id: job.request_id, runtime_id: job.runtime_id, name: job.name }
}
