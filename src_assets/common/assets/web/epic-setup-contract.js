// Used only by the lazy Epic setup surface. Unknown hosts/facts confer no readiness.
import { EPIC_SETUP_STATUS, EPIC_SETUP_TITLE, EPIC_SETUP_SCAN, EPIC_SETUP_SCAN_ENVELOPE, EPIC_SETUP_ERROR } from './generated/polarisV1EpicSetupContract'

export const setupEnums = {
  desktop: ['not_checked', 'open', 'closed', 'unknown'],
  proof: ['required', 'valid', 'changed', 'unavailable'],
  receipt: ['required', 'complete', 'previous', 'unknown'],
  runtime: ['not_checked', 'not_selected', 'absent', 'busy', 'unknown'],
  prerequisites: ['available', 'unavailable', 'unknown'],
  state: ['busy', 'needs_action', 'launcher_closed', 'ready', 'unknown'],
  scan: ['required', 'checking', 'complete', 'metadata_unavailable', 'launcher_unobserved', 'launcher_changed', 'superseded', 'session_busy', 'unsupported'],
  capture: ['captured', 'unchanged', 'unavailable', 'superseded', 'unsupported'],
  reason: ['check_unresolved', 'session_busy', 'check_in_progress', 'choose_game', 'ready', 'epic_open_on_desktop',
    'launcher_present_unplaced', 'epic_launcher_proof_required', 'private_mode_unavailable', 'epic_game_already_running',
    'setup_generation_changed', 'epic_installation_unavailable', 'epic_installation_changed', 'windows_setup_unknown', 'launcher_changed', 'unsupported'],
}
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value)
const revision = value => typeof value === 'string' && /^(0|[1-9][0-9]{0,19})$/.test(value)
const token = value => typeof value === 'string' && /^[A-Za-z0-9_-]{1,256}$/.test(value)

export function readEpicTitle(wire) {
  if (!object(wire)) return null
  const title = { namespace_id: wire[EPIC_SETUP_TITLE.NAMESPACE_ID], item_id: wire[EPIC_SETUP_TITLE.ITEM_ID], artifact_id: wire[EPIC_SETUP_TITLE.ARTIFACT_ID] }
  return Object.values(title).every(token) ? title : null
}

export function readEpicSetup(wire) {
  if (!object(wire)) return null
  const facts = {
    version: wire[EPIC_SETUP_STATUS.VERSION], supported: wire[EPIC_SETUP_STATUS.SUPPORTED], epoch: wire[EPIC_SETUP_STATUS.EPOCH],
    revision: wire[EPIC_SETUP_STATUS.REVISION], sample_age_ms: wire[EPIC_SETUP_STATUS.SAMPLE_AGE_MS],
    desktop: wire[EPIC_SETUP_STATUS.DESKTOP], proof: wire[EPIC_SETUP_STATUS.PROOF], receipt: wire[EPIC_SETUP_STATUS.RECEIPT],
    receipt_revision: wire[EPIC_SETUP_STATUS.RECEIPT_REVISION], last_scan: wire[EPIC_SETUP_STATUS.LAST_SCAN],
    metadata_available: wire[EPIC_SETUP_STATUS.METADATA_AVAILABLE], runtime: wire[EPIC_SETUP_STATUS.RUNTIME],
    prerequisites: wire[EPIC_SETUP_STATUS.PREREQUISITES], state: wire[EPIC_SETUP_STATUS.STATE], reason: wire[EPIC_SETUP_STATUS.REASON],
    selected: wire[EPIC_SETUP_STATUS.SELECTED],
  }
  if (facts.version !== 1 || typeof facts.supported !== 'boolean' || typeof facts.metadata_available !== 'boolean' ||
      typeof facts.epoch !== 'string' || !/^[A-Za-z0-9-]{0,64}$/.test(facts.epoch) ||
      !revision(facts.revision) || !revision(facts.receipt_revision) ||
      !Number.isSafeInteger(facts.sample_age_ms) || facts.sample_age_ms < 0 ||
      !setupEnums.scan.includes(facts.last_scan) ||
      !['desktop', 'proof', 'receipt', 'runtime', 'prerequisites', 'state', 'reason'].every(key => setupEnums[key].includes(facts[key]))) return null
  if (facts.selected !== null && !(facts.selected = readEpicTitle(facts.selected))) return null
  if (['complete', 'previous'].includes(facts.receipt) && (!facts.epoch || !facts.supported || facts.proof !== 'valid' || !facts.metadata_available || facts.receipt_revision === '0')) return null
  if (facts.state === 'ready' && (!facts.epoch || !facts.supported || !facts.selected || facts.desktop !== 'closed' ||
      facts.proof !== 'valid' || !facts.metadata_available || facts.runtime !== 'absent' || facts.prerequisites !== 'available' || facts.reason !== 'ready')) return null
  if (facts.state === 'launcher_closed' && (!facts.epoch || !facts.supported || facts.desktop !== 'closed' || facts.proof !== 'valid')) return null
  return facts
}

export function readEpicScan(wire) {
  if (!object(wire)) return null
  const scan = { version: wire[EPIC_SETUP_SCAN.VERSION], metadata_available: wire[EPIC_SETUP_SCAN.METADATA_AVAILABLE],
    capture: wire[EPIC_SETUP_SCAN.CAPTURE], scan: wire[EPIC_SETUP_SCAN.SCAN], receipt_revision: wire[EPIC_SETUP_SCAN.RECEIPT_REVISION] }
  return scan.version === 1 && typeof scan.metadata_available === 'boolean' && setupEnums.capture.includes(scan.capture) &&
    setupEnums.scan.includes(scan.scan) && revision(scan.receipt_revision) ? scan : null
}

export function readEpicScanEnvelope(wire) {
  return object(wire) ? readEpicScan(wire[EPIC_SETUP_SCAN_ENVELOPE.EPIC_SETUP]) : null
}

export function readEpicSetupError(wire) {
  return object(wire) && wire[EPIC_SETUP_ERROR.STATUS] === false &&
    ['authentication_required', 'authentication_unavailable', 'invalid_selection', 'status_unavailable'].includes(wire[EPIC_SETUP_ERROR.ERROR]) ? wire[EPIC_SETUP_ERROR.ERROR] : null
}
