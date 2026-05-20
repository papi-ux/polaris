import { describe, expect, it } from 'vitest'
import { filterImportGames, summarizeImportGames } from './library-imports'

describe('Library import filtering', () => {
  const games = [
    { name: 'ARC Raiders', source: 'steam', appid: '1808500', game_category: 'fast_action', selected: true, already_imported: false },
    { name: 'Alan Wake 2', source: 'heroic', slug: 'alan-wake-2', runner: 'legendary', selected: false, already_imported: false },
    { name: 'Hades', source: 'lutris', slug: 'hades', runner: 'wine', selected: false, already_imported: true },
  ]

  it('summarizes staged, available, and imported games', () => {
    expect(summarizeImportGames(games)).toEqual({
      total: 3,
      available: 2,
      selected: 1,
      imported: 1,
    })
  })

  it('filters by status without hiding selected new games', () => {
    expect(filterImportGames(games, { status: 'new' }).map((game) => game.name)).toEqual(['ARC Raiders', 'Alan Wake 2'])
    expect(filterImportGames(games, { status: 'selected' }).map((game) => game.name)).toEqual(['ARC Raiders'])
    expect(filterImportGames(games, { status: 'imported' }).map((game) => game.name)).toEqual(['Hades'])
  })

  it('searches useful import metadata', () => {
    expect(filterImportGames(games, { query: '1808500', status: 'all' }).map((game) => game.name)).toEqual(['ARC Raiders'])
    expect(filterImportGames(games, { query: 'legendary', status: 'all' }).map((game) => game.name)).toEqual(['Alan Wake 2'])
    expect(filterImportGames(games, { query: 'fast action', status: 'all' }).map((game) => game.name)).toEqual(['ARC Raiders'])
  })
})
