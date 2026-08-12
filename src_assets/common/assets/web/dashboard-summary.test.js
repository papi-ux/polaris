import { describe, expect, it } from 'vitest'
import {
  buildLiveSummary,
  buildQualityGrade,
  buildQualityScore,
} from './dashboard-summary'

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


})
