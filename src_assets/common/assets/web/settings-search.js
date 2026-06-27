function normalize(value) {
  return String(value || '')
    .toLowerCase()
    .replace(/[_-]+/g, ' ')
    .replace(/[^a-z0-9\s]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
}

function termsFor(query) {
  return normalize(query).split(' ').filter(Boolean)
}

function scoreText(text, query, terms, weight) {
  const normalized = normalize(text)
  if (!normalized || (!query && terms.length === 0)) return 0
  if (normalized === query) return weight * 10
  if (normalized.startsWith(query)) return weight * 8
  if (normalized.includes(query)) return weight * 6
  if (terms.length && terms.every(term => normalized.includes(term))) return weight * 4
  if (terms.length && terms.some(term => normalized.includes(term))) return weight
  return 0
}

function unique(values) {
  return [...new Set(values.filter(Boolean))]
}

export function normalizeSettingsSearchQuery(query) {
  return normalize(query)
}

export function scoreSettingsOption({ key, label = '', description = '', aliases = [] } = {}, query) {
  const normalizedQuery = normalize(query)
  const terms = termsFor(query)
  if (!normalizedQuery) return 0

  return Math.max(
    scoreText(label, normalizedQuery, terms, 12),
    scoreText(key, normalizedQuery, terms, 10),
    scoreText(aliases.join(' '), normalizedQuery, terms, 8),
    scoreText(description, normalizedQuery, terms, 5),
  )
}

export function scoreSettingsTab(tab = {}, query) {
  const normalizedQuery = normalize(query)
  const terms = termsFor(query)
  if (!normalizedQuery) return 0

  return Math.max(
    scoreText(tab.name, normalizedQuery, terms, 9),
    scoreText(tab.groupLabel, normalizedQuery, terms, 5),
    scoreText(tab.summary, normalizedQuery, terms, 4),
    scoreText((tab.searchAliases || []).join(' '), normalizedQuery, terms, 7),
  )
}

export function rankSettingsSearchTabs(tabs = [], query, optionFactory = () => ({})) {
  const normalizedQuery = normalize(query)
  if (!normalizedQuery) {
    return tabs.map((tab, index) => ({
      tab,
      score: 1,
      index,
      matchedOptionKeys: [],
      firstOptionKey: null,
    }))
  }

  return tabs
    .map((tab, index) => {
      const options = Object.keys(tab.options || {})
        .map((key, optionIndex) => {
          const option = { key, ...optionFactory(key, tab) }
          return {
            key,
            optionIndex,
            score: scoreSettingsOption(option, normalizedQuery),
          }
        })
        .filter(option => option.score > 0)
        .sort((a, b) => b.score - a.score || a.optionIndex - b.optionIndex)

      const tabScore = scoreSettingsTab(tab, normalizedQuery)
      const bestOptionScore = options[0]?.score || 0
      const score = Math.max(tabScore, bestOptionScore)
      if (score <= 0) return null

      return {
        tab,
        score,
        index,
        matchedOptionKeys: unique(options.map(option => option.key)),
        firstOptionKey: options[0]?.key || Object.keys(tab.options || {})[0] || null,
      }
    })
    .filter(Boolean)
    .sort((a, b) => b.score - a.score || a.index - b.index)
}
