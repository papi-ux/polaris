// What a paired device may open, and where it opens first, read from one Spaces snapshot. The
// Devices table and the section around it ask the same questions, so they ask them here.
import { permissionMapping } from './composables/useClients.js'

// A device that can be given a Space: paired for good, with permission to launch.
export const canLaunch = client => !!client && !client.temporary_authorization && (Number(client.perm) & permissionMapping.launch) !== 0

export const activeSpaces = state => (state?.profiles || []).filter(space => !space.archived)

// A device is in a Space when it may open it: as its Default Space, or under Device Access.
export const inSpace = (space, id) => (space.clients || []).includes(id) || (space.access_clients || []).includes(id)

export const spacesFor = (state, id) => activeSpaces(state).filter(space => inSpace(space, id))

export const hasDesktop = (state, id) => (state?.desktop_clients || []).includes(id)

// A default is only ever a place the device may play. Desktop is one with Desktop Access, or when
// the device has no Space at all.
export const offersDesktop = (state, id) => hasDesktop(state, id) || !spacesFor(state, id).length

export const placesFor = (state, id) => [...(offersDesktop(state, id) ? ['desktop'] : []), ...spacesFor(state, id).map(space => space.id)]

// Where the device opens first, in the host's order: a Desktop default, its Default Space, then
// the first Space it may open, then Desktop.
export function opensFirst(state, id) {
  if ((state?.desktop_default_clients || []).includes(id) && hasDesktop(state, id)) return 'desktop'
  return activeSpaces(state).find(space => (space.clients || []).includes(id))?.id || spacesFor(state, id)[0]?.id || 'desktop'
}

// The devices the table lists: every one that can be given a Space, and any that lost that
// permission while a Space still lists it, so it can be cleaned up.
export const listedDevices = (state, clients) => (clients || []).filter(client => canLaunch(client) || spacesFor(state, client.uuid).length)
