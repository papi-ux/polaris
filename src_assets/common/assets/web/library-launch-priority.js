const FAVORITE_KEYS = ['favorite', 'favourite', 'is_favorite', 'is-favorite', 'starred', 'pinned']
const RECENT_KEYS = ['last-launched', 'last_launched', 'lastLaunch', 'lastLaunchTime']

function normalizeText(value = '') {
  return String(value || '').trim()
}

function truthyFlag(value) {
  if (value === true || value === 1) return true
  if (typeof value === 'string') return ['1', 'true', 'yes', 'favorite', 'favourite', 'starred', 'pinned'].includes(value.toLowerCase())
  return false
}

function numericTimestamp(value) {
  if (value instanceof Date) return value.getTime()
  const parsed = Number(value)
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 0
}

export function isFavoriteApp(app = {}) {
  return FAVORITE_KEYS.some((key) => truthyFlag(app?.[key]))
}

export function appLastLaunched(app = {}) {
  return RECENT_KEYS.reduce((latest, key) => Math.max(latest, numericTimestamp(app?.[key])), 0)
}

export function isLaunchReadyApp(app = {}) {
  if (!app?.uuid) return false
  if (normalizeText(app.cmd)) return true
  if (Array.isArray(app.detached) && app.detached.some((command) => normalizeText(command))) return true
  if (Array.isArray(app['prep-cmd']) && app['prep-cmd'].length > 0) return true
  if (Array.isArray(app['state-cmd']) && app['state-cmd'].length > 0) return true
  return normalizeText(app.name).toLowerCase() === 'desktop'
}

export function launchPriorityDetails(app = {}, { currentApp = '', index = 0 } = {}) {
  const favorite = isFavoriteApp(app)
  const lastLaunched = appLastLaunched(app)
  const recent = lastLaunched > 0
  const launchReady = isLaunchReadyApp(app)
  const running = Boolean(currentApp) && app?.uuid === currentApp
  const broken = Boolean(app?.uuid) && !launchReady
  const score =
    (running ? 9000 : 0) +
    (launchReady ? 5000 : -5000) +
    (favorite ? 3000 : 0) +
    (recent ? 2500 : 0) +
    Math.min(lastLaunched / 1000000000, 999) -
    index / 1000

  return { favorite, recent, launchReady, running, broken, lastLaunched, score }
}

export function sortAppsByLaunchPriority(apps = [], options = {}) {
  return apps
    .filter((app) => app?.uuid)
    .map((app, index) => ({ app, index, details: launchPriorityDetails(app, { ...options, index }) }))
    .sort((a, b) => {
      if (b.details.score !== a.details.score) return b.details.score - a.details.score
      return a.index - b.index
    })
    .map(({ app }) => app)
}

export function quickLaunchApps(apps = [], { limit = 6, currentApp = '' } = {}) {
  return sortAppsByLaunchPriority(apps, { currentApp }).slice(0, limit)
}
