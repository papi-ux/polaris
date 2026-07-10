import { describe, expect, it } from 'vitest'
import {
  DISPLAY_PLANNER_SCALE_FACTORS,
  buildResolutionPlanner,
  clampPlannerScale,
  formatDisplayMode,
  parseDisplayMode,
} from './display-resolution-planner'

describe('display resolution planner', () => {
  it('frames Balanced as the normal-user recommendation instead of dumping raw modes first', () => {
    const planner = buildResolutionPlanner({ fallbackMode: '2560x1600x90' })

    expect(planner.recommendedId).toBe('balanced')
    expect(planner.recommendedTitle).toBe('Best for this device')
    expect(planner.recommendedMode).toBe('1920x1200x90')
    expect(planner.sourceAspectRatio).toBe('8:5')
    expect(planner.visibleChoices.map((choice) => choice.title)).toEqual([
      'Native',
      'Balanced',
      'Performance',
    ])
  })

  it('keeps supersampling and custom controls behind advanced visibility', () => {
    const simple = buildResolutionPlanner({ fallbackMode: '1920x1080x60', showAdvanced: false })
    const advanced = buildResolutionPlanner({ fallbackMode: '1920x1080x60', showAdvanced: true, customScale: 1.25 })

    expect(simple.visibleChoices.map((choice) => choice.id)).not.toContain('sharp')
    expect(simple.visibleChoices.map((choice) => choice.id)).not.toContain('custom')
    expect(advanced.visibleChoices.map((choice) => choice.id)).toEqual([
      'native',
      'balanced',
      'sharp',
      'performance',
      'custom',
    ])
    expect(advanced.visibleChoices.find((choice) => choice.id === 'custom').targetMode).toBe('2400x1350x60')
  })

  it('hides unsafe or excessive modes even when advanced controls are open', () => {
    const planner = buildResolutionPlanner({
      fallbackMode: '7680x4320x60',
      showAdvanced: true,
      limits: { maxWidth: 7680, maxHeight: 4320, maxPixels: 7680 * 4320 },
    })

    expect(planner.visibleChoices.map((choice) => choice.id)).toEqual([
      'native',
      'balanced',
      'performance',
      'custom',
    ])
    expect(planner.choices.find((choice) => choice.id === 'sharp').hidden).toBe(true)
    expect(planner.choices.find((choice) => choice.id === 'sharp').safe).toBe(false)
  })

  it('exposes only the supported scale-factor set and clamps custom values', () => {
    expect(DISPLAY_PLANNER_SCALE_FACTORS).toEqual([0.5, 0.75, 1, 1.25, 1.5, 2])
    expect(clampPlannerScale(0.1)).toBe(0.5)
    expect(clampPlannerScale(3)).toBe(2)
  })

  it('parses and formats Moonlight-compatible WxHxFPS display modes', () => {
    expect(parseDisplayMode('3840x2160x120')).toEqual({ width: 3840, height: 2160, fps: 120 })
    expect(formatDisplayMode({ width: 1920, height: 1080, fps: 60 })).toBe('1920x1080x60')
  })
})
