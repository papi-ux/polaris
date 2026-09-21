import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { validSnapshot } from './spaces-access.js'

// The host writes these fields; this console refuses a snapshot it cannot
// verify and says only "Could not verify Spaces". Both sides read this file, so
// a shape one accepts and the other rejects fails here instead of on a page.
const here = dirname(fileURLToPath(import.meta.url))
const shapes = JSON.parse(readFileSync(join(here, '../../../../tests/fixtures/spaces-runtime-upgrade.json'), 'utf8'))

const snapshot = runtime => ({
  schema: 1, enabled: true, available: true, changing: false, failed: false,
  creation_available: true, management_available: true, access_available: true, removal_available: true,
  capacity: { concurrent_limit: 1, concurrent_active: 0 },
  desktop_clients: [], desktop_default_clients: [], activity: [],
  profiles: [{
    id: '15ab1141-72db-4e28-a138-463a0dd1d98a', name: 'papi - steam', clients: [], access_clients: [],
    steam: true, archived: false, ...runtime,
  }],
})

describe('the runtime shape a host sends', () => {
  it('accepts a move offered as an upgrade, which names no driver of its own', () => {
    expect(validSnapshot(snapshot(shapes.upgrade))).toBe(true)
  })

  it('still accepts a move offered to repair a mismatch', () => {
    expect(validSnapshot(snapshot(shapes.mismatch))).toBe(true)
  })

  it('still refuses a move that claims neither a mismatch nor an upgrade', () => {
    const invented = structuredClone(shapes.upgrade)
    invented.runtime_move.reason = 'because_i_said_so'
    expect(validSnapshot(snapshot(invented))).toBe(false)
  })

  it('still refuses an upgrade target that claims a driver version', () => {
    const wrong = structuredClone(shapes.upgrade)
    wrong.runtime_move.nvidia_driver = '615.71.09'
    expect(validSnapshot(snapshot(wrong))).toBe(false)
  })

  it('accepts the launcher family a host names, and refuses one it does not run', () => {
    // The host sends the family every Space belongs to. A console that refuses
    // an unknown one shows "Could not verify Spaces" for the whole page, so the
    // families it knows are stated here rather than discovered on a card.
    for (const family of ['steam', 'heroic', 'lutris', '']) {
      expect(validSnapshot(snapshot({ ...shapes.upgrade, family }))).toBe(true)
    }
    for (const family of ['gamescope', 'Steam', 'epic', 3, null]) {
      expect(validSnapshot(snapshot({ ...shapes.upgrade, family }))).toBe(false)
    }
  })

  it('accepts a repair whose target borrows this PC driver and names no version', () => {
    // This used to be refused: a repair had to name a driver. But the reason
    // says why a Space moves and the target says what it moves to, and once the
    // borrowing runtime is published it is the target of every repair. Refusing
    // it would have failed the whole page for every mismatched Space.
    expect(validSnapshot(snapshot(shapes.mismatch_to_host))).toBe(true)
  })

  it('accepts a move onto a newer build of the runtime a Space already uses', () => {
    expect(validSnapshot(snapshot(shapes.updated))).toBe(true)
  })

  it('still ties the reason to the mismatch it claims', () => {
    // Only a repair goes with a mismatch, and a mismatch is only ever repaired.
    const repairWithoutMismatch = structuredClone(shapes.mismatch)
    repairWithoutMismatch.runtime_mismatch = false
    expect(validSnapshot(snapshot(repairWithoutMismatch))).toBe(false)
    const updateWithMismatch = structuredClone(shapes.updated)
    updateWithMismatch.runtime_mismatch = true
    updateWithMismatch.runtime_driver = '610.57.04'
    expect(validSnapshot(snapshot(updateWithMismatch))).toBe(false)
    // And a target's driver, when it names one, is still a driver version.
    const nonsense = structuredClone(shapes.mismatch)
    nonsense.runtime_move.nvidia_driver = 'latest'
    expect(validSnapshot(snapshot(nonsense))).toBe(false)
  })
})
