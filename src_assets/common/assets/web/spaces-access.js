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
        // The launcher family this Space runs. Empty means the host has one it
        // cannot stream, which is a state to show rather than a reason to
        // refuse the whole snapshot.
        (profile.family !== undefined &&
          !(typeof profile.family === 'string' && ['', 'steam', 'heroic', 'lutris'].includes(profile.family))) ||
        (profile.archived !== undefined && typeof profile.archived !== 'boolean') ||
        (profile.archived && profile.clients.length)) return false
    if (!validRuntime(profile)) return false
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
  if (next.runtime_move_available !== undefined && typeof next.runtime_move_available !== 'boolean') return false
  return next.runtime_move_job === undefined || next.runtime_move_job === null || validMoveJob(next.runtime_move_job)
}

// What a Space's gaming runtime was made for, since 1.4.11. Older hosts send none of it.
const driver = value => typeof value === 'string' && /^[0-9]+(?:\.[0-9]+)+$/.test(value) && value.length <= 32
const nullableDriver = value => value === null || value === '' || driver(value)
const runtimeId = value => typeof value === 'string' && /^[a-z0-9][a-z0-9-]{0,63}$/.test(value)
const word = value => typeof value === 'string' && /^[a-z0-9_]{1,64}$/.test(value)
function validRuntime(profile) {
  if (profile.runtime_mismatch === undefined) return true
  if (typeof profile.runtime_mismatch !== 'boolean' || !nullableDriver(profile.runtime_driver) ||
      !nullableDriver(profile.host_driver)) return false
  // A mismatch always names both drivers.
  if (profile.runtime_mismatch && (!driver(profile.runtime_driver) || !driver(profile.host_driver))) return false
  const move = profile.runtime_move
  // A mismatch always carries a move object, even one that offers nothing.
  if (move === null || move === undefined) return !profile.runtime_mismatch
  // A move is offered to repair a mismatch, or to upgrade a working Space onto
  // a runtime that borrows this PC's driver. That runtime names no driver of
  // its own, because it uses whichever one the PC has loaded.
  const upgrade = move.reason === 'host_driver_available'
  if (move.reason !== undefined && !word(move.reason)) return false
  if ((!profile.runtime_mismatch && !upgrade) || typeof move !== 'object' ||
      typeof move.available !== 'boolean' || !word(move.code)) return false
  return !move.available || (runtimeId(move.runtime_id) && typeof move.installed === 'boolean' &&
    (upgrade ? move.nvidia_driver === '' : driver(move.nvidia_driver)))
}
function validMoveJob(job) {
  const text = value => typeof value === 'string' && value.length <= 1024
  // A job that moves a Space onto the runtime that borrows this PC's driver
  // names no driver version, for the same reason the offer does not.
  return !!job && typeof job === 'object' && typeof job.request_id === 'string' && typeof job.profile_id === 'string' &&
    !!job.profile_id && runtimeId(job.runtime_id) && nullableDriver(job.nvidia_driver) &&
    ['downloading', 'moving', 'done', 'failed'].includes(job.state) && (job.code === '' || word(job.code)) &&
    text(job.message) && text(job.action)
}
