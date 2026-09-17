const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/
const runtimeId = /^[a-z0-9][a-z0-9-]{0,63}$/
const states = ['downloading', 'preparing', 'prepared', 'cancelled', 'interrupted', 'failed', 'recovery_required', 'configuring', 'restart_required', 'activation_failed']
const downloadStates = ['downloading', 'ready', 'failed', 'cancelled']
const retryStates = ['cancelled', 'interrupted', 'failed']
const text = value => typeof value === 'string' && value.length <= 1024
const word = value => typeof value === 'string' && /^[a-z0-9_]{1,64}$/.test(value)
export function validSetupStart(value) {
  return value?.operation === 'start' && uuid.test(value.request_id) && runtimeId.test(value.runtime_id) &&
    typeof value.name === 'string' && value.name.length > 0 && value.name === value.name.trim() &&
    new TextEncoder().encode(value.name).length <= 128 && !/[\x00-\x1f\x7f]/.test(value.name)
}
export function validJobSnapshot(value) {
  if (!value || value.version !== 1 || typeof value.available !== 'boolean' || !text(value.message) ||
      (value.unavailable_reason !== undefined && !word(value.unavailable_reason)) ||
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
  const graphics = value.graphics ?? []
  if (!Array.isArray(graphics) || graphics.length > 64) return false
  const gpuIds = new Set()
  for (const gpu of graphics) {
    if (!gpu || !validGpuId(gpu.id) || gpuIds.has(gpu.id) || !text(gpu.label) || !gpu.label) return false
    gpuIds.add(gpu.id)
  }
  if (!validDownload(value.download, value.available)) return false
  if (value.job === null) return true
  const job = value.job
  if (job && job.blocked_by !== undefined && (!Array.isArray(job.blocked_by) || job.blocked_by.length > 8 ||
      !job.blocked_by.every(word))) return false
  if (job && job.recovery !== undefined && (!job.recovery || typeof job.recovery !== 'object' ||
      !/^#[a-z0-9-]{1,64}$/.test(job.recovery.doc_anchor))) return false
  return !!job && validSetupStart({ ...job, operation: 'start' }) && states.includes(job.state) &&
    (job.gpu_id === undefined || job.gpu_id === '' || validGpuId(job.gpu_id)) &&
    (job.can_activate === undefined || (typeof job.can_activate === 'boolean' && (!job.can_activate ||
      (value.available && ['prepared', 'activation_failed'].includes(job.state))))) &&
    (!['configuring', 'restart_required', 'activation_failed'].includes(job.state) || validGpuId(job.gpu_id)) &&
    text(job.message) && typeof job.can_retry === 'boolean' && typeof job.can_cancel === 'boolean' &&
    (!job.can_retry || (value.available && retryStates.includes(job.state))) &&
    (!job.can_cancel || (value.available && job.state === 'downloading'))
}
// A download-only job: the gaming runtime alone, never a Steam home. Hosts from
// before it send no download field.
function validDownload(download, available) {
  if (download === undefined || download === null) return true
  return typeof download === 'object' && uuid.test(download.request_id) && runtimeId.test(download.runtime_id) &&
    downloadStates.includes(download.state) && word(download.code) && text(download.message) &&
    typeof download.can_cancel === 'boolean' && (!download.can_cancel || (available && download.state === 'downloading'))
}
export function requestForJob(job) {
  return { operation: 'start', request_id: job.request_id, runtime_id: job.runtime_id, name: job.name }
}

// Host setup an administrator approves at the PC that runs Polaris, since 1.4.9.
// The host names the action on the check it fixes; this list is what the console knows how to ask for.
export const hostActions = ['security_install', 'docker_access']
export const hostActionWorking = ['waiting_for_approval', 'running']
const hostActionStates = [...hostActionWorking, 'done', 'refused', 'cancelled', 'not_authorized', 'no_agent', 'timed_out', 'failed']
const detail = value => typeof value === 'string' && value.length <= 4096
export function validHostActionSnapshot(value) {
  if (!value || value.version !== 1 || typeof value.available !== 'boolean') return false
  if (value.available ? value.reason !== undefined : !word(value.reason) || !text(value.message)) return false
  if (value.refusal !== undefined && (!value.refusal || !word(value.refusal.code) || !text(value.refusal.message))) return false
  if (value.job === null) return true
  const job = value.job
  return !!job && typeof job === 'object' && uuid.test(job.request_id) && hostActions.includes(job.action) &&
    hostActionStates.includes(job.state) && text(job.message) && detail(job.detail) &&
    (job.check === undefined || (!!job.check && typeof job.check === 'object'))
}

function validGpuId(value) { return typeof value === 'string' && /^[A-Za-z0-9_.-]{1,128}$/.test(value) }
