import { reportSettingsReadable, reportSettingsUnreadable } from './settings-unreadable.js'

let cachedConfig = null
let inflightConfig = null

// GET ./api/config answers 503 with this code when the host is up and the
// request was authenticated, but the settings store refused polaris.conf
// (#782). The body names the file, the reason and the fix, never its contents.
export const SETTINGS_UNREADABLE = 'config_unreadable'

export class SettingsUnreadableError extends Error {
  constructor(refusal) {
    super(`Polaris cannot read its settings file${refusal.path ? ` ${refusal.path}` : ''}`)
    this.name = 'SettingsUnreadableError'
    this.refusal = refusal
  }
}

function isJsonReply(response) {
  const headers = response?.headers
  if (!headers || typeof headers.get !== 'function') return true
  const mediaType = (headers.get('content-type') || '').split(';', 1)[0].trim().toLowerCase()
  return mediaType === 'application/json'
}

function replyFailed(response) {
  if (typeof response?.ok === 'boolean') return !response.ok
  const status = Number(response?.status)
  return Number.isFinite(status) && (status < 200 || status >= 300)
}

function text(value) {
  return typeof value === 'string' ? value : ''
}

// The refusal in a settings reply body the caller has already read, or null.
// A Live Tuning reply carries the same fields beside its own code.
export function settingsRefusalFromBody(status, body) {
  if (Number(status) !== 503 || !body || typeof body !== 'object' || body.error !== SETTINGS_UNREADABLE) return null
  return { path: text(body.path), reason: text(body.reason), fix: text(body.fix) }
}

// The refusal a settings reply carries, or null when it is some other answer.
export async function readSettingsRefusal(response) {
  if (Number(response?.status) !== 503 || typeof response?.json !== 'function' || !isJsonReply(response)) {
    return null
  }
  let body
  try {
    body = await response.json()
  } catch {
    return null
  }
  return settingsRefusalFromBody(response.status, body)
}

// The refusal a save reply carries, reported to the app banner. It reads a
// clone when the reply has one, so the caller can still read the reply itself.
export async function readSettingsSaveRefusal(response) {
  const refusal = await readSettingsRefusal(typeof response?.clone === 'function' ? response.clone() : response)
  if (refusal) reportSettingsUnreadable(refusal)
  return refusal
}

async function rejectSettingsReply(response) {
  const refusal = await readSettingsRefusal(response)
  if (refusal) {
    reportSettingsUnreadable(refusal)
    throw new SettingsUnreadableError(refusal)
  }
  throw new Error(`Config request failed with status ${response?.status}`)
}

// The settings in a GET ./api/config reply. A refused settings file rejects
// with SettingsUnreadableError; any other failure with a plain Error.
export function readConfigResponse(response) {
  // Return json() itself on success, so a caller's next step runs exactly as
  // soon as it did when it called json() directly.
  if (!replyFailed(response)) {
    reportSettingsReadable()
    return response.json()
  }
  return rejectSettingsReply(response)
}

// The settings in a GET ./api/config reply, or null when the host did not send
// them, for the views that carry on without settings. A refused file still
// reaches the app banner, and any other failure reads as missing data, as it
// did before (#782).
export function readConfigOrNull(response) {
  if (!replyFailed(response)) return readConfigResponse(response)
  return readConfigResponse(response).catch(() => null)
}

export function clearCachedConfig() {
  cachedConfig = null
  inflightConfig = null
}

export function primeCachedConfig(config) {
  if (!config || typeof config !== 'object') return
  cachedConfig = config
}

export async function getCachedConfig() {
  if (cachedConfig) return cachedConfig
  if (inflightConfig) return inflightConfig

  inflightConfig = fetch('./api/config', { credentials: 'include' })
    .then(readConfigResponse)
    .then((data) => {
      cachedConfig = data
      return data
    })
    .finally(() => {
      inflightConfig = null
    })

  return inflightConfig
}
