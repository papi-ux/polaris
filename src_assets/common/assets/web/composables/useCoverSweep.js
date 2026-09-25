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
    // The position of the poster this row will store, not its token. A token belongs to the host's
    // preview cache, which holds sixty four of them across every row, so opening thirteen rows
    // evicts the first one's. The provider returns a game's posters in the same order every time, so
    // a position survives being read again and a token does not.
    chosenIndex: 0,
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
  // The last start found nothing to look up, which is an answer rather than a failure.
  const nothingToDo = ref(false)
  const loading = ref(false)
  const starting = ref(false)
  const applying = ref(false)
  const applied = ref(0)
  const error = ref('')

  // The poster read in flight for each row, so a second caller awaits it rather than skipping it.
  const inFlight = new Map()
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
    // An apply is walking rows.value right now. Rebuilding it would hand that loop orphaned objects:
    // every row it marked applied after this point would be marked on something nothing renders, the
    // count would disagree with the rows, and pressing Apply again would store those covers twice.
    if (applying.value) return
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
        chosenIndex: before.chosenIndex,
        postersLoading: before.postersLoading,
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
    nothingToDo.value = false
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
        nothingToDo.value = Boolean(data.nothing_to_do)
        absorb(data.sweep || null)
        stopPolling()
        if (searching()) pollTimer = setTimeout(load, pollIntervalMs)
      } else {
        // The host's own sentence, which names the fix for a missing or refused key.
        error.value = data?.error || 'Could not start the cover search'
        if (data?.sweep) absorb(data.sweep)
        // Refused because one is already going: follow that one rather than leaving it frozen.
        stopPolling()
        if (searching()) pollTimer = setTimeout(load, pollIntervalMs)
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

  /**
   * Read one row's posters, the way the Find Cover panel reads a candidate's.
   *
   * @param force Read them again even if this row already has some, because the tokens they carry
   *              expire and get evicted. An apply always forces, so it stores with a fresh one.
   */
  async function loadPosters(row, { force = false } = {}) {
    if (!row?.providerGameId) return
    // Already on its way. Awaiting the same promise is what stops an apply, which forces, from
    // deciding there is no poster while the first read is still in flight.
    if (row.postersLoading) return inFlight.get(row.uuid)
    if (row.posters.length && !force) return
    row.postersLoading = true
    row.postersError = ''
    const read = (async () => {
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
        // A read again keeps the position the reviewer picked, unless there are fewer posters now.
        if (row.chosenIndex >= row.posters.length) row.chosenIndex = 0
        if (!row.posters.length) row.postersError = 'No poster came back for this match.'
      } else {
        row.postersError = data?.error || 'Could not read this game’s posters'
      }
    } catch (e) {
      row.postersError = 'Could not read this game’s posters'
    } finally {
      row.postersLoading = false
      inFlight.delete(row.uuid)
    }
    })()
    inFlight.set(row.uuid, read)
    return read
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
        // Read the posters again, always. A token the reviewer's browser is still showing may already
        // have been evicted from the host's cache by the rows they opened after it, and select refuses
        // an evicted token.
        await loadPosters(row, { force: true })
        const token = row.posters[row.chosenIndex]?.token
        if (!token) {
          row.applyError = row.postersError || 'No poster to store for this game.'
          continue
        }
        try {
          const res = await fetch('./api/covers/select', {
            credentials: 'include',
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ uuid: row.uuid, token }),
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
    nothingToDo,
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
