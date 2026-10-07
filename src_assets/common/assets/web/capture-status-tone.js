// Transport and memory residency are observations, not a stream health verdict.
export function captureStatusTone(stats = {}) {
  if (stats.streaming !== true) return 'neutral'
  const reason = String(stats.capture_path_reason || '')
  if (reason.startsWith('gpu_native_requested_') ||
      (stats.runtime_backend === 'labwc' && stats.runtime_effective_headless === false &&
       stats.capture_transport === 'shm' && stats.runtime_gpu_native_override_active === true)) {
    return 'warning'
  }
  if (stats.capture_gpu_native === true) return 'info'
  return 'neutral'
}
