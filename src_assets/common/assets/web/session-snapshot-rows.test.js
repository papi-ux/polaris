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

  it('summarises stream stats for the advanced diagnostics tile', () => {
    expect(summarizeStreamStats({}, t)).toBe('snapshot_no_active_stream')
    expect(summarizeStreamStats(streaming, t)).toBe('snapshot_stream_summary(fps=118.4 FPS,target=120.0 FPS,kbps=16988,loss=0.00,encode=0)')
  })
})
