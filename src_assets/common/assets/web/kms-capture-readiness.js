// What Host Now says about capture and DRM/KMS, from the kms_capture report /api/stats/system sends.
// The host decides the state from its own process and from the capture its stream mode asks for;
// this only words it. KMS readiness only matters where capture would use KMS: a host set to portal
// or kwin runs without CAP_SYS_ADMIN on purpose, and that reads as KMS not in use, never as broken.

export const KMS_ENABLE_COMMAND = 'sudo -H polaris --setup-host --enable-kms'
export const KMS_SETUP_COMMAND = 'sudo -H polaris --setup-host'
export const KMS_RESTART_COMMAND = 'systemctl --user daemon-reload && systemctl --user restart polaris'
export const KMS_START_SERVICE_COMMAND = 'systemctl --user start polaris'
export const CAPTURE_SETTING_ROUTE = '/config#capture'
export const LOGS_ROUTE = '/troubleshooting#logs'

// tone: ready, available, action, or off. A step toward KMS is only a warning where capture asks
// for KMS; on a host that searches, it is the next step for someone who installed the package.
const STATES = {
  ready: { tone: 'ready' },
  kms_found_nothing: { tone: 'action', link: 'logs' },
  automatic: { tone: 'available' },
  not_in_use: { tone: 'off' },
  mode_sets_kms_aside: { tone: 'off' },
  not_installed: { tone: 'setup', command: KMS_ENABLE_COMMAND },
  helper_broken: { tone: 'action' },
  no_capability: { tone: 'action' },
  not_enabled: { tone: 'setup', command: KMS_ENABLE_COMMAND },
  not_in_group: { tone: 'action', command: KMS_ENABLE_COMMAND },
  waiting_for_login: { tone: 'action', command: KMS_SETUP_COMMAND },
  finish_setup: { tone: 'action', command: KMS_SETUP_COMMAND },
  outside_service: { tone: 'action', command: KMS_START_SERVICE_COMMAND },
  restart_needed: { tone: 'action', command: KMS_RESTART_COMMAND },
}

export const KMS_STATES = Object.keys(STATES)

const TONE_CLASSES = {
  ready: 'system-telemetry-state-success',
  available: 'text-ice',
  action: 'text-warning-bright',
  off: 'system-telemetry-state-muted',
}

const STATUS_KEYS = {
  ready: 'index.kms_status_ready',
  available: 'index.kms_status_available',
  action: 'index.kms_status_action',
}

// Backends as polaris.conf and the stream stats name them. kwin and drm are the portal and KMS
// under other names, and a label keeps the name the host was given.
const CAPTURE_LABELS = new Set(['kms', 'drm', 'portal', 'kwin', 'wlr', 'x11', 'nvfbc', 'cage'])
const CAPTURE_PHRASES = new Set(['kms', 'portal', 'wlr', 'x11', 'nvfbc'])
const ROUTE_LABELS = new Set(['portal_screencast', 'portal_gamescope_node', 'portal_kwin_node'])

function token(value) {
  return String(value || '').trim().toLowerCase()
}

/** The i18n key for a capture backend's name, or null for one this console does not know. */
export function captureLabelKey(capture) {
  const value = token(capture)
  if (!value || value === 'auto') return 'index.capture_label_auto'
  return CAPTURE_LABELS.has(value) ? `index.capture_label_${value}` : null
}

function captureLabel(t, capture) {
  const key = captureLabelKey(capture)
  return key ? t(key) : String(capture)
}

// How a sentence names a route: "captures through the portal", not "through Portal".
function capturePhrase(t, route) {
  const value = token(route)
  if (!value) return t('index.capture_phrase_auto')
  return CAPTURE_PHRASES.has(value) ? t(`index.capture_phrase_${value}`) : String(route)
}

function modeName(t, kms) {
  return String(kms.stream_mode_label || '').trim() || t('index.capture_mode_unknown')
}

function detailSentences(kms) {
  const state = kms.state
  const accountParam = { account: true }
  // An empty route under an explicit capture is the automatic search standing in for a backend
  // that found nothing, so the sentence names that backend rather than Autodetect.
  const searchStandsIn = Boolean(token(kms.capture)) && !token(kms.route)
  // After a package update the running helper reads "(deleted)", and it is still the helper.
  const runsHelper = kms.running_helper === true || kms.running_replaced_helper === true
  switch (state) {
    case 'ready':
      return [{ key: 'index.kms_state_ready_detail', params: { mode: true } }]
    case 'kms_found_nothing':
      return [{ key: 'index.kms_state_kms_found_nothing_detail', params: { substitute: true } }]
    case 'automatic':
      if (searchStandsIn) {
        return [
          { key: 'index.kms_search_stands_in', params: { capture: true, mode: true } },
          { key: 'index.kms_state_automatic_search_detail' },
        ]
      }
      return [{ key: 'index.kms_state_automatic_detail' }]
    case 'mode_sets_kms_aside':
      return [
        { key: 'index.kms_mode_sets_kms_aside_detail', params: { mode: true, route: true } },
        { key: 'index.kms_modes_that_use_kms' },
        { key: 'index.kms_mode_resets_capture' },
      ]
    case 'not_in_use': {
      const sentences = []
      if (searchStandsIn) {
        sentences.push({ key: 'index.kms_search_stands_in', params: { capture: true, mode: true } })
        if (kms.route_kind === 'automatic') {
          sentences.push({ key: 'index.kms_not_in_use_search_not_set_up' })
        } else {
          sentences.push({ key: 'index.kms_not_in_use_search_passes_over', params: { mode: true } })
        }
      } else if (kms.route_kind === 'automatic') {
        sentences.push({ key: 'index.kms_not_in_use_not_set_up' })
      } else if (token(kms.route)) {
        sentences.push({ key: 'index.kms_not_in_use_route', params: { mode: true, route: true } })
      } else {
        sentences.push({ key: 'index.kms_not_in_use_autodetect', params: { mode: true } })
      }
      // Only a host that runs the helper had a capability to give up; for any other host the
      // sentence would explain something that never happened.
      if (kms.capability_set_aside === true && runsHelper) {
        sentences.push({ key: 'index.kms_set_aside_note' })
      }
      if (kms.kms_possible_in_mode === false) {
        sentences.push({ key: 'index.kms_switch_mode', params: { mode: true } })
      } else if (runsHelper || kms.cap_sys_admin === true) {
        sentences.push({ key: 'index.kms_switch_restart' })
      } else if (kms.helper_installed === true) {
        sentences.push({ key: 'index.kms_switch_enable' })
      } else {
        sentences.push({ key: 'index.kms_switch_setup' })
      }
      // Choosing Mirror Desktop in Settings writes capture = portal, which is how most hosts that
      // set up the helper end up here; a KMS chosen before the mode does not survive it.
      sentences.push({ key: 'index.kms_mode_resets_capture' })
      return sentences
    }
    case 'waiting_for_login':
      return [{ key: kms.lingering === true ? 'index.kms_waiting_for_login_linger_detail' : 'index.kms_waiting_for_login_detail', params: accountParam }]
    case 'not_in_group':
    case 'finish_setup':
      return [{ key: `index.kms_state_${state}_detail`, params: accountParam }]
    default:
      return [{ key: `index.kms_state_${state}_detail` }]
  }
}

// What capture opened. The host names none while more than one client streams, or while the one
// stream has not opened capture yet, and neither is "none since Polaris started".
function observedItem(t, observed, streamsRunning) {
  if (observed) {
    return {
      labelKey: observed.when === 'streaming' ? 'index.capture_readout_streaming' : 'index.capture_readout_last',
      value: observedValue(t, observed),
    }
  }
  if (streamsRunning > 1) {
    return { labelKey: 'index.capture_readout_streaming', value: t('index.capture_readout_clients', { count: streamsRunning }) }
  }
  if (streamsRunning === 1) {
    return { labelKey: 'index.capture_readout_streaming', value: t('index.capture_readout_not_open') }
  }
  return { labelKey: 'index.capture_readout_last', value: t('index.capture_readout_none') }
}

function observedValue(t, observed) {
  const backend = captureLabel(t, observed.opened)
  const route = token(observed.route)
  const client = String(observed.client_name || '').trim()
  const params = { backend, client: client || t('index.capture_readout_unnamed_client') }
  if (ROUTE_LABELS.has(route)) {
    return t('index.capture_readout_observed_route', { ...params, route: t(`index.capture_route_${route}`) })
  }
  return t('index.capture_readout_observed', params)
}

/**
 * Everything the Capture row shows for one kms_capture report, worded with t, or null when the host
 * sent none: every host before this field, and every host that is not Linux.
 *
 * @param {object} kms The kms_capture object from /api/stats/system.
 * @param {(key: string, params?: object) => string} t The console's translate function.
 * @param {string} runningBinary running_binary.path from the same report.
 */
export function describeKmsCapture(kms, t, runningBinary = '') {
  if (!kms || typeof kms !== 'object') return null
  const spec = STATES[kms.state]
  if (!spec) return null
  const wanted = kms.route_kind === 'kms'
  const tone = spec.tone === 'setup' ? (wanted ? 'action' : 'off') : spec.tone
  let statusKey = STATUS_KEYS[tone]
  if (!statusKey) {
    statusKey = spec.tone === 'setup' ? 'index.kms_status_not_set_up' : 'index.kms_status_not_in_use'
  }

  const values = {
    mode: modeName(t, kms),
    route: capturePhrase(t, kms.route),
    substitute: capturePhrase(t, kms.substitute),
    capture: captureLabel(t, kms.capture_setting ?? kms.capture),
    account: String(kms.account || '').trim() || t('index.kms_this_account'),
  }
  const detail = detailSentences(kms)
    .map(({ key, params = {} }) => {
      const resolved = {}
      for (const [name, value] of Object.entries(params)) {
        resolved[name] = value === true ? values[name] : value
      }
      return t(key, resolved)
    })
    .join(' ')

  let link = null
  if (spec.link === 'logs') {
    link = { to: LOGS_ROUTE, labelKey: 'index.kms_open_logs' }
  } else if (kms.state === 'not_in_use' && kms.kms_possible_in_mode !== false) {
    link = { to: CAPTURE_SETTING_ROUTE, labelKey: 'index.kms_open_capture_setting' }
  }

  const observed = kms.observed && typeof kms.observed === 'object' && kms.observed.opened ? kms.observed : null
  const readout = [
    { labelKey: 'index.capture_readout_setting', value: values.capture },
    { labelKey: 'index.capture_readout_mode', value: t('index.capture_readout_mode_value', { mode: values.mode, route: captureLabel(t, kms.route) }) },
    observedItem(t, observed, Number(kms.streams_running) || 0),
  ].map(({ labelKey, value }) => ({ label: t(labelKey), value }))

  let capabilityKey = 'index.kms_fact_no_capability'
  if (kms.cap_sys_admin === true) {
    capabilityKey = 'index.kms_fact_capability'
  } else if (kms.capability_set_aside === true) {
    capabilityKey = 'index.kms_fact_set_aside'
  }

  return {
    state: kms.state,
    tone,
    toneClass: TONE_CLASSES[tone],
    statusKey,
    headlineKey: `index.kms_state_${kms.state}`,
    detail,
    command: spec.command || '',
    link,
    readout,
    binary: String(runningBinary || '').trim(),
    capability: t(capabilityKey),
  }
}
