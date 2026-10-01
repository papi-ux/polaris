import { resolveStreamDisplayModeAvailability } from './client-settings-sync.js'

export const LAUNCH_AS_ORDER = Object.freeze([
  'host_default', 'headless_stream', 'windowed_stream', 'gamescope_stream',
  'host_virtual_display', 'desktop_takeover', 'desktop_display',
])

const labels = {
  host_default: 'Host default', headless_stream: 'Private Stream',
  windowed_stream: 'Private Stream (GPU-native)', gamescope_stream: 'Gamescope Stream',
  host_virtual_display: 'Host Virtual Display', desktop_takeover: 'Desktop Takeover',
  desktop_display: 'Mirror Desktop',
}

export function launchAsOptions(catalog, hostDefaultLabel = '') {
  return LAUNCH_AS_ORDER.map((id) => ({
    id,
    label: id === 'host_default' ? hostDefaultLabel || labels[id] : labels[id],
    ...(id === 'host_default'
      ? { available: true, unavailableReason: '' }
      : resolveStreamDisplayModeAvailability(id, catalog)),
    descKey: `apps.launch_as_${id}_desc`,
  }))
}

// The host owns legacy mapping and writes downgrade-compatible flags and basis.
// Sending them back from a new editor would make an old flag look like a newer choice.
export function appPayloadForSave(entry) {
  const payload = { ...entry }
  delete payload['desktop-mirror']
  delete payload['virtual-display']
  delete payload['launch-as-basis']
  return payload
}

export function sentence(reason) {
  const text = String(reason || '').trim()
  return text && !text.endsWith('.') ? `${text}.` : text
}
