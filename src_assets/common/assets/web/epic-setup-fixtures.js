// Test data only; never imported by the feature.
export const epicTitle = { namespace_id: 'ns', item_id: 'item', artifact_id: 'game' }
export const epicSample = overrides => ({ version: 1, supported: true, epoch: 'epoch-a', revision: '4', sample_age_ms: 0,
  desktop: 'closed', proof: 'valid', receipt: 'complete', receipt_revision: '2', last_scan: 'complete',
  metadata_available: true, runtime: 'not_selected', prerequisites: 'unknown', state: 'launcher_closed', reason: 'choose_game',
  selected: null, ...overrides })
export const epicReply = (body = epicSample(), status = 200) => ({ ok: status >= 200 && status < 300, status, json: async () => body })
export const deferredEpic = () => { let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no }); return { promise, resolve, reject } }
