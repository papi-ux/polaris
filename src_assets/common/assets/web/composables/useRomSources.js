import { ref } from 'vue'

async function readJson(res) {
  try {
    return await res.json()
  } catch (e) {
    return null
  }
}

/**
 * The ROM folders Polaris scans for emulator games, and the emulator presets it knows.
 *
 * @returns Reactive folder state and the add, remove and load functions.
 */
export function useRomSources() {
  const presets = ref([])
  const sources = ref([])
  const loading = ref(false)
  const saving = ref(false)
  const error = ref('')

  async function load() {
    loading.value = true
    error.value = ''
    try {
      const res = await fetch('./api/library/sources', { credentials: 'include' })
      const data = await readJson(res)
      if (res.ok && data?.status) {
        presets.value = data.presets || []
        sources.value = data.sources || []
      } else {
        error.value = data?.error || 'Could not load the ROM folders'
      }
    } catch (e) {
      error.value = 'Could not load the ROM folders'
    } finally {
      loading.value = false
    }
  }

  async function add(payload) {
    saving.value = true
    error.value = ''
    try {
      const res = await fetch('./api/library/sources', {
        credentials: 'include',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      })
      const data = await readJson(res)
      if (res.ok && data?.status) {
        sources.value = data.sources || sources.value
        return true
      }
      error.value = data?.error || 'Could not add the folder'
    } catch (e) {
      error.value = 'Could not add the folder'
    } finally {
      saving.value = false
    }
    return false
  }

  async function remove(id) {
    saving.value = true
    error.value = ''
    try {
      const res = await fetch(`./api/library/sources/${encodeURIComponent(id)}`, {
        credentials: 'include',
        method: 'DELETE'
      })
      const data = await readJson(res)
      if (res.ok && data?.status) {
        sources.value = data.sources || sources.value.filter(source => source.id !== id)
        return true
      }
      error.value = data?.error || 'Could not remove the folder'
    } catch (e) {
      error.value = 'Could not remove the folder'
    } finally {
      saving.value = false
    }
    return false
  }

  return { presets, sources, loading, saving, error, load, add, remove }
}
