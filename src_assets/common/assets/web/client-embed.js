// A client app that opens the console inside itself says so on the first load: Nova's Host
// Console adds ?client=nova&theme=<theme id> (Nova #310). The console keeps that for the visit, in
// session storage, so signing in or moving between pages keeps it, and never as a saved setting.
//
// The query is trusted for presentation only: the theme shown for the visit, the guide links
// hidden in an app that cannot open them in place, and where Play sends a title (the app's launch
// link, which streams it to the device the app runs on, instead of starting it on the host).
// public/assets/theme-boot.js reads the same query and storage key before first paint.

export const EMBED_STORAGE_KEY = 'polaris-embed'
const CLIENTS = ['nova']

let current = null

function sessionStore() {
  try {
    return window.sessionStorage
  } catch {
    return null
  }
}

function kept(storage) {
  try {
    return JSON.parse(storage?.getItem(EMBED_STORAGE_KEY) || 'null')
  } catch {
    return null
  }
}

function keep(storage, state) {
  try {
    storage?.setItem(EMBED_STORAGE_KEY, JSON.stringify(state))
  } catch {
    // Without session storage the visit keeps it in memory only.
  }
}

/**
 * Reads what the app that opened the console said, from the first load's query or, after that,
 * from the visit's session storage. `themes` is the list of known theme ids; any other is ignored.
 */
export function initClientEmbed({ search = window.location.search, storage = sessionStore(), themes = [] } = {}) {
  const params = new URLSearchParams(search || '')
  const known = theme => (themes.includes(theme) ? theme : '')
  if (CLIENTS.includes(params.get('client'))) {
    current = { client: params.get('client'), theme: known(params.get('theme')) }
    keep(storage, current)
  } else {
    const visit = kept(storage)
    current = visit && CLIENTS.includes(visit.client) ? { client: visit.client, theme: known(visit.theme) } : null
  }
  return current
}

/** The app the console was opened in ('nova'), or '' in a browser. */
export const embeddedClient = () => current?.client || ''

/** The theme the app asked for this visit, or ''. */
export const embeddedTheme = () => current?.theme || ''

/** A theme picked in the console is the one shown from then on, in this visit too. */
export function forgetEmbeddedTheme(storage = sessionStore()) {
  if (!current?.theme) return
  current = { ...current, theme: '' }
  keep(storage, current)
}

/**
 * Nova's launch link for one of this host's apps, as the Library's "Launch on this device" opens:
 * Nova starts the stream on the device it runs on. Every value is encoded, so a name with & or #
 * cannot cut the link short.
 */
export function artLaunchHref({ hostUuid, hostName, app }) {
  const part = (name, value) => `${name}=${encodeURIComponent(value ?? '')}`
  return `art://launch?${[
    part('host_uuid', hostUuid), part('host_name', hostName), part('app_uuid', app?.uuid), part('app_name', app?.name),
  ].join('&')}`
}

/** Opens a client app's link; the app takes it before the page goes anywhere. */
export function followClientLink(href) {
  window.location.assign(href)
}
