function normalize(value) {
  return String(value || '').replaceAll('_', ' ').toLowerCase()
}

const PLATFORM_NAMES = { windows: 'Windows', linux: 'Linux', mac: 'macOS' }
const RUNTIME_NAMES = { proton: 'Proton', wine: 'Wine', native: 'Native' }

function properName(value, names) {
  const key = String(value || '').toLowerCase()
  return names[key] || (key ? key[0].toUpperCase() + key.slice(1) : '')
}

// Who launches an official Epic title. A title both can launch is one row, so this tells them apart.
export function epicProviderLabel(provider = '') {
  return { official: 'Epic', heroic: 'Heroic' }[provider] || ''
}

// What an Epic title runs as, in one chip: a native title has only its platform to say.
export function epicPlatformChip({ installed_platform: platform, runtime } = {}) {
  const platformName = properName(platform, PLATFORM_NAMES)
  const runtimeName = properName(runtime, RUNTIME_NAMES)
  if (!platformName) return runtimeName
  return runtimeName && runtimeName !== RUNTIME_NAMES.native ? `${platformName} via ${runtimeName}` : platformName
}

function matchesImportStatus(game, status) {
  if (status === 'all') return true
  if (status === 'selected') return Boolean(game.selected) && !game.already_imported
  if (status === 'imported') return Boolean(game.already_imported)
  return !game.already_imported
}

function importSearchText(game) {
  return [
    game.name,
    game.source,
    game.appid,
    game.slug,
    game.runner,
    game.emulator,
    game.emulator_label,
    game.platform,
    game.rom_path,
    game.cmd,
    game.game_category,
    // Official Epic entries: searchable by provider, platform, runtime and exact triple.
    game.provider,
    game.installed_platform,
    game.runtime,
    game.namespace,
    game.item_id,
    game.artifact_id,
    ...(Array.isArray(game.genres) ? game.genres : []),
  ].map(normalize).join(' ')
}

export function summarizeImportGames(games = []) {
  const normalized = games.filter(Boolean)

  return {
    total: normalized.length,
    available: normalized.filter((game) => !game.already_imported).length,
    selected: normalized.filter((game) => game.selected && !game.already_imported).length,
    imported: normalized.filter((game) => game.already_imported).length,
  }
}

export function filterImportGames(games = [], { query = '', status = 'new' } = {}) {
  const terms = normalize(query).split(/\s+/).filter(Boolean)

  return games
    .filter(Boolean)
    .filter((game) => matchesImportStatus(game, status))
    .filter((game) => {
      if (!terms.length) return true
      const searchText = importSearchText(game)
      return terms.every((term) => searchText.includes(term))
    })
}
