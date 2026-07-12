import { afterEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import {
  permissionMapping,
  permissionPresetKey,
  permissionPresetMask,
  useClients,
} from './composables/useClients'

describe('client permission presets', () => {
  it('maps Standard Access to the existing default permission mask', () => {
    expect(permissionPresetMask('standard')).toBe(permissionMapping._default)
    expect(permissionPresetKey(permissionMapping._default)).toBe('standard')
  })

  it('maps Game Control to launch, view, list, and all input permissions only', () => {
    const mask = permissionPresetMask('game_control')

    expect(mask).toBe(permissionMapping._game_control)
    expect(mask & permissionMapping._all_actions).toBe(permissionMapping._all_actions)
    expect(mask & permissionMapping._all_inputs).toBe(permissionMapping._all_inputs)
    expect(mask & permissionMapping._all_operations).toBe(0)
    expect(permissionPresetKey(mask)).toBe('game_control')
  })

  it('maps Full Control to the existing full permission mask', () => {
    expect(permissionPresetMask('full')).toBe(permissionMapping._all)
    expect(permissionPresetKey(permissionMapping._all)).toBe('full')
  })
})

describe('paired client metadata', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('preserves added and last-seen timestamps from the clients API', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
      json: async () => ({
        status: true,
        named_certs: [{
          name: 'Retroid',
          uuid: 'client-1',
          perm: permissionMapping._game_control,
          connected: false,
          paired_at: 1_720_000_000,
          last_seen_at: 1_720_003_600,
        }],
      }),
    }))
    const { clients, refreshClients } = useClients()

    await refreshClients()

    expect(clients.value[0]).toMatchObject({
      paired_at: 1_720_000_000,
      last_seen_at: 1_720_003_600,
    })
  })
})


describe('client revocation failures', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('rejects single-client unpair when the backend reports status false', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
      json: async () => ({ status: false, error: 'Paired-client revocation could not be persisted' }),
    }))
    const { unpairSingle } = useClients()

    await expect(unpairSingle('client-1')).rejects.toThrow('Paired-client revocation could not be persisted')
  })

  it('rejects unpair-all when the backend reports status false', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
      json: async () => ({ status: false, error: 'Paired-client revocation could not be persisted' }),
    }))
    const { unpairAll } = useClients()

    await expect(unpairAll()).rejects.toThrow('Paired-client revocation could not be persisted')
  })
})

describe('client update failures', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('rejects a client update when the backend reports status false', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
      ok: true,
      json: async () => ({ status: false, error: 'Paired-client update could not be persisted' }),
    }))
    const { saveClient } = useClients()
    const client = {
      uuid: 'client-1',
      editName: 'Retroid',
      editDisplayMode: '',
      editAllowClientCommands: false,
      editEnableLegacyOrdering: false,
      editAlwaysUseVirtualDisplay: false,
      editPerm: permissionMapping._game_control,
      edit_do: [],
      edit_undo: [],
    }

    await expect(saveClient(client)).rejects.toThrow('Paired-client update could not be persisted')
  })

  it('only leaves edit mode after the client update succeeds', () => {
    const source = readFileSync(resolve(process.cwd(), 'src_assets/common/assets/web/views/PinView.vue'), 'utf8')
    const start = source.indexOf('function saveClient(client)')
    const end = source.indexOf('\nfunction togglePermission', start)
    const handler = source.slice(start, end)

    expect(handler).toMatch(/saveClientAPI\(client\)\s*\.then\(\(\) => \{\s*client\.editing = false/)
    expect(handler).toMatch(/\.catch\(err => \{\s*client\.editing = true/)
  })
})
