function normalize(value) {
  return String(value || '').replaceAll('_', ' ').toLowerCase()
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
    game.cmd,
    game.game_category,
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
