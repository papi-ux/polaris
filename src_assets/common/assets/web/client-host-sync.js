// Per-device host view for the Devices page: what the host applies for one
// paired client, read from GET /api/settings/metadata?client=<uuid>. The
// projection's `fields` carry every synced setting; only the player-facing
// scalar ones become rows here, the structured client_* blocks stay internal.

export const CLIENT_HOST_FIELDS = Object.freeze([
  'display_mode',
  'target_bitrate_kbps',
  'stream_display_mode',
  'adaptive_bitrate_enabled',
  'disconnect_resume_timeout_seconds',
])

function formatValue(name, value, t) {
  if (value === null || value === undefined || value === '') return t('pin.host_value_unset')
  if (typeof value === 'boolean') return t(value ? 'pin.host_value_on' : 'pin.host_value_off')
  if (name === 'target_bitrate_kbps') {
    const kbps = Number(value)
    return kbps > 0 ? t('pin.host_value_kbps', { kbps }) : t('pin.host_value_client_choice')
  }
  if (name === 'disconnect_resume_timeout_seconds') {
    return t('pin.host_value_seconds', { seconds: Number(value) })
  }
  return String(value)
}

export function buildClientHostSyncRows(projection, t, { streamDisplay = null } = {}) {
  const fields = projection && typeof projection === 'object' ? projection : null
  if (!fields) return []
  const rows = []
  for (const name of CLIENT_HOST_FIELDS) {
    const field = fields[name]
    if (!field || typeof field !== 'object' || field.status === 'deprecated') continue
    let value = field.effective !== undefined && field.effective !== null && field.effective !== '' ? field.effective : field.desired
    if (name === 'stream_display_mode' && streamDisplay?.effective_label) value = streamDisplay.effective_label
    const notes = []
    if (typeof field.source_label === 'string' && field.source_label.trim()) notes.push(field.source_label.trim())
    if (field.requires_relaunch === true) notes.push(t('pin.host_sync_pending'))
    else if (field.live === true) notes.push(t('pin.host_sync_live'))
    if (field.status === 'pending') notes.push(t('pin.host_sync_status_pending'))
    rows.push({
      key: name,
      label: t(`pin.host_field_${name}`),
      value: formatValue(name, value, t),
      note: notes.join(', '),
    })
  }
  return rows
}
