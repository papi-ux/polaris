import { computed, ref } from 'vue'

export const SETTINGS_METADATA_ENDPOINT = './api/settings/metadata'

/**
 * The host projection this page binds to: GET /api/settings/metadata, one
 * aggregate payload with version 1. A host without the endpoint answers 404
 * and every consumer falls back to the config-derived behaviour it had before.
 */
export function isValidSettingsMetadata(payload) {
  return Boolean(
    payload
    && typeof payload === 'object'
    && payload.status === true
    && payload.version === 1
    && Array.isArray(payload.modes)
    && payload.fields
    && typeof payload.fields === 'object',
  )
}

export function useConfigProjection({ fetchImpl } = {}) {
  const payload = ref(null)
  const ok = ref(false)
  const loading = ref(false)
  const error = ref(null)

  async function load(clientUuid = '') {
    const doFetch = fetchImpl || ((...args) => fetch(...args))
    const url = clientUuid
      ? `${SETTINGS_METADATA_ENDPOINT}?client=${encodeURIComponent(clientUuid)}`
      : SETTINGS_METADATA_ENDPOINT
    loading.value = true
    error.value = null
    try {
      const response = await doFetch(url, {
        credentials: 'include',
        headers: { Accept: 'application/json' },
      })
      if (!response.ok) {
        throw new Error(`settings metadata unavailable (HTTP ${response.status})`)
      }
      const body = await response.json()
      if (!isValidSettingsMetadata(body)) {
        throw new Error('settings metadata has an unexpected shape')
      }
      payload.value = body
      ok.value = true
    } catch (cause) {
      payload.value = null
      ok.value = false
      error.value = cause
    } finally {
      loading.value = false
    }
    return ok.value
  }

  const section = (key) => computed(() => (ok.value && payload.value ? payload.value[key] ?? null : null))

  return {
    ok,
    loading,
    error,
    payload,
    load,
    fields: section('fields'),
    fieldMap: section('field_map'),
    modes: section('modes'),
    tuning: section('tuning'),
    autoQuality: section('auto_quality'),
    streamDisplay: section('stream_display'),
    sync: section('sync'),
    clients: section('clients'),
    responseOnlyKeys: section('response_only_keys'),
    liveFields: section('live_fields'),
    restartFields: section('restart_fields'),
  }
}

/**
 * Who last set a config key, from the projection's per-field provenance.
 * `field_map` translates config keys to the nvhttp sync-field names that
 * `fields` is keyed by. Empty when the projection is absent or silent.
 */
export function provenanceLabel(projection, configKey) {
  const map = projection?.fieldMap?.value
  const fields = projection?.fields?.value
  if (!map || !fields) return ''
  const fieldName = map[configKey]
  const entry = fieldName ? fields[fieldName] : null
  const label = entry && typeof entry.source_label === 'string' ? entry.source_label.trim() : ''
  return label
}
