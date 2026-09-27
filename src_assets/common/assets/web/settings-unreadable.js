import { readonly, ref } from 'vue'

// Whether the host refused its settings file (#782), as the last reply that
// could tell said it. Every reader of a settings reply reports here: the sign-in
// check, Reconnecting, a restart, the config cache, the views that read
// ./api/config themselves, and a save or a Live Tuning change the host turned
// away. The one banner in App.vue reads it, so a refused file is said on every
// page rather than only on Settings.
const refusal = ref(null)

export const settingsUnreadable = readonly(refusal)

function text(value) {
  return typeof value === 'string' ? value : ''
}

export function reportSettingsUnreadable(value) {
  if (!value || typeof value !== 'object') return
  refusal.value = { path: text(value.path), reason: text(value.reason), fix: text(value.fix) }
}

// A reply that carried the settings, or a save that went through: the host read
// the file, so it is not refused any more.
export function reportSettingsReadable() {
  refusal.value = null
}

// Signed out: what was known about the file belongs to the session that ended.
export function forgetSettingsRefusal() {
  refusal.value = null
}

// What a sign-in probe learned: the refusal it carried, or settings it read. A
// probe that learned neither, such as a host that is down, leaves the last answer.
export function reportSettingsProbe(result) {
  if (result?.settingsUnreadable) {
    reportSettingsUnreadable(result.settingsUnreadable)
  } else if (result?.config) {
    reportSettingsReadable()
  }
}

// Settings says a refused file in place of its form, so the app banner stays
// off that page and shows on every other one. @p routePath is normalized.
const SETTINGS_ROUTE = '/config'

export function showsSettingsBanner(value, routePath) {
  return Boolean(value) && routePath !== SETTINGS_ROUTE
}

// The words a save, a toggle or Live Tuning shows when the host turned it away
// for this reason: the reason and the fix, not a generic failure.
export function settingsRefusalSentence(lead, value) {
  return [lead, text(value?.reason), text(value?.fix)].filter(Boolean).join(' ')
}
