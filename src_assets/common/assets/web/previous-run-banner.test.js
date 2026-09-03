import { describe, expect, it } from 'vitest'
import { describePreviousRunBanner } from './previous-run-banner.js'

const t = (key) => key.replace(/^troubleshooting\./, '')

describe('previous run banner', () => {
  it('stays silent for a clean or unrecorded exit', () => {
    expect(describePreviousRunBanner(null, t)).toBeNull()
    expect(describePreviousRunBanner({ recorded: true, outcome: 'clean' }, t)).toBeNull()
    expect(describePreviousRunBanner({ recorded: false, outcome: 'crashed' }, t)).toBeNull()
  })

  it('raises the alarm tone for a real crash and names the signal', () => {
    const banner = describePreviousRunBanner({ recorded: true, outcome: 'crashed', signal_name: 'SIGSEGV', signal_number: 11, started_at: '2026-09-02T20:50:37Z' }, t)
    expect(banner).toMatchObject({ crashed: true, tone: 'danger', title: 'previous_run_crashed_title', startedAt: '2026-09-02T20:50:37Z' })
    expect(banner.summary).toContain('SIGSEGV (11)')
  })

  it('keeps a kill at warning tone and says it was probably not a fault', () => {
    const banner = describePreviousRunBanner({ recorded: true, outcome: 'unclean' }, t)
    expect(banner).toMatchObject({ crashed: false, tone: 'warning', title: 'previous_run_unclean_title', startedAt: '' })
    expect(banner.summary).toMatch(/killed rather than/)
  })
})
