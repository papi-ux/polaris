import { getCurrentScope, onScopeDispose, ref } from 'vue'

/** How often the folder list is read again while an emulator installs. */
export const INSTALL_POLL_INTERVAL_MS = 2000

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
 * An emulator install from Flathub runs on the host in the background; while one is
 * running the folder list is read again every few seconds, and each finished install
 * is reported once to the onInstallFinished listeners.
 *
 * @returns Reactive folder state and the add, remove, load and install functions.
 */
export function useRomSources({ pollIntervalMs = INSTALL_POLL_INTERVAL_MS } = {}) {
  const presets = ref([])
  const sources = ref([])
  const loading = ref(false)
  const saving = ref(false)
  const error = ref('')
  // Emulator ids whose install request is on its way to the host.
  const installRequests = ref({})
  const jobStates = new Map()
  const finishedListeners = []
  let pollTimer = null
  let loadSequence = 0
  let appliedSequence = 0

  function stopInstallPolling() {
    if (pollTimer) clearTimeout(pollTimer)
    pollTimer = null
  }

  function noteInstallJobs(nextPresets) {
    const finished = []
    for (const preset of nextPresets) {
      if (!preset?.id) continue
      const job = preset.install_job || null
      const state = job?.state || ''
      if (jobStates.get(preset.id) === 'installing' && state && state !== 'installing') {
        finished.push({ emulator: preset.id, job })
      }
      jobStates.set(preset.id, state)
    }
    return finished
  }

  async function load() {
    const sequence = ++loadSequence
    loading.value = true
    error.value = ''
    let finished = []
    try {
      const res = await fetch('./api/library/sources', { credentials: 'include' })
      const data = await readJson(res)
      // A poll and a click can overlap; an answer older than one already shown is dropped.
      if (sequence < appliedSequence) return
      appliedSequence = sequence
      if (res.ok && data?.status) {
        presets.value = data.presets || []
        sources.value = data.sources || []
        finished = noteInstallJobs(presets.value)
      } else {
        error.value = data?.error || 'Could not load the ROM folders'
      }
    } catch (e) {
      error.value = 'Could not load the ROM folders'
    } finally {
      if (sequence === loadSequence) loading.value = false
    }
    if (sequence < appliedSequence) return
    stopInstallPolling()
    if (presets.value.some((preset) => preset?.install_job?.state === 'installing')) {
      pollTimer = setTimeout(load, pollIntervalMs)
    }
    for (const entry of finished) {
      for (const listener of finishedListeners) listener(entry)
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

  /** Ask the host to install a preset's emulator from Flathub; progress arrives through load. */
  async function install(emulator) {
    if (!emulator || installRequests.value[emulator]) return false
    installRequests.value = { ...installRequests.value, [emulator]: true }
    error.value = ''
    let started = false
    let failure = ''
    try {
      const res = await fetch('./api/library/emulators/install', {
        credentials: 'include',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ emulator })
      })
      const data = await readJson(res)
      if (res.ok && data?.status) {
        started = true
      } else {
        failure = data?.error || 'Could not start the install'
      }
    } catch (e) {
      failure = 'Could not start the install'
    } finally {
      const pending = { ...installRequests.value }
      delete pending[emulator]
      installRequests.value = pending
    }
    if (started) {
      // The job may already be over by the time the list is read; either way it reports once.
      jobStates.set(emulator, 'installing')
    }
    await load()
    if (failure) error.value = failure
    return started
  }

  function onInstallFinished(listener) {
    finishedListeners.push(listener)
  }

  if (getCurrentScope()) onScopeDispose(stopInstallPolling)

  return {
    presets, sources, loading, saving, error, installRequests,
    load, add, remove, install, onInstallFinished, stopInstallPolling
  }
}
