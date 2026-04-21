let cachedConfig = null
let inflightConfig = null

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
    .then(async (response) => {
      if (!response.ok) {
        throw new Error(`Config request failed with status ${response.status}`)
      }

      const data = await response.json()
      cachedConfig = data
      return data
    })
    .finally(() => {
      inflightConfig = null
    })

  return inflightConfig
}
