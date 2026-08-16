const PRIVATE_STREAM_MODES = new Set(['headless_stream', 'windowed_stream'])

export function presentVirtualDisplayStatus(status = {}) {
  if (PRIVATE_STREAM_MODES.has(status.policy_mode)) {
    return {
      kind: 'unused',
      label: 'Not needed for Private Stream',
      detail: 'Private Stream creates its own compositor output, so host virtual-display creation is intentionally skipped.',
    }
  }

  if (status.available) {
    return {
      kind: 'available',
      label: 'Available',
      detail: `${status.backend || 'The detected backend'} is ready to create or manage the stream output.`,
    }
  }

  if (status.backend_detected) {
    return {
      kind: 'unconfigured',
      label: 'Configuration required',
      detail: status.unavailable_reason || `${status.backend || 'The detected backend'} needs additional configuration.`,
    }
  }

  return {
    kind: 'missing',
    label: 'Not available',
    detail: status.unavailable_reason || 'No supported host virtual-display backend was detected.',
  }
}
