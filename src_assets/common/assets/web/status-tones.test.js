import { describe, expect, it } from 'vitest'
import { STATUS_TONES, statusTone } from './status-tones'

describe('status tones', () => {
  it('maps pass to the success tone set', () => {
    expect(statusTone('pass')).toEqual({
      card: 'border-success/20',
      badge: 'border border-success/30 bg-success/10 text-success',
      label: 'Looks good',
    })
  })

  it('maps fail to the danger tone set', () => {
    expect(statusTone('fail')).toEqual({
      card: 'border-danger/25',
      badge: 'border border-danger/30 bg-danger/10 text-danger-bright',
      label: 'Fix first',
    })
  })

  it('maps warning to the warning tone set', () => {
    expect(statusTone('warning')).toEqual({
      card: 'border-warning/25',
      badge: 'border border-warning/30 bg-warning/10 text-warning-bright',
      label: 'Check',
    })
  })

  it('falls back to the warning tone for unknown or missing statuses', () => {
    // Port probes can grade as 'hint' and future grades must stay visible,
    // matching the historical else-branch of the view-local helpers.
    expect(statusTone('hint')).toBe(STATUS_TONES.warning)
    expect(statusTone('')).toBe(STATUS_TONES.warning)
    expect(statusTone(undefined)).toBe(STATUS_TONES.warning)
  })
})
