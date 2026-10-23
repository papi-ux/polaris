import { afterEach, describe, expect, it, vi } from 'vitest'
import { useGameScanner } from './useGameScanner.js'

const reply = (body, ok = true, status = ok ? 200 : 400) => ({ ok, status, json: async () => body })
const epicEntry = extra => ({
  name: 'Alan Wake 2', source: 'epic_launcher', namespace: 'ns1', item_id: 'it1', artifact_id: 'af1',
  already_imported: false, existing_uuid: '', providers: ['official', 'heroic'], provider: 'official',
  installed_platform: 'Windows', runtime: 'proton', lifecycle: 'owner_confirmation', ...extra,
})

afterEach(() => { vi.unstubAllGlobals() })

describe('useGameScanner Epic support', () => {
  it('stages epic_games from the scan with their exact triple fields', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ epic_games: [epicEntry()] })))
    const scanner = useGameScanner()
    await scanner.scan()
    expect(scanner.epicGames.value).toHaveLength(1)
    const game = scanner.epicGames.value[0]
    expect(game.selected).toBe(true)
    expect(game).toMatchObject({ source: 'epic_launcher', namespace: 'ns1', item_id: 'it1', artifact_id: 'af1', provider: 'official' })
  })

  it('does not re-stage an already-imported epic title as selected', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => reply({ epic_games: [epicEntry({ already_imported: true, existing_uuid: 'u9' })] })))
    const scanner = useGameScanner()
    await scanner.scan()
    expect(scanner.epicGames.value[0].selected).toBe(false)
  })

  it('imports a selected official entry carrying source and the exact triple, never a browser command', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(reply({ epic_games: [epicEntry()] })) // scan
      .mockResolvedValueOnce(reply({ status: true, imported: 1, imported_games: [] })) // import
    vi.stubGlobal('fetch', fetchMock)
    const scanner = useGameScanner()
    await scanner.scan()
    expect(await scanner.importSelected()).toBe(1)
    const body = JSON.parse(fetchMock.mock.calls[1][1].body)
    expect(body.games).toHaveLength(1)
    expect(body.games[0]).toMatchObject({ source: 'epic_launcher', namespace: 'ns1', item_id: 'it1', artifact_id: 'af1' })
    expect(body.games[0].cmd).toBe('')
  })

  it('switchEpicProvider posts the provider and triple to the per-app endpoint and refreshes the scan', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(reply({ status: true })) // provider switch
      .mockResolvedValueOnce(reply({ epic_games: [epicEntry({ provider: 'heroic' })] })) // refreshed scan
    vi.stubGlobal('fetch', fetchMock)
    const scanner = useGameScanner()
    const ok = await scanner.switchEpicProvider('app-uuid-1', { provider: 'heroic', namespace: 'ns1', item_id: 'it1', artifact_id: 'af1' })
    expect(ok).toBe(true)
    expect(fetchMock.mock.calls[0][0]).toBe('./api/apps/app-uuid-1/epic-provider')
    expect(fetchMock.mock.calls[0][1].method).toBe('POST')
    expect(JSON.parse(fetchMock.mock.calls[0][1].body)).toEqual({ provider: 'heroic', namespace: 'ns1', item_id: 'it1', artifact_id: 'af1' })
    expect(scanner.epicGames.value[0].provider).toBe('heroic')
  })

  it('refuses an invalid provider without any request', async () => {
    const fetchMock = vi.fn()
    vi.stubGlobal('fetch', fetchMock)
    const scanner = useGameScanner()
    expect(await scanner.switchEpicProvider('u1', { provider: 'nope' })).toBe(false)
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('surfaces a refused provider switch and does not refresh the scan', async () => {
    const fetchMock = vi.fn(async () => reply({ status: false, error: 'App is active' }, false, 409))
    vi.stubGlobal('fetch', fetchMock)
    const scanner = useGameScanner()
    expect(await scanner.switchEpicProvider('u1', { provider: 'official', namespace: 'ns1', item_id: 'it1', artifact_id: 'af1' })).toBe(false)
    expect(scanner.error.value).toBe('App is active')
    expect(fetchMock).toHaveBeenCalledTimes(1) // no scan refresh after refusal
  })
})
