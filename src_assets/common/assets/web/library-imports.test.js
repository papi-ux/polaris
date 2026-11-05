import { describe, expect, it } from 'vitest'
import { epicPlatformChip, epicProviderLabel, filterImportGames, summarizeImportGames } from './library-imports'
import { sourceLabel } from './library-source-names'

describe('Library source names', () => {
  it('names every source properly and leaves an unknown one as it came', () => {
    expect(['steam', 'lutris', 'heroic', 'emulator', 'epic_launcher'].map((source) => sourceLabel(source)))
      .toEqual(['Steam', 'Lutris', 'Heroic', 'Emulator', 'Epic'])
    expect(sourceLabel('manual')).toBe('manual')
    expect(sourceLabel()).toBe('Unknown')
  })

  it('names who provides an Epic title, and nothing for a value it does not know', () => {
    expect(epicProviderLabel('official')).toBe('Epic')
    expect(epicProviderLabel('heroic')).toBe('Heroic')
    expect(epicProviderLabel('')).toBe('')
    expect(epicProviderLabel('other')).toBe('')
  })

  it('reads platform and runtime as one chip', () => {
    // What the host sends for an official Epic title and what Heroic records.
    expect(epicPlatformChip({ installed_platform: 'windows', runtime: 'proton' })).toBe('Windows via Proton')
    expect(epicPlatformChip({ installed_platform: 'windows', runtime: 'wine' })).toBe('Windows via Wine')
    expect(epicPlatformChip({ installed_platform: 'linux', runtime: 'native' })).toBe('Linux')
    expect(epicPlatformChip({ installed_platform: 'mac', runtime: 'native' })).toBe('macOS')
    // Only one known, or neither.
    expect(epicPlatformChip({ installed_platform: 'windows' })).toBe('Windows')
    expect(epicPlatformChip({ runtime: 'proton' })).toBe('Proton')
    expect(epicPlatformChip({ runtime: 'native' })).toBe('Native')
    expect(epicPlatformChip({})).toBe('')
    // Case the host did not settle, and a value it has not sent before, still read as names.
    expect(epicPlatformChip({ installed_platform: 'Windows', runtime: 'PROTON' })).toBe('Windows via Proton')
    expect(epicPlatformChip({ installed_platform: 'windows', runtime: 'crossover' })).toBe('Windows via Crossover')
  })
})

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

  it('searches official Epic metadata (provider, platform, runtime and exact triple)', () => {
    const epicGames = [
      { name: 'Alan Wake 2', source: 'epic_launcher', provider: 'official', installed_platform: 'Windows', runtime: 'proton', namespace: 'ns-aw2', item_id: 'it-aw2', artifact_id: 'af-aw2', already_imported: false },
      { name: 'Rocket League', source: 'epic_launcher', provider: 'heroic', installed_platform: 'Windows', runtime: 'proton', namespace: 'ns-rl', item_id: 'it-rl', artifact_id: 'af-rl', already_imported: false },
    ]
    expect(filterImportGames(epicGames, { query: 'ns-aw2', status: 'all' }).map((game) => game.name)).toEqual(['Alan Wake 2'])
    expect(filterImportGames(epicGames, { query: 'official', status: 'all' }).map((game) => game.name)).toEqual(['Alan Wake 2'])
    expect(filterImportGames(epicGames, { query: 'proton', status: 'all' }).map((game) => game.name)).toEqual(['Alan Wake 2', 'Rocket League'])
  })

  it('searches ROM folder metadata too', () => {
    const emulatorGames = [
      { name: 'Hades', source: 'emulator', emulator: 'eden', emulator_label: 'Eden', platform: 'Nintendo Switch', rom_path: '/roms/switch/Hades.nsp', already_imported: false },
      { name: 'Metroid Prime', source: 'emulator', emulator: 'dolphin', emulator_label: 'Dolphin', platform: 'GameCube and Wii', rom_path: '/roms/gc/Metroid Prime.rvz', already_imported: false },
    ]
    expect(filterImportGames(emulatorGames, { query: 'switch', status: 'all' }).map((game) => game.name)).toEqual(['Hades'])
    expect(filterImportGames(emulatorGames, { query: 'dolphin', status: 'all' }).map((game) => game.name)).toEqual(['Metroid Prime'])
    expect(filterImportGames(emulatorGames, { query: 'hades.nsp', status: 'all' }).map((game) => game.name)).toEqual(['Hades'])
  })
})
