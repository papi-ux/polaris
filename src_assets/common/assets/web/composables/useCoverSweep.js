import { getCurrentScope, onScopeDispose, ref } from 'vue'

/** How often the run is read again while it is still looking games up. */
export const SWEEP_POLL_INTERVAL_MS = 2000

async function readJson(res) {
  try {
    return await res.json()
  } catch (e) {
    return null
  }
}

/** A proposal, plus what the review has decided about it. */
function rowFor(proposal) {
  return {
    uuid: proposal.uuid,
    name: proposal.name,
    outcome: proposal.outcome,
    note: proposal.note || '',
    title: proposal.title || '',
    providerGameId: proposal.provider_game_id || '',
    confidence: typeof proposal.confidence === 'number' ? proposal.confidence : null,
    releaseYear: proposal.release_year || null,
    // Only a proposal is kept by default. There is nothing to keep about the others.
    keep: proposal.outcome === 'proposed',
    posters: [],
    postersLoading: false,
    postersError: '',
    chosen: null,
    applied: false,
    applyError: '',
  }
}

/**
 * One pass over every game with no cover, and the review of what it proposed.
 *
 * The host proposes matches and stores nothing. Posters are read one row at a time, when the review
 * opens that row, because the host's preview cache holds sixty four entries and a run over a few
 * hundred games would evict the early rows long before anyone looked at them.
 *
 * Applying is the same three steps the Find Cover panel already takes for one game, repeated for each
 * row that was kept: list that game's posters, pick one, and save the entry pointing at it. Saving is
 * the caller's, because the entry belongs to the page.
 *
 * @returns Reactive run state, the rows under review, and start, stop, load, loadPosters and apply.
 */
export function useCoverSweep({ pollIntervalMs = SWEEP_POLL_INTERVAL_MS } = {}) {
  const sweep = ref(null)
  const rows = ref([])
  const loading = ref(false)
  const starting = ref(false)
  const applying = ref(false)
  const applied = ref(0)
  const error = ref('')

  let pollTimer = null
  let loadSequence = 0
  let appliedSequence = 0
  let disposed = false

  function stopPolling() {
    if (pollTimer) clearTimeout(pollTimer)
    pollTimer = null
  }

  const searching = () => sweep.value?.state === 'searching'

  /**
   * Fold a fresh run into the rows, keeping what the review has already decided.
   *
   * A poll arrives every couple of seconds while the run is going, and it must not throw away a
   * poster somebody has just looked at or a row they have just unticked.
   */
  function absorb(next) {
    sweep.value = next
    const previous = new Map(rows.value.map((row) => [row.uuid, row]))
    rows.value = (next?.proposals || []).map((proposal) => {
      const before = previous.get(proposal.uuid)
      const row = rowFor(proposal)
      if (!before) return row
      // The outcome can still change under a row while the run is going; everything the reviewer
      // touched survives that.
      return {
        ...row,
        keep: before.outcome === row.outcome ? before.keep : row.keep,
        posters: before.posters,
        chosen: before.chosen,
        postersError: before.postersError,
        applied: before.applied,
        applyError: before.applyError,
      }
    })
  }

  async function load() {
    const sequence = ++loadSequence
    loading.value = true
    error.value = ''
    try {
      const res = await fetch('./api/covers/sweep', { credentials: 'include' })
      const data = await readJson(res)
      if (sequence < appliedSequence || disposed) return
      appliedSequence = sequence
      if (res.ok && data?.status) {
        absorb(data.sweep || null)
      } else {
        error.value = data?.error || 'Could not read the cover search'
      }
    } catch (e) {
      error.value = 'Could not read the cover search'
    } finally {
      if (sequence === loadSequence) loading.value = false
    }
    if (sequence < appliedSequence || disposed) return
    stopPolling()
    if (searching()) pollTimer = setTimeout(load, pollIntervalMs)
  }

  async function start() {
    starting.value = true
    error.value = ''
    applied.value = 0
    try {
      const res = await fetch('./api/covers/sweep', {
        credentials: 'include',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: '{}',
      })
      const data = await readJson(res)
      if (disposed) return
      if (res.ok && data?.status) {
        absorb(data.sweep || null)
        stopPolling()
        if (searching()) pollTimer = setTimeout(load, pollIntervalMs)
      } else {
        // The host's own sentence, which names the fix for a missing or refused key.
        error.value = data?.error || 'Could not start the cover search'
        if (data?.sweep) absorb(data.sweep)
      }
    } catch (e) {
      error.value = 'Could not start the cover search'
    } finally {
      starting.value = false
    }
  }

  async function stop() {
    error.value = ''
    try {
      const res = await fetch('./api/covers/sweep', { credentials: 'include', method: 'DELETE' })
      const data = await readJson(res)
      if (disposed) return
      if (res.ok && data?.status) {
        stopPolling()
        absorb(data.sweep || null)
        // A run still finishing the game it is on keeps its state until the next read.
        if (searching()) pollTimer = setTimeout(load, pollIntervalMs)
      } else {
        error.value = data?.error || 'Could not stop the cover search'
      }
    } catch (e) {
      error.value = 'Could not stop the cover search'
    }
  }

  /** Read one row's posters, the way the Find Cover panel reads a candidate's. */
  async function loadPosters(row) {
    if (!row?.providerGameId || row.postersLoading || row.posters.length) return
    row.postersLoading = true
    row.postersError = ''
    try {
      const res = await fetch('./api/covers/choices', {
        credentials: 'include',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          uuid: row.uuid,
          provider_game_id: row.providerGameId,
          title: row.title,
        }),
      })
      const data = await readJson(res)
      if (disposed) return
      if (res.ok && data?.status) {
        row.posters = (data.choices || []).filter((choice) => choice?.token && choice?.preview)
        row.chosen = row.posters[0]?.token || null
        if (!row.posters.length) row.postersError = 'No poster came back for this match.'
      } else {
        row.postersError = data?.error || 'Could not read this game’s posters'
      }
    } catch (e) {
      row.postersError = 'Could not read this game’s posters'
    } finally {
      row.postersLoading = false
    }
  }

  /**
   * Store the poster for every row that was kept.
   *
   * @param saveCover Called with (uuid, path) for each stored image, to write it into the entry.
   *                  Returning false marks that row as failed and the rest carry on.
   */
  async function apply(saveCover) {
    applying.value = true
    applied.value = 0
    error.value = ''
    try {
      for (const row of rows.value) {
        if (!row.keep || row.applied || row.outcome !== 'proposed') continue
        row.applyError = ''
        await loadPosters(row)
        if (!row.chosen) {
          row.applyError = row.postersError || 'No poster to store for this game.'
          continue
        }
        try {
          const res = await fetch('./api/covers/select', {
            credentials: 'include',
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ uuid: row.uuid, token: row.chosen }),
          })
          const data = await readJson(res)
          if (disposed) return
          if (!res.ok || !data?.status || !data?.path) {
            row.applyError = data?.error || 'Could not store this cover'
            continue
          }
          const saved = await saveCover(row.uuid, data.path)
          if (saved === false) {
            row.applyError = 'Could not save the entry for this cover'
            continue
          }
          row.applied = true
          applied.value += 1
        } catch (e) {
          row.applyError = 'Could not store this cover'
        }
      }
    } finally {
      applying.value = false
    }
  }

  if (getCurrentScope()) {
    onScopeDispose(() => {
      disposed = true
      stopPolling()
    })
  }

  return {
    sweep,
    rows,
    loading,
    starting,
    applying,
    applied,
    error,
    load,
    start,
    stop,
    loadPosters,
    apply,
  }
}
