import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import {
  buildLiveSummary,
  buildQualityGrade,
  buildQualityScore,
  dashboardLossPct,
} from './dashboard-summary'

// A second of the Retroid Pocket 6's HEVC run: the newest report lost 7.38% of its frames, while the
// host's window, which Doctor judges, holds 1.87%, light and below the 2% that makes pressure.
const hevcSecond = {
  streaming: true,
  fps: 119.4,
  session_target_fps: 120,
  latency_ms: 14,
  packet_loss: 7.38,
  bitrate_kbps: 45000,
  encode_time_ms: 3.2,
  network_verdict: {
    loss_state: 'light',
    loss_pct: 1.87,
    frames_lost: 18,
    frames_expected: 963,
    window_seconds: 20,
  },
}

describe('Mission Control dashboard summary helpers', () => {
  it('scores and summarizes a healthy live stream', () => {
    const stats = {
      streaming: true,
      fps: 119.4,
      session_target_fps: 120,
      latency_ms: 14,
      packet_loss: 0,
      bitrate_kbps: 45000,
      encode_time_ms: 3.2,
    }
    const score = buildQualityScore(stats)
    const grade = buildQualityGrade(score)
    const summary = buildLiveSummary({ stats, qualityGrade: grade, qualityScore: score, gradeTone: 'text-success' })

    expect(score).toBeGreaterThanOrEqual(90)
    expect(grade).toBe('A')
    expect(summary.qualityDetail).toBe('Locked in')
    expect(summary.bitrate).toBe('45.0 Mbps')
  })

  it('shows, colours and grades the loss Doctor judged, not the newest one second report', () => {
    const summary = buildLiveSummary({ stats: hevcSecond })
    expect(summary.loss).toBe('1.9%')
    expect(summary.lossTone).toBe('text-warning')
    expect(summary.lossTitle).toBe('Video frames lost after FEC over the last 20 s')
    expect(dashboardLossPct(hevcSecond)).toBe(1.87)
    // The newest second's 7.38% would have cost 30 points; the window's 1.87% costs its share.
    expect(buildQualityScore(hevcSecond)).toBe(Math.round(100 - 1.87 * 5))

    const clean = { ...hevcSecond, packet_loss: 7.38, network_verdict: { ...hevcSecond.network_verdict, loss_state: 'clean', loss_pct: 0.4 } }
    expect(buildLiveSummary({ stats: clean }).lossTone).toBe('text-success')
    const pressure = { ...hevcSecond, network_verdict: { ...hevcSecond.network_verdict, loss_state: 'elevated', loss_pct: 2.4 } }
    expect(buildLiveSummary({ stats: pressure }).lossTone).toBe('text-danger')
  })

  it('shows no loss figure until the host has judged one', () => {
    for (const verdict of [undefined, { loss_state: 'collecting', loss_pct: null }, { loss_state: 'stale', loss_pct: null }]) {
      const stats = { ...hevcSecond, network_verdict: verdict }
      const summary = buildLiveSummary({ stats })
      expect(summary.loss).toBe('--')
      expect(summary.lossTone).toBe('text-storm')
      expect(summary.lossTitle).toBe('Video frame loss is not judged yet')
      expect(dashboardLossPct(stats)).toBeNull()
      expect(buildQualityScore(stats)).toBe(100)
    }
  })

  it('charts the judged loss and shows a client row only while the host serves its loss', () => {
    const dashboard = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/DashboardView.vue'), 'utf8')
    expect(dashboard).toContain('lossHistory.value.push(dashboardLossPct(newStats))')
    expect(dashboard).not.toContain('newStats.packet_loss')
    expect(dashboard).toContain('client.packet_loss_available && Number.isFinite(client.packet_loss)')
    expect(dashboard).toContain(':title="liveSummary.lossTitle"')
  })


})
