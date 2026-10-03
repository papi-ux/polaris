import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import {
  buildSessionSnapshotRows,
  captureReasonDescription,
  fpsTargetGapDescription,
  runtimeOverrideDescription,
  summarizeStreamStats,
} from './session-snapshot-rows.js'

const t = (key, params = {}) => {
  const base = key.replace(/^troubleshooting\./, '')
  const values = Object.entries(params).map(([k, v]) => `${k}=${v}`).join(',')
  return values ? `${base}(${values})` : base
}

const enLocale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))

// The console's English, so an assertion reads what a host sees.
function english(key, params = {}) {
  const message = key.split('.').reduce((node, part) => node?.[part], enLocale)
  if (typeof message !== 'string') return key
  return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
}

const streaming = {
  streaming: true,
  client_name: 'RetroidPocket6',
  width: 1920,
  height: 1080,
  codec: 'hevc',
  fps: 118.4,
  session_target_fps: 120,
  requested_client_fps: 120,
  bitrate_kbps: 16988,
  client_ip: '10.0.0.180',
  active_sessions: 1,
  runtime_backend: 'kms',
  stream_display_mode: 'Mirror Desktop',
  capture_path: 'mixed_or_unknown',
  capture_path_reason: 'headless_shm_default',
  latency_ms: 4.96,
  packet_loss: 0,
}

describe('session snapshot rows', () => {
  it('stays empty until a stream is active', () => {
    expect(buildSessionSnapshotRows(null, t)).toEqual({ summary: [], details: [] })
    expect(buildSessionSnapshotRows({ streaming: false }, t)).toEqual({ summary: [], details: [] })
  })

  it('leads with client, resolution, codec, and FPS and keeps the rest as details', () => {
    const rows = buildSessionSnapshotRows(streaming, t)
    expect(rows.summary.map((row) => row.label)).toEqual(['snapshot_client', 'snapshot_resolution', 'snapshot_codec', 'snapshot_fps'])
    expect(rows.summary[0].value).toBe('RetroidPocket6')
    expect(rows.summary[1].value).toBe('1920x1080')
    expect(rows.summary[3].value).toBe('snapshot_fps_value(encoded=118.4 FPS,target=120.0 FPS)')
    expect(rows.details.find((row) => row.label === 'snapshot_capture_reason').value).toBe('capture_reason_headless_shm')
    expect(rows.details.some((row) => row.label === 'snapshot_last_write')).toBe(false)
  })

  it('names the network path by kind and points a tailnet at the relay check', () => {
    const tailnet = buildSessionSnapshotRows({ ...streaming, client_network_path: 'cgnat' }, t)
    const row = tailnet.details.find((entry) => entry.label === 'snapshot_network_path')
    expect(row.value).toBe('snapshot_network_path_cgnat')
    expect(row.note).toBe('snapshot_network_path_relay_note')

    const lan = buildSessionSnapshotRows({ ...streaming, client_network_path: 'link-local' }, t)
    const lanRow = lan.details.find((entry) => entry.label === 'snapshot_network_path')
    expect(lanRow.value).toBe('snapshot_network_path_link_local')
    expect(lanRow.note).toBeUndefined()
  })

  it('names the kind of client and, for a Moonlight-protocol client, what it cannot use', () => {
    const moonlight = buildSessionSnapshotRows({ ...streaming, client_family: 'moonlight' }, t)
    const row = moonlight.details.find((entry) => entry.label === 'snapshot_client_family')
    expect(row).toEqual({
      label: 'snapshot_client_family',
      value: 'Moonlight / Artemis',
      note: 'snapshot_client_family_moonlight_note',
    })
    // Beside the address the client came from.
    const labels = moonlight.details.map((entry) => entry.label)
    expect(labels.indexOf('snapshot_client_family')).toBe(labels.indexOf('snapshot_client_ip') + 1)

    const nova = buildSessionSnapshotRows({ ...streaming, client_family: 'nova' }, t)
    expect(nova.details.find((entry) => entry.label === 'snapshot_client_family')).toEqual({
      label: 'snapshot_client_family',
      value: 'Nova',
    })

    // An older host says nothing, and nothing is not Moonlight.
    const older = buildSessionSnapshotRows(streaming, t)
    expect(older.details.find((entry) => entry.label === 'snapshot_client_family').value).toBe('snapshot_unknown')
  })

  it('says when a Display Mode Override replaced the mode the client asked for', () => {
    const rows = buildSessionSnapshotRows({
      ...streaming,
      display_mode_decision: { requested: '1920x1080x60', applied: '3840x2160x60', pinned_by_host: true },
    }, t)
    expect(rows.details.find((entry) => entry.label === 'snapshot_display_mode').value)
      .toBe('snapshot_display_mode_overridden(applied=3840x2160x60,requested=1920x1080x60)')

    const chosen = buildSessionSnapshotRows({
      ...streaming,
      display_mode_decision: { requested: '1280x800x60', applied: '1280x800x60', pinned_by_host: false },
    }, t)
    expect(chosen.details.find((entry) => entry.label === 'snapshot_display_mode').value).toBe('1280x800x60')
  })

  it('never renders undefined, NaN, or an empty value for a sparse payload', () => {
    const rows = buildSessionSnapshotRows({ streaming: true }, t)
    const values = [...rows.summary, ...rows.details].map((row) => row.value)
    for (const value of values) {
      expect(value).toBeTruthy()
      expect(value).not.toMatch(/undefined|NaN/)
    }
    expect(rows.summary[0].value).toBe('snapshot_unknown')
  })

  it('reads the stream display and the last write from the host projection when served', () => {
    const rows = buildSessionSnapshotRows(streaming, t, {
      streamDisplay: { configured_label: 'Private Stream', effective_label: 'Mirror Desktop', relaunch_required: true },
      provenance: [{ at: '2026-09-02T23:00:00Z', writer: 'gamestream', keys: ['max_bitrate', 'fallback_mode'] }],
    })
    const display = rows.details.find((row) => row.label === 'snapshot_stream_display_mode')
    expect(display).toEqual({
      label: 'snapshot_stream_display_mode',
      value: 'Mirror Desktop',
      note: 'snapshot_stream_display_pending(configured=Private Stream)',
    })
    expect(rows.details.at(-1)).toEqual({
      label: 'snapshot_last_write',
      value: 'snapshot_writer_gamestream',
      note: 'snapshot_last_write_note(count=2,at=2026-09-02T23:00:00Z)',
    })
  })

  it('falls back to the stream stats mode when the projection is absent or silent', () => {
    const rows = buildSessionSnapshotRows(streaming, t, { streamDisplay: {}, provenance: [] })
    expect(rows.details.find((row) => row.label === 'snapshot_stream_display_mode')).toEqual({
      label: 'snapshot_stream_display_mode',
      value: 'Mirror Desktop',
    })
  })

  it('describes the FPS gap only for high-refresh targets that fall short', () => {
    expect(fpsTargetGapDescription({ fps: 60, session_target_fps: 60 }, t)).toBe('snapshot_none')
    expect(fpsTargetGapDescription({ fps: 118, session_target_fps: 120 }, t)).toBe('snapshot_none')
    expect(fpsTargetGapDescription({ fps: 70, session_target_fps: 120 }, t)).toBe('snapshot_fps_gap_value(encoded=70.0 FPS,target=120.0 FPS)')
  })

  it('collapses host capture reasons onto the player-facing keys', () => {
    expect(captureReasonDescription('headless_shm_fallback', t)).toBe('capture_reason_headless_shm')
    expect(captureReasonDescription('SHM_CAPTURE', t)).toBe('capture_reason_cpu_capture')
    expect(captureReasonDescription('', t)).toBe('capture_reason_unknown')
    expect(captureReasonDescription('something_new', t)).toBe('capture_reason_unknown')
  })

  it('names the GPU-native override only when it is really in effect', () => {
    expect(runtimeOverrideDescription({}, t)).toBe('snapshot_none')
    expect(runtimeOverrideDescription({ runtime_gpu_native_override_active: true }, t)).toBe('snapshot_runtime_override_active')
    expect(runtimeOverrideDescription({
      runtime_requested_headless: true,
      runtime_effective_headless: false,
      runtime_gpu_native_override_active: true,
    }, t)).toBe('snapshot_runtime_override_windowed')
  })

  it('says in English why video frame loss has no figure', () => {
    // It read "not judged yet of video frames lost over 20 s", and said "not judged yet" for the whole
    // stream to a Moonlight client, which never sends the reports loss is judged from.
    const network = (stats) => buildSessionSnapshotRows(stats, english).details
      .find((row) => row.label === 'Network').value
    const live = { ...streaming, latency_ms: 8 }
    expect(summarizeStreamStats(live, english)).toBe('118.4 FPS / 120.0 FPS target, 16988 kbps, video frame loss not judged yet, 0 ms encode')
    expect(network(live)).toBe('8.0 ms round trip / video frame loss not judged yet')
    expect(network({ ...live, client_family: 'moonlight' })).toBe('8.0 ms round trip / video frame loss not reported by Moonlight / Artemis')
    expect(network({ ...live, network_verdict: { loss_pct: null, loss_state: 'stale', rtt_median_ms: 7.9 } }))
      .toBe("7.9 ms round trip / video frame loss not judged since the client's reports stopped")
    expect(network({ ...live, network_verdict: { loss_pct: 1.869, loss_state: 'light', rtt_median_ms: 7.9 } }))
      .toBe('7.9 ms round trip / 1.87% of video frames lost over 20 s')
  })

  it('summarises stream stats for the advanced diagnostics tile', () => {
    expect(summarizeStreamStats({}, t)).toBe('snapshot_no_active_stream')
    expect(summarizeStreamStats({
      streaming: false,
      doctor: { primary_issue: 'stream_failed_to_start' },
      last_session: { client_name: 'Living Room TV' },
    }, t)).toBe('snapshot_last_stream_failed_to_start(client=Living Room TV)')
    // Past the host's window the Doctor issue is no_active_stream again, and so is the row.
    expect(summarizeStreamStats({
      streaming: false,
      doctor: { primary_issue: 'no_active_stream' },
      last_session: { client_name: 'Living Room TV' },
    }, t)).toBe('snapshot_no_active_stream')
    // Nothing judged yet says so, rather than a zero nobody measured.
    expect(summarizeStreamStats(streaming, t)).toBe('snapshot_stream_summary_unjudged(fps=118.4 FPS,target=120.0 FPS,kbps=16988,loss=snapshot_loss_not_judged,encode=0)')
    // The window's figure, the one Doctor judges, not the newest second's report.
    expect(summarizeStreamStats({
      ...streaming,
      packet_loss: 7.44,
      network_verdict: { loss_pct: 1.869, loss_state: 'light', frames_lost: 18, frames_expected: 963, window_seconds: 20, rtt_median_ms: 7.9 },
    }, t)).toBe('snapshot_stream_summary(fps=118.4 FPS,target=120.0 FPS,kbps=16988,loss=1.87%,encode=0)')
  })
})
