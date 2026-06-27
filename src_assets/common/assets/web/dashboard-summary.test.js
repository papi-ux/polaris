import { describe, expect, it } from 'vitest'
import {
  buildFpsTargetGap,
  buildLiveSummary,
  buildMissionControlStrip,
  buildQualityGrade,
  buildQualityScore,
  buildSecondScreenOverlayRecommendation,
  buildTelemetryGuidance,
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
    const summary = buildLiveSummary({ stats, qualityGrade: grade, qualityScore: score, gradeTone: 'text-green-400' })

    expect(score).toBeGreaterThanOrEqual(90)
    expect(grade).toBe('A')
    expect(summary.qualityDetail).toBe('Locked in')
    expect(summary.bitrate).toBe('45.0 Mbps')
  })

  it('turns noisy telemetry into gamer-readable concerns', () => {
    const stats = {
      streaming: true,
      fps: 58,
      requested_client_fps: 120,
      latency_ms: 48,
      packet_loss: 2.5,
      bitrate_kbps: 30000,
      encode_time_ms: 13.4,
      capture_cpu_copy: true,
    }
    const fpsTargetGap = buildFpsTargetGap(stats)
    const guidance = buildTelemetryGuidance({
      stats,
      fpsTargetGap,
      captureReason: 'Wayland capture fell back to SHM/system-memory frames.',
      autoQuality: { enabled: false },
    })

    expect(guidance.concerns.map((concern) => concern.label)).toEqual([
      'Network risk',
      'Encoder pressure',
      'CPU copy path',
      'Frame cap likely',
    ])
    expect(guidance.recommendations[0].message).toContain('Network risk')
    expect(guidance.recommendations.at(-1).message).toContain('Enable Auto Quality')
  })

  it('builds Now Next Fix for idle launch blockers', () => {
    const strip = buildMissionControlStrip({
      statsLoaded: true,
      stats: { streaming: false },
      pairedClients: 1,
      appCatalogCount: 0,
      readyCheckDisplay: {
        primaryIssue: { label: 'Library', detail: 'Import at least one game.', to: '/apps' },
      },
    })

    expect(strip.map((item) => item.label)).toEqual(['Now', 'Next', 'Fix'])
    expect(strip[1].title).toBe('Import games')
    expect(strip[2]).toMatchObject({ title: 'Library', to: '/apps' })
  })

  it('keeps second-screen overlay as a later local lane recommendation', () => {
    const recommendation = buildSecondScreenOverlayRecommendation()

    expect(recommendation.title).toContain('second-screen overlay')
    expect(recommendation.detail).toContain('After the base cockpit is proven')
  })
})
