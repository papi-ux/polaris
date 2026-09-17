// Shared read-only validation for space assignment summaries.
export function validSnapshot(next) {
  if (!next || (next.schema !== undefined && next.schema !== 1) ||
      (next.capacity !== undefined && (!next.capacity || typeof next.capacity !== 'object' ||
        ['concurrent_limit', 'concurrent_active'].some(key => !Number.isInteger(next.capacity[key]) || next.capacity[key] < 0))) ||
      ['enabled', 'available', 'changing', 'failed'].some(key => typeof next[key] !== 'boolean') ||
      !Array.isArray(next.profiles) || ['creation_available', 'management_available', 'access_available', 'removal_available'].some(key => next[key] !== undefined && typeof next[key] !== 'boolean')) return false
  if (next.desktop_clients !== undefined && (!Array.isArray(next.desktop_clients) ||
      next.desktop_clients.some(id => typeof id !== 'string' || !id) ||
      new Set(next.desktop_clients).size !== next.desktop_clients.length)) return false
  if (next.desktop_default_clients !== undefined && (!Array.isArray(next.desktop_default_clients) ||
      next.desktop_default_clients.some(id => typeof id !== 'string' || !id) ||
      new Set(next.desktop_default_clients).size !== next.desktop_default_clients.length)) return false
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
  // One Default Space per device: Desktop or a Space, never both.
  if ((next.desktop_default_clients || []).some(id => clients.has(id))) return false
  if (next.activity !== undefined && (!Array.isArray(next.activity) || next.activity.length > 4096 ||
      next.activity.some(item => !item || !profiles.has(item.profile_id) ||
        typeof item.client_id !== 'string' || !item.client_id ||
        !['starting', 'running', 'stopping'].includes(item.state)))) return false
  return true
}
