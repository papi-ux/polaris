import { describe, it, expect } from 'vitest'
import { readEpicSetup, readEpicTitle, readEpicScanEnvelope, readEpicSetupError, setupEnums } from './epic-setup-contract'

const sample = overrides => ({ version: 1, supported: true, epoch: 'opaque-epoch', revision: '4', sample_age_ms: 0,
  desktop: 'closed', proof: 'valid', receipt: 'complete', receipt_revision: '2', last_scan: 'complete',
  metadata_available: true, runtime: 'absent', prerequisites: 'available', state: 'ready', reason: 'ready',
  selected: { namespace_id: 'ns', item_id: 'item', artifact_id: 'game' }, ...overrides })

describe('lazy Epic setup contract reader', () => {
  it('accepts a selected sampled preflight without making the receipt an admission rule', () => {
    expect(readEpicSetup(sample())).toEqual(sample())
    expect(readEpicSetup(sample({ receipt: 'required', receipt_revision: '0' }))).not.toBeNull()
  })
  it.each(Object.keys(sample()))('requires the host field %s', key => {
    const wire = sample(); delete wire[key]
    expect(readEpicSetup(wire)).toBeNull()
  })
  it.each(['desktop', 'proof', 'receipt', 'runtime', 'prerequisites', 'state', 'reason'])('rejects unknown %s enums', key => {
    expect(readEpicSetup(sample({ [key]: 'future-unknown-value' }))).toBeNull()
    expect(setupEnums[key].length).toBeGreaterThan(0)
  })
  it.each([{ version: 2 }, { supported: 'yes' }, { metadata_available: 1 }, { epoch: '/private/path' },
    { sample_age_ms: -1 }, { sample_age_ms: NaN }, { sample_age_ms: '0' }, { revision: 1 }, { revision: '-1' },
    { receipt_revision: '00' }, { last_scan: 'future' }, { selected: {} }, { selected: { namespace_id: '/path', item_id: 'item', artifact_id: 'game' } }])('rejects malformed host facts %j', override => {
    expect(readEpicSetup(sample(override))).toBeNull()
  })
  it.each([{ supported: false }, { epoch: '' }, { desktop: 'open' }, { proof: 'required' }, { runtime: 'unknown' },
    { prerequisites: 'unknown' }, { selected: null }, { metadata_available: false }, { reason: 'choose_game' }])('never accepts optimistic ready with %j', override => {
    expect(readEpicSetup(sample({ receipt: 'required', ...override }))).toBeNull()
  })
  it('separates no title and unknown support from ready', () => {
    expect(readEpicSetup(sample({ state: 'launcher_closed', reason: 'choose_game', runtime: 'not_selected', selected: null, prerequisites: 'unknown' }))).not.toBeNull()
    expect(readEpicSetup(sample({ state: 'launcher_closed', desktop: 'unknown' }))).toBeNull()
  })
  it('accepts a busy or unsupported sample without inventing an epoch or receipt', () => {
    for (const supported of [true, false]) expect(readEpicSetup(sample({ supported, state: 'busy', reason: 'session_busy',
      desktop: 'not_checked', proof: 'required', receipt: 'required', receipt_revision: '0', epoch: '', selected: null }))).not.toBeNull()
  })
  it.each(['complete', 'previous'])('binds %s history to host proof, available metadata and receipt revision', receipt => {
    for (const override of [{ epoch: '' }, { proof: 'changed' }, { metadata_available: false }, { receipt_revision: '0' }])
      expect(readEpicSetup(sample({ state: 'unknown', receipt, ...override }))).toBeNull()
  })
  it('retains unavailable metadata separately from successful empty scan', () => {
    const scan = { version: 1, metadata_available: true, capture: 'captured', scan: 'complete', receipt_revision: '2' }
    expect(readEpicScanEnvelope({ epic_setup: scan })).toEqual(scan)
    const failed = { ...scan, metadata_available: false, scan: 'metadata_unavailable' }
    expect(readEpicScanEnvelope({ epic_setup: failed })).toEqual(failed)
    expect(readEpicScanEnvelope({})).toBeNull()
    expect(readEpicScanEnvelope({ epic_setup: { ...scan, capture: 'future' } })).toBeNull()
    expect(readEpicScanEnvelope({ epic_setup: { ...scan, scan: 'future' } })).toBeNull()
  })
  it('accepts only bounded exact identities and public failure codes', () => {
    expect(readEpicTitle(null)).toBeNull()
    expect(readEpicTitle({ namespace_id: 'n'.repeat(257), item_id: 'i', artifact_id: 'g' })).toBeNull()
    expect(readEpicSetupError({ status: false, error: 'authentication_unavailable' })).toBe('authentication_unavailable')
    expect(readEpicSetupError({ status: false, error: '/private/path pid=42' })).toBeNull()
    expect(readEpicSetupError({ status: true, error: 'status_unavailable' })).toBeNull()
    expect(readEpicSetup(null)).toBeNull()
    expect(readEpicSetup([])).toBeNull()
  })
})
