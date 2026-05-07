import { ref } from 'vue'

/**
 * Composable for scanning and importing Steam/Lutris/Heroic games.
 *
 * @returns Reactive scanning state and import functions.
 */
export function useGameScanner() {
  const scanning = ref(false)
  const importing = ref(false)
  const steamGames = ref([])
  const lutrisGames = ref([])
  const heroicGames = ref([])
  const error = ref(null)

  async function scan() {
    scanning.value = true
    error.value = null
    try {
      const res = await fetch('./api/games/scan', { credentials: 'include' })
      if (res.ok) {
        const data = await res.json()
        steamGames.value = (data.steam_games || []).map(g => ({
          ...g,
          selected: !g.already_imported
        }))
        lutrisGames.value = (data.lutris_games || []).map(g => ({
          ...g,
          selected: !g.already_imported
        }))
        heroicGames.value = (data.heroic_games || []).map(g => ({
          ...g,
          selected: !g.already_imported
        }))
      } else {
        error.value = 'Failed to scan for games'
      }
    } catch (e) {
      error.value = 'Scanner not available'
    } finally {
      scanning.value = false
    }
  }

  async function importSelected() {
    const allGames = [...steamGames.value, ...lutrisGames.value, ...heroicGames.value]
    const selected = allGames.filter(g => g.selected && !g.already_imported)
    if (selected.length === 0) return 0

    importing.value = true
    error.value = null
    try {
      const res = await fetch('./api/games/import', {
        credentials: 'include',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          games: selected.map(g => ({
            name: g.name,
            appid: g.appid || '',
            source: g.source,
            slug: g.slug || '',
            runner: g.runner || '',
            cmd: g.cmd || '',
            image_path: g.image_path || g['image-path'] || g.cover_path || '',
            game_category: g.game_category || '',
            genres: g.genres || []
          }))
        })
      })
      const data = await res.json()
      if (data.status) {
        selected.forEach(g => { g.already_imported = true; g.selected = false })
        return data.imported || 0
      } else {
        error.value = data.error || 'Import failed'
      }
    } catch (e) {
      error.value = 'Import request failed'
    } finally {
      importing.value = false
    }
    return 0
  }

  function toggleAll(val, source) {
    const lists = { steam: steamGames, lutris: lutrisGames, heroic: heroicGames }
    const target = source ? lists[source] : null
    const targets = target ? [target.value] : [steamGames.value, lutrisGames.value, heroicGames.value]
    targets.forEach(list => {
      list.forEach(g => {
        if (!g.already_imported) g.selected = val
      })
    })
  }

  return { scanning, importing, steamGames, lutrisGames, heroicGames, error, scan, importSelected, toggleAll }
}
