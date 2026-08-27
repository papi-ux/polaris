import { afterEach, describe, expect, it, vi } from 'vitest'
import { useGameScanner } from './composables/useGameScanner'

describe('Game scanner import contract', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('round-trips validated Heroic identity metadata to the import endpoint', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({
          heroic_games: [{
            name: 'Alan Wake 2',
            app_name: 'HEROIC-APP-ID',
            store: 'epic',
            runner: 'legendary',
            install: 'flatpak',
            cmd: "untrusted display copy",
            source: 'heroic',
            already_imported: false,
          }],
        }),
      })
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({ status: true, imported: 1 }),
      })
    vi.stubGlobal('fetch', fetchMock)

    const scanner = useGameScanner()
    await scanner.scan()
    expect(scanner.heroicGames.value[0].selected).toBe(true)

    await expect(scanner.importSelected()).resolves.toBe(1)
    const request = fetchMock.mock.calls[1][1]
    const payload = JSON.parse(request.body)
    expect(payload.games).toHaveLength(1)
    expect(payload.games[0]).toMatchObject({
      name: 'Alan Wake 2',
      app_name: 'HEROIC-APP-ID',
      store: 'epic',
      runner: 'legendary',
      install: 'flatpak',
      source: 'heroic',
    })
  })
})
