// Which kind of client a paired device is. The host marks a device "nova" the first time it calls
// the Polaris API, which only Nova does; every other client speaks only the Moonlight protocol.
export function clientFamilyLabel(device) {
  return device?.client_family === 'nova' ? 'Nova' : 'Moonlight / Artemis'
}

// The kind a live stream carries, which the host reads from the stream's own pairing record when it
// starts: 'nova', or 'moonlight' for a client that speaks only the Moonlight protocol. Empty when the
// host did not say, as an older host never does.
export function streamClientFamilyLabel(family) {
  if (family === 'nova') return 'Nova'
  if (family === 'moonlight') return 'Moonlight / Artemis'
  return ''
}

// A stream from an older host reports its client by name only, so the kind comes from the paired
// device with that name. Two devices can share a name; when they are not the same kind, the stream's
// kind is unknown and nothing is shown.
export function liveClientFamilyLabel(name, pairedDevices) {
  if (!name || !Array.isArray(pairedDevices)) return ''
  const kinds = new Set(pairedDevices.filter((device) => device?.name === name).map(clientFamilyLabel))
  return kinds.size === 1 ? [...kinds][0] : ''
}
