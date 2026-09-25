import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { effectScope } from 'vue'
import { useCoverSweep } from './composables/useCoverSweep.js'

/**
 * The review of a cover sweep.
 *
 * The host proposes and stores nothing, so every way this can go wrong is on this side: a poll
 * throwing away a decision the reviewer just made, a row applied twice, one row's failure taking the
 * rest of them down, or a timer still running after the page is gone.
 */

const sweepReady = (proposals) => ({
  state: 'ready',
  total: proposals.length,
  looked_at: proposals.length,
  proposed: proposals.filter((p) => p.outcome === 'proposed').length,
  message: 'Found a cover for 1 of 2 games.',
  started_at: 1700000000,
  finished_at: 1700000012,
  proposals,
})

const proposed = (uuid, name, title) => ({
  uuid,
  name,
  outcome: 'proposed',
  provider_game_id: '2254',
  title,
  confidence: 97,
  release_year: 2004,
})

const noMatch = (uuid, name) => ({ uuid, name, outcome: 'no_match', note: 'No game by that name.' })

function jsonOnce(body, ok = true, status = 200) {
  return { ok, status, json: async () => body }
}

describe('useCoverSweep', () => {
  let scope
  let sweep

  beforeEach(() => {
    scope = effectScope()
    global.fetch = vi.fn()
  })

  afterEach(() => {
    scope.stop()
    vi.restoreAllMocks()
    vi.useRealTimers()
  })

  const run = (fn) => scope.run(fn)

  it('reads a finished run and keeps only the rows with a match', async () => {
    global.fetch.mockResolvedValueOnce(
      jsonOnce({ status: true, sweep: sweepReady([proposed('u1', 'Blank Game', 'Half-Life 2'), noMatch('u2', 'Odd Entry')]) }),
    )
    sweep = run(() => useCoverSweep())
    await sweep.load()

    expect(sweep.sweep.value.state).toBe('ready')
    expect(sweep.rows.value.map((r) => r.name)).toEqual(['Blank Game', 'Odd Entry'])
    expect(sweep.rows.value[0].keep).toBe(true)
    expect(sweep.rows.value[0].title).toBe('Half-Life 2')
    expect(sweep.rows.value[0].confidence).toBe(97)
    // Nothing to keep about a row with no match, so it is not offered as one.
    expect(sweep.rows.value[1].keep).toBe(false)
    expect(sweep.rows.value[1].note).toBe('No game by that name.')
  })

  it('a poll does not undo what the reviewer has already decided', async () => {
    const searching = { ...sweepReady([proposed('u1', 'Blank Game', 'Half-Life 2')]), state: 'searching' }
    global.fetch.mockResolvedValue(jsonOnce({ status: true, sweep: searching }))
    sweep = run(() => useCoverSweep({ pollIntervalMs: 10 }))
    await sweep.load()

    sweep.rows.value[0].keep = false
    sweep.rows.value[0].posters = [{ token: 'abc', preview: './api/covers/preview/abc' }]
    sweep.rows.value[0].chosen = 'abc'

    await sweep.load()

    expect(sweep.rows.value[0].keep).toBe(false)
    expect(sweep.rows.value[0].chosen).toBe('abc')
    expect(sweep.rows.value[0].posters).toHaveLength(1)
  })

  it('carries the host sentence when a run cannot start', async () => {
    global.fetch.mockResolvedValueOnce(
      jsonOnce({ status: false, error: 'Add a SteamGridDB API key in Settings.' }, false, 503),
    )
    sweep = run(() => useCoverSweep())
    await sweep.start()
    expect(sweep.error.value).toBe('Add a SteamGridDB API key in Settings.')
  })

  it('reports a run already going, and shows the run it answered with', async () => {
    const running = { ...sweepReady([proposed('u1', 'Blank Game', 'Half-Life 2')]), state: 'searching' }
    global.fetch.mockResolvedValueOnce(
      jsonOnce({ status: false, error: 'A cover search is already running.', sweep: running }, false, 409),
    )
    sweep = run(() => useCoverSweep({ pollIntervalMs: 10 }))
    await sweep.start()
    expect(sweep.error.value).toBe('A cover search is already running.')
    expect(sweep.sweep.value.state).toBe('searching')
  })

  it('applies every kept row and leaves the others alone', async () => {
    global.fetch
      .mockResolvedValueOnce(
        jsonOnce({ status: true, sweep: sweepReady([proposed('u1', 'One', 'Game One'), proposed('u2', 'Two', 'Game Two')]) }),
      )
      // u1: posters, then the pick
      .mockResolvedValueOnce(jsonOnce({ status: true, choices: [{ token: 't1', preview: 'p1' }] }))
      .mockResolvedValueOnce(jsonOnce({ status: true, path: '/covers/u1.png' }))
      // u2 is not kept, so nothing is asked about it

    sweep = run(() => useCoverSweep())
    await sweep.load()
    sweep.rows.value[1].keep = false

    const saved = []
    await sweep.apply((uuid, path) => {
      saved.push([uuid, path])
      return true
    })

    expect(saved).toEqual([['u1', '/covers/u1.png']])
    expect(sweep.applied.value).toBe(1)
    expect(sweep.rows.value[0].applied).toBe(true)
    expect(sweep.rows.value[1].applied).toBe(false)
  })

  it('one row failing does not stop the rest', async () => {
    global.fetch
      .mockResolvedValueOnce(
        jsonOnce({ status: true, sweep: sweepReady([proposed('u1', 'One', 'Game One'), proposed('u2', 'Two', 'Game Two')]) }),
      )
      // u1's posters come back empty, so there is nothing to store
      .mockResolvedValueOnce(jsonOnce({ status: true, choices: [] }))
      // u2 works
      .mockResolvedValueOnce(jsonOnce({ status: true, choices: [{ token: 't2', preview: 'p2' }] }))
      .mockResolvedValueOnce(jsonOnce({ status: true, path: '/covers/u2.png' }))

    sweep = run(() => useCoverSweep())
    await sweep.load()

    const saved = []
    await sweep.apply((uuid, path) => {
      saved.push(uuid)
      return true
    })

    expect(saved).toEqual(['u2'])
    expect(sweep.applied.value).toBe(1)
    expect(sweep.rows.value[0].applied).toBe(false)
    expect(sweep.rows.value[0].applyError).toBeTruthy()
  })

  it('a row the page could not save is marked rather than counted', async () => {
    global.fetch
      .mockResolvedValueOnce(jsonOnce({ status: true, sweep: sweepReady([proposed('u1', 'One', 'Game One')]) }))
      .mockResolvedValueOnce(jsonOnce({ status: true, choices: [{ token: 't1', preview: 'p1' }] }))
      .mockResolvedValueOnce(jsonOnce({ status: true, path: '/covers/u1.png' }))

    sweep = run(() => useCoverSweep())
    await sweep.load()
    await sweep.apply(() => false)

    expect(sweep.applied.value).toBe(0)
    expect(sweep.rows.value[0].applied).toBe(false)
    expect(sweep.rows.value[0].applyError).toBeTruthy()
  })

  it('a row is read for posters once, however often it is opened', async () => {
    global.fetch
      .mockResolvedValueOnce(jsonOnce({ status: true, sweep: sweepReady([proposed('u1', 'One', 'Game One')]) }))
      .mockResolvedValueOnce(jsonOnce({ status: true, choices: [{ token: 't1', preview: 'p1' }] }))

    sweep = run(() => useCoverSweep())
    await sweep.load()
    const row = sweep.rows.value[0]
    await sweep.loadPosters(row)
    await sweep.loadPosters(row)
    await sweep.loadPosters(row)

    // The run, then one read of the posters, and nothing more.
    expect(global.fetch).toHaveBeenCalledTimes(2)
    expect(row.posters).toHaveLength(1)
    expect(row.chosen).toBe('t1')
  })

  it('stops polling when the page goes away', async () => {
    vi.useFakeTimers()
    const searching = { ...sweepReady([proposed('u1', 'One', 'Game One')]), state: 'searching' }
    global.fetch.mockResolvedValue(jsonOnce({ status: true, sweep: searching }))
    sweep = run(() => useCoverSweep({ pollIntervalMs: 1000 }))
    await sweep.load()
    const before = global.fetch.mock.calls.length

    scope.stop()
    await vi.advanceTimersByTimeAsync(5000)

    expect(global.fetch.mock.calls.length).toBe(before)
  })
})
