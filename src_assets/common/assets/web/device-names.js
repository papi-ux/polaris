import { formatClientTimestamp } from './client-timestamps.js'

// The name a person sees for a paired device: its friendly name, or the name it paired with.
export function deviceBaseName(device) {
  return String(device?.friendly_name || device?.name || '').trim()
}

// Two devices can pair under the same name, for example two builds of Nova on one handheld. When
// names collide, each label adds when that device paired, and if even that repeats, a number in
// pairing order. A unique name stays exactly as it is.
export function deviceNameLabels(devices, { t, fallback = '', locale, timeZone } = {}) {
  const labels = new Map()
  const groups = new Map()
  for (const device of Array.isArray(devices) ? devices : []) {
    if (!device?.uuid) continue
    const name = deviceBaseName(device) || fallback
    const key = name.toLocaleLowerCase()
    if (!groups.has(key)) groups.set(key, [])
    groups.get(key).push({ device, name })
  }
  for (const group of groups.values()) {
    if (group.length === 1) {
      labels.set(group[0].device.uuid, group[0].name)
      continue
    }
    const ordered = [...group].sort((a, b) =>
      (Number(a.device.paired_at) || 0) - (Number(b.device.paired_at) || 0) ||
      String(a.device.uuid).localeCompare(String(b.device.uuid)))
    const paired = ordered.map(({ device }) => formatClientTimestamp(device.paired_at, locale, timeZone, ''))
    const readable = paired.every(Boolean) && new Set(paired).size === paired.length
    ordered.forEach(({ device, name }, index) => {
      labels.set(device.uuid, readable
        ? t('_common.device_name_paired', { name, paired: paired[index] })
        : t('_common.device_name_numbered', { name, number: index + 1 }))
    })
  }
  return labels
}
