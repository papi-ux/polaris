import { reading } from './mission-control.js'

// How long a stream has run: the host's own count when it sends one, which holds for a page opened
// mid-stream and for a browser whose clock is wrong; otherwise the start this page saw. Neither
// known is an empty label, never a guess.
export function sessionElapsedLabel({ hostSeconds, witnessedStartMs = 0, now = Date.now() } = {}) {
  const host = reading(hostSeconds)
  const total = host !== null ? Math.max(0, Math.round(host)) : witnessedStartMs > 0 ? Math.max(0, Math.round((now - witnessedStartMs) / 1000)) : null
  if (total === null) return ''
  if (total < 60) return `${total}s`
  const minutes = Math.floor(total / 60)
  if (minutes < 60) return `${minutes} min`
  return `${Math.floor(minutes / 60)} h ${minutes % 60} min`
}

// The connection chip says only what the host confirmed from tailscaled's peer status. Anything
// else, including a stream on the local network, has no chip: an address is not evidence.
export function connectionChip(evidence, t = key => key) {
  if (evidence?.scope !== 'peer_status' || evidence.confirmed !== true) return null
  const transport = ['direct', 'derp', 'peer_relay'].includes(evidence.transport) ? evidence.transport : 'unknown'
  return { transport, label: t(`mission_control.connection_${transport}`), note: t('mission_control.connection_scope') }
}
