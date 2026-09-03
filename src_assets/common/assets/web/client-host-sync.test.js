import { describe, expect, it } from 'vitest'
import { CLIENT_HOST_FIELDS, buildClientHostSyncRows } from './client-host-sync.js'

const t = (key, params = {}) => {
  const base = key.replace(/^pin\./, '')
  const values = Object.entries(params).map(([k, v]) => `${k}=${v}`).join(',')
  return values ? `${base}(${values})` : base
}

const fields = {
  display_mode: { desired: '1920x1080x120', effective: '1920x1080x120', live: false, requires_relaunch: false, scope: 'paired_client', status: 'synced' },
  target_bitrate_kbps: { desired: 0, effective: 0, live: true, requires_relaunch: false, source: 'client_request', source_label: 'Client request', status: 'synced' },
  stream_display_mode: { desired: 'headless_stream', effective: 'headless_stream', live: false, requires_relaunch: true, scope: 'host', status: 'synced' },
  adaptive_bitrate_enabled: { desired: false, effective: false, live: true, requires_relaunch: false, status: 'synced' },
  ai_auto_quality_enabled: { desired: false, effective: false, status: 'deprecated' },
  disconnect_resume_timeout_seconds: { desired: 300, effective: 300, live: true, status: 'synced' },
  client_runtime: { desired: {}, effective: {}, live: true, status: 'pending' },
}

describe('client host sync rows', () => {
  it('stays empty without a projection', () => {
    expect(buildClientHostSyncRows(null, t)).toEqual([])
    expect(buildClientHostSyncRows(undefined, t)).toEqual([])
  })

  it('renders only the player-facing scalar fields, in a fixed order, and skips deprecated ones', () => {
    const rows = buildClientHostSyncRows(fields, t)
    expect(rows.map((row) => row.key)).toEqual(CLIENT_HOST_FIELDS)
    expect(rows.map((row) => row.key)).not.toContain('ai_auto_quality_enabled')
    expect(rows.map((row) => row.key)).not.toContain('client_runtime')
  })

  it('formats values and notes the way a player reads them', () => {
    const rows = Object.fromEntries(buildClientHostSyncRows(fields, t).map((row) => [row.key, row]))
    expect(rows.display_mode).toMatchObject({ label: 'host_field_display_mode', value: '1920x1080x120', note: '' })
    expect(rows.target_bitrate_kbps).toMatchObject({ value: 'host_value_client_choice', note: 'Client request, host_sync_live' })
    expect(rows.stream_display_mode).toMatchObject({ value: 'headless_stream', note: 'host_sync_pending' })
    expect(rows.adaptive_bitrate_enabled).toMatchObject({ value: 'host_value_off', note: 'host_sync_live' })
    expect(rows.disconnect_resume_timeout_seconds).toMatchObject({ value: 'host_value_seconds(seconds=300)' })
  })

  it('prefers the host label for the stream display mode and shows a real bitrate as kbps', () => {
    const rows = Object.fromEntries(buildClientHostSyncRows({
      ...fields,
      target_bitrate_kbps: { ...fields.target_bitrate_kbps, effective: 25000 },
    }, t, { streamDisplay: { effective_label: 'Private Stream' } }).map((row) => [row.key, row]))
    expect(rows.stream_display_mode.value).toBe('Private Stream')
    expect(rows.target_bitrate_kbps.value).toBe('host_value_kbps(kbps=25000)')
  })

  it('never renders undefined for a sparse field', () => {
    const rows = buildClientHostSyncRows({ display_mode: {}, target_bitrate_kbps: { status: 'pending' } }, t)
    expect(rows.map((row) => row.value)).toEqual(['host_value_unset', 'host_value_unset'])
    expect(rows[1].note).toBe('host_sync_status_pending')
  })
})
