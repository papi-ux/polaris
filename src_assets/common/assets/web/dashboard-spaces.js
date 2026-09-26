/** Summarize a validated Spaces snapshot without treating activity as media telemetry. */
export function summarizeDashboardSpaces(state, { loaded = false, error = false, clients = [] } = {}) {
  const stale = Boolean(error)
  const enabled = loaded && state.enabled
  const activityKnown = loaded && Array.isArray(state.activity)
  const profiles = new Map((state.profiles || []).map(profile => [profile.id, profile]))
  const names = new Map(clients.map(client => [client.uuid, client.name]))
  const sessions = []
  const seen = new Set()
  for (const item of state.activity || []) {
    const key = `${item.profile_id}\0${item.client_id}`
    if (seen.has(key)) continue
    seen.add(key)
    const profile = profiles.get(item.profile_id)
    sessions.push({ key, name: profile?.name || '', family: profile?.family || '',
      client: names.get(item.client_id) || '', state: item.state })
  }
  const activeProfiles = (state.profiles || []).filter(profile => !profile.archived)
  const mismatch = activeProfiles.filter(profile => profile.runtime_mismatch === true).length
  const unsupported = activeProfiles.filter(profile => profile.family === '').length
  const jobFailed = state.runtime_move_job?.state === 'failed'
  const attention = stale || (enabled && (state.failed || !state.available || mismatch > 0 || unsupported > 0 || jobFailed))
  return {
    visible: Boolean(enabled || stale), enabled, stale, loaded, activityKnown,
    hasActivity: sessions.length > 0, total: sessions.length, sessions: sessions.slice(0, 100),
    remaining: Math.max(0, sessions.length - 100), attention, mismatch, unsupported, jobFailed,
    changing: Boolean(state.changing), failed: Boolean(state.failed), available: Boolean(state.available),
    // A dropped/old snapshot must never reopen a desktop image behind a Space session.
    previewBlocked: !loaded || stale || sessions.length > 0 ||
      (enabled && (!activityKnown || state.failed || state.changing || !state.available)),
  }
}
