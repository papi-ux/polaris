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

  it('still refuses a mismatch target with no driver version', () => {
    const wrong = structuredClone(shapes.mismatch)
    wrong.runtime_move.nvidia_driver = ''
    expect(validSnapshot(snapshot(wrong))).toBe(false)
  })
})
