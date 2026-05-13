export const CLIENT_SETTINGS_RESPONSE_ONLY_KEYS = [
  'client_settings_available',
  'client_settings_v1',
  'client_settings_endpoint',
  'client_settings_sync_mode',
  'client_settings_authority',
  'client_settings_relaunch_required',
  'client_settings_stream_display_mode',
  'client_settings_effective_stream_display_mode',
  'client_settings_stream_display_mode_label',
  'client_settings_effective_stream_display_mode_label',
  'client_settings_live_fields',
  'client_settings_restart_fields',
  'ai_auto_quality_enabled',
]

export function isTruthySetting(value) {
  return value === true || value === 'true' || value === 'enabled' || value === 1 || value === '1'
}

export function resolveStreamDisplayMode(config = {}) {
  const headless = isTruthySetting(config.headless_mode)
  const cage = isTruthySetting(config.linux_use_cage_compositor)
  const gpuNative = isTruthySetting(config.linux_prefer_gpu_native_capture)

  if (headless && cage && gpuNative) return 'windowed_stream'
  if (headless && cage) return 'headless_stream'
  if (headless) return 'host_virtual_display'
  return 'desktop_display'
}

export function resolveClientSettingsSync(config = {}) {
  const available = isTruthySetting(config.client_settings_available) || isTruthySetting(config.client_settings_v1)
  const relaunchRequired = isTruthySetting(config.client_settings_relaunch_required)
  const desiredMode = config.client_settings_stream_display_mode || resolveStreamDisplayMode(config)
  const effectiveMode = config.client_settings_effective_stream_display_mode || desiredMode

  let state = 'unavailable'
  if (available) {
    state = relaunchRequired ? 'pending_relaunch' : 'synced'
  }

  return {
    available,
    state,
    endpoint: config.client_settings_endpoint || '/polaris/v1/client-settings',
    mode: config.client_settings_sync_mode || (available ? 'bidirectional' : 'unavailable'),
    authority: config.client_settings_authority || 'polaris_effective_runtime',
    desiredMode,
    effectiveMode,
    desiredModeLabel: config.client_settings_stream_display_mode_label || labelForStreamDisplayMode(desiredMode),
    effectiveModeLabel: config.client_settings_effective_stream_display_mode_label || labelForStreamDisplayMode(effectiveMode),
    relaunchRequired,
    liveFields: normalizeFieldList(config.client_settings_live_fields),
    restartFields: normalizeFieldList(config.client_settings_restart_fields),
  }
}

export function labelForStreamDisplayMode(mode) {
  if (mode === 'headless_stream') return 'Headless Stream'
  if (mode === 'host_virtual_display') return 'Host Virtual Display'
  if (mode === 'windowed_stream') return 'GPU-Native Test'
  return 'Desktop Display'
}

export function normalizeFieldList(value) {
  if (Array.isArray(value)) return value
  if (typeof value === 'string') {
    try {
      const parsed = JSON.parse(value)
      if (Array.isArray(parsed)) return parsed
    } catch {}
    return value.split(',').map((item) => item.trim()).filter(Boolean)
  }
  return []
}

export function stripClientSettingsResponseOnly(config = {}) {
  for (const key of CLIENT_SETTINGS_RESPONSE_ONLY_KEYS) {
    delete config[key]
  }
  return config
}
