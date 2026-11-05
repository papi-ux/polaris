// Pure presentation state. Only validated host samples can establish a receipt.
export const emptyEpicSetup = () => ({ facts: null, history: null, phase: 'idle', notice: '', scan: null })

export function reduceEpicSetup(state, event) {
  switch (event.type) {
    case 'clear': return { ...emptyEpicSetup(), phase: event.phase || 'checking' }
    case 'invalidate': return { ...state, facts: null, phase: 'checking' }
    case 'scan': return { ...state, facts: null, phase: 'checking', scan: event.scan }
    case 'sample': {
      const facts = event.facts
      const history = ['complete', 'previous'].includes(facts.receipt)
        ? { epoch: facts.epoch, revision: facts.receipt_revision } : null
      return { facts, history, phase: 'current', scan: facts.last_scan === 'complete' ? null : state.scan,
        notice: event.restarted ? 'restart' : facts.proof === 'changed' ? 'update'
          : facts.receipt === 'complete' && facts.last_scan === 'complete' ? '' : state.notice }
    }
    case 'error': return { ...state, facts: null, phase: event.phase,
      history: event.phase === 'auth' ? null : state.history }
    default: return state
  }
}

const transportCopy = {
  idle: 'Open this checklist to read setup status.',
  checking: 'Checking setup on this PC.',
  paused: 'Setup checks are paused. Return here for a fresh check.',
  stale: 'The last setup check expired. Check again for current status.',
  timeout: 'The setup check timed out. Check the connection and retry.',
  offline: "Can't read setup status. Check the connection and retry.",
  auth: "Sign in to this PC's Polaris console to check setup.",
  auth_store: "Polaris can't read its sign-in state. Reconnect to the console and retry.",
  unsupported: "This host doesn't provide live Epic setup status. Follow the setup steps below.",
  malformed: "This host returned setup status this console can't check. Check the host and console versions.",
  failed: "Polaris couldn't read setup status. Retry when the host is available.",
  superseded: 'Setup changed while it was being checked. A fresh check is needed.',
}

export function epicSetupPresentation(state) {
  const f = state.facts
  const row = (id, title, status, message) => ({ id, title, status, message })
  const checking = state.phase === 'checking'
  const unknown = checking ? 'Checking' : "Can't check"
  const rows = [
    row('observed', 'Epic observed open on this PC', unknown, 'Open Epic on the host desktop before scanning.'),
    row('receipt', 'Scanned while Epic was open', unknown, state.history
      ? 'Previous successful scan (historical). Its current setup binding cannot be checked.'
      : 'Only a successful scan with Epic observed open completes this step.'),
    row('closed', 'Epic closed: check Private Stream setup', unknown, 'Polaris needs a fresh launcher and selected-game check.'),
  ]
  let message = transportCopy[state.phase] || ''
  if (f) {
    if (!f.supported) message = transportCopy.unsupported
    else if (f.reason === 'check_in_progress') {
      message = 'Another setup check is in progress. Check again shortly.'
      for (const item of rows) item.status = 'Checking'
    } else if (f.state === 'busy' || f.reason === 'session_busy') message = 'A stream is active or changing. Check setup when it is idle.'
    else {
      const history = ['complete', 'previous'].includes(f.receipt)
      rows[0].status = f.desktop === 'open' || history ? 'Complete' : f.desktop === 'unknown' ? "Can't check" : 'Needs action'
      rows[0].message = f.desktop === 'open' ? 'Epic is open on the host desktop. Select Rescan Sources.'
        : history ? 'Epic was observed during this setup; that observation is historical.'
          : 'Open Epic on this PC and select Rescan Sources.'
      rows[1].status = history ? 'Complete' : f.receipt === 'unknown' ? "Can't check" : 'Needs action'
      rows[1].message = history ? (f.receipt === 'previous' || (state.scan && state.scan.scan !== 'complete')
        ? 'Previous successful scan (historical). The latest rescan did not complete this step.'
        : 'Epic was observed at both ends of a successful metadata scan. This is setup history, not a live-open claim.')
        : 'Scan with Epic open. Provider choice or a Mirror Desktop launch does not complete this step.'
      rows[2].status = 'Needs action'
      rows[2].message = 'Quit Epic completely on the host desktop, then check again.'
      if (f.proof === 'changed' || f.reason === 'launcher_changed') {
        rows[2].message = 'Epic changed or updated. Open it on the host desktop and rescan, then quit it.'
      } else if (f.proof === 'required') {
        rows[2].message = 'Open Epic on the host desktop, scan, then quit it. Repeat after Polaris restarts.'
      } else if (f.reason === 'windows_setup_unknown') {
        rows[2].status = "Can't check"
        rows[2].message = "Polaris can't check this game's Windows setup folder. If this is your first play on this PC, play once in Mirror Desktop, then close the game and Epic and try again. If you've played before, finish any update and check that the folder can be read."
      } else if (['epic_installation_unavailable', 'epic_installation_changed'].includes(f.reason)) {
        rows[2].status = "Can't check"
        rows[2].message = "This game's installation is unavailable or changed. Finish its install or update in Epic, then rescan and select it again."
      } else if (f.runtime === 'busy' || f.reason === 'epic_game_already_running') {
        rows[2].message = 'This game or its exact runtime is still running. Close it yourself, then check again.'
      } else if (f.desktop === 'closed' && f.proof === 'valid') {
        if (!f.selected) rows[2].message = 'Epic is closed; choose a game to check its setup.'
        else if (f.state === 'ready' && f.runtime === 'absent' && f.prerequisites === 'available') {
          rows[2].status = 'Complete'
          rows[2].message = 'Epic is closed: ready for Private Stream at this sample.'
        } else {
          rows[2].status = "Can't check"
          rows[2].message = f.prerequisites === 'unavailable'
            ? 'Private Stream prerequisites are unavailable. Use Mirror Desktop or check the host configuration.'
            : f.runtime === 'absent'
              ? "Epic is closed. This game and its exact runtime were not found at this sample. This host can't confirm all Private Stream prerequisites for the selected game."
              : "Epic is closed. This host can't confirm all Private Stream prerequisites for the selected game."
        }
      } else if (f.desktop === 'unknown' || f.proof === 'unavailable' || f.reason === 'check_unresolved') {
        rows[2].status = "Can't check"
        rows[2].message = "Polaris couldn't complete the launcher check. Retry, or use Mirror Desktop."
      }
      message = rows[2].message
    }
  }
  const scanMessage = state.scan && state.scan.scan !== 'complete'
    ? ({ metadata_unavailable: 'The rescan could not read Epic metadata. No new setup receipt was created.',
      launcher_unobserved: 'The rescan did not observe the same desktop Epic launcher. No new setup receipt was created.',
      launcher_changed: 'Epic changed during the rescan. Open Epic and scan again.',
      session_busy: 'The rescan crossed an active or changing stream. Check again when idle.',
      unsupported: 'This host does not provide setup receipts.',
      superseded: 'Another setup change superseded the rescan. Check again.' }[state.scan.scan] || 'The rescan has no completed setup receipt.') : ''
  return { rows, message, scanMessage,
    notice: state.notice === 'restart' ? 'Polaris restarted or its setup epoch changed. Repeat setup on this PC.'
      : state.notice === 'update' ? 'Epic changed or updated. Repeat setup on this PC.' : '',
    summary: [f?.reason === 'windows_setup_unknown' ? "This game's Windows setup folder can't be checked." : message,
      ...rows.map(r => `${r.title}: ${r.status}`), scanMessage].filter(Boolean).join(' ') }
}
