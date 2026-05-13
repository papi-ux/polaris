import { describe, expect, it } from 'vitest'
import { AUTO_QUALITY_STATES, resolveAutoQualityState } from './auto-quality-state'

function streamingStats(overrides = {}) {
  return {
    streaming: true,
    width: 1920,
    height: 1080,
    fps: 118,
    session_target_fps: 120,
    bitrate_kbps: 30000,
    adaptive_bitrate_enabled: true,
    ai_auto_quality_enabled: true,
    adaptive_target_bitrate_kbps: 30000,
    ai_enabled: true,
    codec: 'hevc_nvenc',
    capture_transport: 'dmabuf',
    capture_residency: 'gpu',
    encode_target_residency: 'gpu',
    capture_gpu_native: true,
    health: {
      grade: 'good',
      summary: 'Session looks steady.',
      safe_bitrate_kbps: 30000,
      safe_display_mode: 'headless',
    },
    ...overrides,
  }
}

describe('Auto Quality UI state', () => {
  it('reports a healthy quality run as stable', () => {
    const state = resolveAutoQualityState(streamingStats())

    expect(state.state).toBe(AUTO_QUALITY_STATES.STABLE)
    expect(state.label).toBe('Auto Quality Stable')
    expect(state.targetSummary).toContain('1920x1080@120')
    expect(state.targetSummary).toContain('HEVC')
  })

  it('turns adaptive target drops into a recovering state', () => {
    const state = resolveAutoQualityState(streamingStats({
      adaptive_target_bitrate_kbps: 12000,
      adaptive_base_bitrate_kbps: 30000,
    }))

    expect(state.state).toBe(AUTO_QUALITY_STATES.RECOVERING)
    expect(state.label).toBe('Recovering Bitrate')
    expect(state.compactLabel).toBe('12 Mbps')
  })

  it('gives host-render-limited sessions a specific label', () => {
    const state = resolveAutoQualityState(streamingStats({
      fps: 82,
      health: {
        grade: 'watch',
        primary_issue: 'host_render_limited',
        host_render_limited: true,
      },
    }))

    expect(state.state).toBe(AUTO_QUALITY_STATES.RECOVERING)
    expect(state.label).toBe('Host Render Limited')
    expect(state.compactLabel).toBe('Host')
  })

  it('surfaces manual override before stable stream state', () => {
    const state = resolveAutoQualityState(streamingStats(), {
      state: 'manual_override',
      manualOverride: true,
      message: 'Retroid is using a manual quality preset.',
    })

    expect(state.state).toBe(AUTO_QUALITY_STATES.MANUAL_OVERRIDE)
    expect(state.label).toBe('Manual Override')
    expect(state.detail).toContain('Retroid')
  })

  it('treats CPU capture as attention', () => {
    const state = resolveAutoQualityState(streamingStats({
      capture_gpu_native: false,
      capture_transport: 'shm',
      capture_residency: 'cpu',
    }))

    expect(state.state).toBe(AUTO_QUALITY_STATES.NEEDS_ATTENTION)
    expect(state.label).toBe('Capture Attention')
  })

  it('reports Auto Quality as off when both tuning engines are disabled', () => {
    const state = resolveAutoQualityState(streamingStats({
      ai_auto_quality_enabled: false,
      adaptive_bitrate_enabled: false,
      ai_enabled: false,
    }))

    expect(state.state).toBe(AUTO_QUALITY_STATES.OFF)
    expect(state.enabled).toBe(false)
  })

  it('prefers the unified Auto Quality field over legacy component state', () => {
    const state = resolveAutoQualityState(streamingStats({
      ai_auto_quality_enabled: false,
      adaptive_bitrate_enabled: true,
      ai_enabled: true,
    }))

    expect(state.state).toBe(AUTO_QUALITY_STATES.OFF)
    expect(state.enabled).toBe(false)
  })
})
