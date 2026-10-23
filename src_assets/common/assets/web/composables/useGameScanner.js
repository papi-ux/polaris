import { ref } from 'vue'

function staged(list = []) {
  return (list || []).map(g => ({
    ...g,
    selected: !g.already_imported
  }))
}

/**
 * Composable for scanning and importing Steam, Lutris, Heroic, ROM folder and
 * official Epic Games Launcher titles.
 *
 * @returns Reactive scanning state and import functions.
 */
export function useGameScanner() {
  const scanning = ref(false)
  const importing = ref(false)
  const importedGames = ref([])
  const steamGames = ref([])
  const lutrisGames = ref([])
  const heroicGames = ref([])
  const emulatorGames = ref([])
  // Official Epic Games Launcher titles, keyed by their exact Epic triple by the host.
  const epicGames = ref([])
  const switching = ref(false)
  // The ROM folders as the scan saw them: emulator install state and how many games each holds.
  const librarySources = ref([])
  const error = ref(null)

  const lists = { steam: steamGames, lutris: lutrisGames, heroic: heroicGames, emulator: emulatorGames, epic: epicGames }

  async function scan() {
    scanning.value = true
    error.value = null
    try {
      const res = await fetch('./api/games/scan', { credentials: 'include' })
      if (res.ok) {
        const data = await res.json()
        steamGames.value = staged(data.steam_games)
        lutrisGames.value = staged(data.lutris_games)
        heroicGames.value = staged(data.heroic_games)
        emulatorGames.value = staged(data.emulator_games)
        epicGames.value = staged(data.epic_games)
        librarySources.value = data.library_sources || []
      } else {
        error.value = 'Failed to scan for games'
      }
    } catch (e) {
      error.value = 'Scanner not available'
    } finally {
      scanning.value = false
    }
  }

  function allGames() {
    return [...steamGames.value, ...lutrisGames.value, ...heroicGames.value, ...emulatorGames.value, ...epicGames.value]
  }

  async function importSelected() {
    importedGames.value = []
    const selected = allGames().filter(g => g.selected && !g.already_imported)
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
            app_name: g.app_name || '',
            store: g.store || '',
            install: g.install || '',
            cmd: g.cmd || '',
            image_path: g.image_path || g['image-path'] || g.cover_path || '',
            game_category: g.game_category || '',
            genres: g.genres || [],
            // ROM folder entries: the host rebuilds the command from these two, never from cmd.
            source_id: g.source_id || '',
            rom_path: g.rom_path || '',
            // Official Epic entries: the host reconstructs metadata/command from the exact triple,
            // never from a browser-supplied command or path.
            namespace: g.namespace || '',
            item_id: g.item_id || '',
            artifact_id: g.artifact_id || ''
          }))
        })
      })
      const data = await res.json()
      if (res.ok && data.status) {
        importedGames.value = Array.isArray(data.imported_games) ? data.imported_games : []
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

  /**
   * Explicit, owner-driven launch-provider switch for an already-imported title
   * that both Heroic and the official launcher can provide. Never automatic: the
   * caller confirms first. The host preserves the app's UUID, art, history and
   * settings and only updates the launch provider/derived command; it refuses an
   * active app, an unavailable provider or an uncertain triple. On success the
   * scan is refreshed so the row reflects the new provider.
   *
   * @returns {Promise<boolean>} whether the switch was applied.
   */
  async function switchEpicProvider(uuid, { provider, namespace, item_id, artifact_id } = {}) {
    if (!uuid || (provider !== 'official' && provider !== 'heroic')) {
      error.value = 'Invalid provider switch request'
      return false
    }
    switching.value = true
    error.value = null
    try {
      const res = await fetch(`./api/apps/${encodeURIComponent(uuid)}/epic-provider`, {
        credentials: 'include',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ provider, namespace: namespace || '', item_id: item_id || '', artifact_id: artifact_id || '' })
      })
      let data = null
      try { data = await res.json() } catch (e) { /* tolerate empty body */ }
      if (res.ok && data && data.status) {
        await scan()
        return true
      }
      error.value = (data && data.error) || 'Could not switch provider'
      return false
    } catch (e) {
      error.value = 'Provider switch request failed'
      return false
    } finally {
      switching.value = false
    }
  }

  function toggleAll(val, source) {
    const target = source ? lists[source] : null
    const targets = target ? [target.value] : Object.values(lists).map(list => list.value)
    targets.forEach(list => {
      list.forEach(g => {
        if (!g.already_imported) g.selected = val
      })
    })
  }

  return {
    scanning, importing, switching, steamGames, lutrisGames, heroicGames, emulatorGames, epicGames, librarySources,
    error, importedGames, scan, importSelected, switchEpicProvider, toggleAll
  }
}
