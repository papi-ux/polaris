import { describe, expect, it } from 'vitest'
import {
  SETTINGS_METADATA_ENDPOINT,
  isValidSettingsMetadata,
  provenanceLabel,
  useConfigProjection,
} from './useConfigProjection'

const payload = () => ({
  status: true,
  version: 1,
  view: 'host',
  fields: {
    target_bitrate_kbps: { direction: 'read_write', status: 'synced', source_label: 'Nova on RP6' },
    display_mode: { direction: 'read_write', status: 'pending', source_label: '' },
  },
  field_map: { max_bitrate: 'target_bitrate_kbps', fallback_mode: 'display_mode' },
  modes: [{ value: 'headless_stream', label: 'Private Stream', badge: 'Recommended', available: true }],
  tuning: { adaptive_target_bitrate_kbps: 12345 },
  auto_quality: { state: 'holding' },
  stream_display: { configured: 'headless_stream', configured_label: 'Private Stream', effective: 'headless_stream', effective_label: 'Private Stream', relaunch_required: false },
  response_only_keys: ['status', 'stream_path_id'],
  live_fields: ['max_bitrate'],
  restart_fields: ['linux_stream_mode'],
  provenance: [{ at: '2026-09-02T22:10:00Z', writer: 'web_ui', keys: ['max_bitrate'] }],
})

const respond = (status, body) => async (url, init) => ({
  ok: status >= 200 && status < 300,
  status,
  url,
  init,
  json: async () => body,
})

describe('useConfigProjection', () => {
  it('binds the host projection when the endpoint answers with version 1', async () => {
    const calls = []
    const projection = useConfigProjection({
      fetchImpl: async (url, init) => {
        calls.push({ url, init })
        return respond(200, payload())(url, init)
      },
    })

    expect(await projection.load()).toBe(true)
    expect(calls[0].url).toBe(SETTINGS_METADATA_ENDPOINT)
    expect(calls[0].init.credentials).toBe('include')
    expect(projection.ok.value).toBe(true)
    expect(projection.modes.value[0].badge).toBe('Recommended')
    expect(projection.autoQuality.value.state).toBe('holding')
    expect(projection.responseOnlyKeys.value).toEqual(['status', 'stream_path_id'])
    expect(projection.provenance.value[0].writer).toBe('web_ui')
  })

  it('scopes to a paired client when asked', async () => {
    const calls = []
    const projection = useConfigProjection({
      fetchImpl: async (url, init) => {
        calls.push(url)
        return respond(200, payload())(url, init)
      },
    })

    await projection.load('client one')
    expect(calls[0]).toBe(`${SETTINGS_METADATA_ENDPOINT}?client=client%20one`)
  })

  it('falls back when the host predates the endpoint or the shape is wrong', async () => {
    const missing = useConfigProjection({ fetchImpl: respond(404, { status: false }) })
    expect(await missing.load()).toBe(false)
    expect(missing.ok.value).toBe(false)
    expect(missing.modes.value).toBeNull()
    expect(String(missing.error.value.message)).toContain('404')

    const drifted = useConfigProjection({ fetchImpl: respond(200, { status: true, version: 2, modes: [], fields: {} }) })
    expect(await drifted.load()).toBe(false)
    expect(drifted.ok.value).toBe(false)

    const thrown = useConfigProjection({ fetchImpl: async () => { throw new TypeError('offline') } })
    expect(await thrown.load()).toBe(false)
    expect(thrown.error.value).toBeInstanceOf(TypeError)
  })

  it('validates the payload shape it depends on', () => {
    expect(isValidSettingsMetadata(payload())).toBe(true)
    expect(isValidSettingsMetadata({ ...payload(), status: false })).toBe(false)
    expect(isValidSettingsMetadata({ ...payload(), modes: 'none' })).toBe(false)
    expect(isValidSettingsMetadata(null)).toBe(false)
  })

  it('reads provenance through the field map', async () => {
    const projection = useConfigProjection({ fetchImpl: respond(200, payload()) })
    await projection.load()

    expect(provenanceLabel(projection, 'max_bitrate')).toBe('Nova on RP6')
    expect(provenanceLabel(projection, 'fallback_mode')).toBe('')
    expect(provenanceLabel(projection, 'unmapped_key')).toBe('')

    const absent = useConfigProjection({ fetchImpl: respond(404, {}) })
    await absent.load()
    expect(provenanceLabel(absent, 'max_bitrate')).toBe('')
  })
})
