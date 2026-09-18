const PRIVATE_STREAM_MODES = new Set(['headless_stream', 'windowed_stream'])

// The values linux_virtual_display_backend accepts, in the order Automatic
// tries them. Kept in step with parse_backend_preference on the host.
export const VIRTUAL_DISPLAY_BACKEND_OPTIONS = Object.freeze([
  { value: 'auto', label: 'Automatic' },
  { value: 'evdi', label: 'EVDI' },
  { value: 'kwin', label: 'KWin Virtual Screen' },
  { value: 'wlr', label: 'Hyprland' },
  { value: 'kscreen', label: 'Borrowed Connector (kscreen-doctor)' },
])

export const KWIN_VIRTUAL_OUTPUT_BACKEND = 'KWin virtual output'

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

function connectorRank(output) {
  if (output.connected && !output.enabled) return 0
  if (output.connected) return 1
  return 2
}

// The host strips the cardN- prefix, so a desktop with an iGPU and a discrete
// card can report DP-1 twice. kscreen-doctor addresses outputs by that bare
// name, so fold duplicates into one entry that is connected or enabled when
// either card says so.
function mergeConnectorsByName(outputs) {
  if (!Array.isArray(outputs)) return []
  const byName = new Map()
  for (const output of outputs) {
    const name = typeof output?.name === 'string' ? output.name.trim() : ''
    if (!name) continue
    const previous = byName.get(name)
    byName.set(name, {
      name,
      connected: previous?.connected === true || output.connected === true,
      enabled: previous?.enabled === true || output.enabled === true,
    })
  }
  return [...byName.values()]
}

// Connectors from /api/linux/display-outputs, spare ones first: a connected
// connector that is not lighting the desktop is what a dummy plug looks like.
export function kscreenConnectorOptions(outputs) {
  return mergeConnectorsByName(outputs)
    .map((output) => {
      const state = !output.connected ? 'nothing plugged in' : output.enabled ? 'in use' : 'connected, not in use'
      return { ...output, label: `${output.name} (${state})` }
    })
    .sort((a, b) => connectorRank(a) - connectorRank(b) || a.name.localeCompare(b.name))
}

// kscreen-doctor cannot create a display. For the length of a stream it borrows
// the connector named in linux_streaming_output, so the operator needs to know
// whether the running host has loaded that choice yet and whether it is a
// monitor they are looking at. `loaded` is the connector the running host
// holds, or null when the page could not find out.
export function presentKscreenConnector({
  selected = '',
  loaded = null,
  available = false,
  primary = '',
  outputs = null,
} = {}) {
  const name = String(selected || '').trim()
  const hostKnown = typeof loaded === 'string'
  const hostName = hostKnown ? loaded.trim() : ''
  const warnings = []

  if (!name) {
    if (available) {
      return {
        kind: 'pending',
        message: hostName
          ? `Polaris is still using ${hostName}. Clearing it takes effect after you save and restart Polaris.`
          : 'Polaris still has a connector loaded. Clearing it takes effect after you save and restart Polaris.',
        warnings,
      }
    }
    return {
      kind: 'unset',
      message: 'No connector chosen yet. Host Virtual Display stays unavailable until you pick one, save, and restart Polaris.',
      warnings,
    }
  }

  const merged = mergeConnectorsByName(outputs)
  const output = merged.find((candidate) => candidate.name === name)
  if (name === String(primary || '').trim()) {
    warnings.push(`${name} is also set as your primary output. Host Virtual Display will take over that monitor for the stream, which looks the same as Mirror Desktop. Pick a spare connector with a dummy plug instead.`)
  } else if (output?.connected && output.enabled) {
    warnings.push(`${name} is on and part of your desktop right now, which is expected for a dummy plug. If it is a monitor you use, pick a spare connector instead, because Host Virtual Display will take it over for the stream.`)
  }
  if (merged.length > 0) {
    if (!output) {
      warnings.push(`No DRM connector named ${name} was found on this host. kscreen-doctor -o lists the names it expects.`)
    } else if (!output.connected) {
      warnings.push(`Nothing is plugged into ${name}, so there is no display to turn on and the stream will not start.`)
    }
  }

  if (available && !hostKnown) {
    return {
      kind: 'configured',
      message: `Polaris has a connector loaded. If ${name} is a new choice, it takes effect after you save and restart Polaris.`,
      warnings,
    }
  }
  if (available && hostName === name) {
    return { kind: 'ready', message: `Host Virtual Display will use ${name}.`, warnings }
  }
  return {
    kind: 'pending',
    message: available && hostName
      ? `Polaris is still using ${hostName}. ${name} takes effect after you save and restart Polaris.`
      : `${name} takes effect after you save and restart Polaris.`,
    warnings,
  }
}
