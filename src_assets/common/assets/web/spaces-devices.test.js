import { describe, expect, it } from 'vitest'
import { validSnapshot } from './spaces-access.js'
import { canLaunch, inSpace, listedDevices, offersDesktop, opensFirst, placesFor, spacesFor } from './spaces-devices.js'

const launch = 0x04000000
const state = extra => ({ enabled: true, available: true, changing: false, failed: false, profiles: [], ...extra })
const space = (id, clients = [], access = [], extra = {}) => ({ id, name: id, clients, access_clients: access, ...extra })

describe('what a device may open', () => {
  it('counts a Default Space as access, and never an archived Space', () => {
    const current = state({ profiles: [space('a', ['tv']), space('b', [], ['tv']), space('c', [], ['tv'], { archived: true }), space('d')] })
    expect(spacesFor(current, 'tv').map(item => item.id)).toEqual(['a', 'b'])
    expect(inSpace(current.profiles[3], 'tv')).toBe(false)
  })

  it('offers Desktop with Desktop Access, or when the device has no Space at all', () => {
    const current = state({ desktop_clients: ['tv'], profiles: [space('a', [], ['tv', 'rp6'])] })
    expect(placesFor(current, 'tv')).toEqual(['desktop', 'a'])
    expect(placesFor(current, 'rp6')).toEqual(['a'])
    expect(offersDesktop(current, 'new')).toBe(true)
    expect(placesFor(current, 'new')).toEqual(['desktop'])
  })

  it('opens first in the host order: a Desktop default, the Default Space, the first Space, then Desktop', () => {
    const spaces = [space('a', [], ['tv']), space('b', ['tv'])]
    expect(opensFirst(state({ desktop_clients: ['tv'], desktop_default_clients: ['tv'], profiles: spaces }), 'tv')).toBe('desktop')
    // A Desktop default without Desktop Access is not one.
    expect(opensFirst(state({ desktop_default_clients: ['tv'], profiles: spaces }), 'tv')).toBe('b')
    expect(opensFirst(state({ profiles: [space('a', [], ['tv'])] }), 'tv')).toBe('a')
    expect(opensFirst(state(), 'tv')).toBe('desktop')
  })

  it('lists devices that can be given a Space, and any a Space still lists after they lost that', () => {
    const clients = [{ uuid: 'tv', perm: launch }, { uuid: 'former', perm: 0 }, { uuid: 'stranger', perm: 0 },
      { uuid: 'guest', perm: launch, temporary_authorization: true }]
    expect(canLaunch(clients[3])).toBe(false)
    expect(listedDevices(state({ profiles: [space('a', ['former'])] }), clients).map(client => client.uuid)).toEqual(['tv', 'former'])
  })
})

describe('the snapshot the table trusts', () => {
  const base = state()
  it('allows shared access across Spaces but rejects duplicate or archived grants', () => {
    const alex = { id: 'a', name: 'Alex', clients: ['default'], access_clients: [] }
    const shared = { ...base, profiles: [alex, { ...alex, id: 'b', clients: [], access_clients: ['default'] }] }
    expect(validSnapshot(shared)).toBe(true)
    expect(validSnapshot({ ...shared, profiles: [{ ...alex, access_clients: ['rp6', 'rp6'] }] })).toBe(false)
    expect(validSnapshot({ ...shared, profiles: [{ ...alex, archived: true, clients: [], access_clients: ['rp6'] }] })).toBe(false)
  })

  it('rejects ambiguous Desktop grants', () => {
    expect(validSnapshot({ ...base, desktop_clients: ['rp6'] })).toBe(true)
    for (const desktop_clients of [['rp6', 'rp6'], [''], 'rp6', [true]])
      expect(validSnapshot({ ...base, desktop_clients })).toBe(false)
  })

  it('takes the Desktop setting as a boolean or not at all', () => {
    expect(validSnapshot({ ...base, desktop_by_default: true })).toBe(true)
    expect(validSnapshot({ ...base, desktop_by_default: 'yes' })).toBe(false)
  })
})
