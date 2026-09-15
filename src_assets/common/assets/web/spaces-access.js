// Shared read-only validation for space assignment summaries.
export function validSnapshot(next) {
  if (!next || ['enabled', 'available', 'changing', 'failed'].some(key => typeof next[key] !== 'boolean') ||
      !Array.isArray(next.profiles) || ['creation_available', 'management_available', 'access_available'].some(key => next[key] !== undefined && typeof next[key] !== 'boolean')) return false
  if (next.desktop_clients !== undefined && (!Array.isArray(next.desktop_clients) ||
      next.desktop_clients.some(id => typeof id !== 'string' || !id) ||
      new Set(next.desktop_clients).size !== next.desktop_clients.length)) return false
  const profiles = new Set(), clients = new Set()
  for (const profile of next.profiles) {
    if (!profile || typeof profile.id !== 'string' || !profile.id || profiles.has(profile.id) ||
        typeof profile.name !== 'string' || !Array.isArray(profile.clients) ||
        (profile.steam !== undefined && typeof profile.steam !== 'boolean') ||
        (profile.archived !== undefined && typeof profile.archived !== 'boolean') ||
        (profile.archived && profile.clients.length)) return false
    if (profile.access_clients !== undefined && (!Array.isArray(profile.access_clients) ||
        profile.access_clients.some(id => typeof id !== 'string' || !id) ||
        new Set(profile.access_clients).size !== profile.access_clients.length ||
        (profile.archived && profile.access_clients.length))) return false
    profiles.add(profile.id)
    for (const id of profile.clients) {
      if (typeof id !== 'string' || !id || clients.has(id)) return false
      clients.add(id)
    }
  }
  return true
}
