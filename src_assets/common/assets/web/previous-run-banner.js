import { describePreviousRun } from './diagnostics-export.js'

/**
 * The previous-run banner on Doctor & Support. A crash and a kill are
 * different news: one is Polaris's fault until proven otherwise, the other
 * usually the OOM killer, a SIGKILL, or power loss. The banner says which,
 * and only raises the alarm tone for the crash.
 */
export function describePreviousRunBanner(crash, t) {
  const summary = describePreviousRun(crash || {})
  if (!summary) return null
  const crashed = String(crash?.outcome || '').toLowerCase() === 'crashed'
  return {
    crashed,
    tone: crashed ? 'danger' : 'warning',
    title: t(crashed ? 'troubleshooting.previous_run_crashed_title' : 'troubleshooting.previous_run_unclean_title'),
    summary,
    startedAt: String(crash?.started_at || ''),
  }
}
