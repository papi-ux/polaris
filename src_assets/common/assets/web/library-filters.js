const FILTERS = new Set(['all', 'steam', 'manual', 'fast_action', 'running'])

function normalize(value = '') {
  return String(value).toLowerCase().trim()
}

function searchableText(app) {
  return [
    app.name,
    app.source,
    app['game-category'],
    app.cmd,
  ].map(normalize).join(' ')
}

function matchesFilter(app, filter, currentApp) {
  switch (filter) {
    case 'steam':
      return app.source === 'steam'
    case 'manual':
      return !app.source || app.source === 'manual'
    case 'fast_action':
      return app['game-category'] === 'fast_action'
    case 'running':
      return Boolean(currentApp) && app.uuid === currentApp
    case 'all':
    default:
      return true
  }
}

export function filterLibraryApps(apps = [], { query = '', filter = 'all', currentApp = '' } = {}) {
  const safeFilter = FILTERS.has(filter) ? filter : 'all'
  const safeQuery = normalize(query)

  return apps
    .filter((app) => app?.uuid)
    .filter((app) => matchesFilter(app, safeFilter, currentApp))
    .filter((app) => !safeQuery || searchableText(app).includes(safeQuery))
}
