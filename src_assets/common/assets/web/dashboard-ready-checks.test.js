import { describe, expect, it } from 'vitest'
import { buildReadyCheckDisplay } from './dashboard-ready-checks'

describe('Mission Control ready check display', () => {
  const checks = [
    { key: 'clients', ok: true },
    { key: 'library', ok: true },
    { key: 'audio', ok: true },
  ]

  it('collapses healthy checks into a summary state', () => {
    const display = buildReadyCheckDisplay(checks)

    expect(display.total).toBe(3)
    expect(display.passing).toBe(3)
    expect(display.allPassing).toBe(true)
    expect(display.visibleChecks).toEqual([])
  })

  it('shows only checks that need attention', () => {
    const display = buildReadyCheckDisplay([
      checks[0],
      { key: 'library', ok: false },
      { key: 'audio', ok: false },
    ])

    expect(display.passing).toBe(1)
    expect(display.allPassing).toBe(false)
    expect(display.visibleChecks.map((check) => check.key)).toEqual(['library', 'audio'])
  })
})
