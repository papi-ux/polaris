import { describe, expect, it } from 'vitest'

import { saveNeedsRestart } from './config-save-outcome.js'

describe('saveNeedsRestart', () => {
  it('skips the restart only when the host says the change is already live', () => {
    expect(saveNeedsRestart({ status: true, restart_required: false })).toBe(false)
    expect(saveNeedsRestart({ status: true, restart_required: true })).toBe(true)
  })

  it('keeps asking for a restart when the host predates live apply', () => {
    expect(saveNeedsRestart({ status: true })).toBe(true)
    expect(saveNeedsRestart(null)).toBe(true)
    expect(saveNeedsRestart(undefined)).toBe(true)
  })
})
