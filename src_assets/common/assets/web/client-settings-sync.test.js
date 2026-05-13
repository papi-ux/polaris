import { describe, expect, it } from 'vitest'
import {
  resolveClientSettingsSync,
  resolveStreamDisplayMode,
  stripClientSettingsResponseOnly,
} from './client-settings-sync'

describe('client settings sync helpers', () => {
  it('maps legacy display flags to the canonical stream display mode', () => {
    expect(resolveStreamDisplayMode({
      headless_mode: 'enabled',
      linux_use_cage_compositor: 'enabled',
      linux_prefer_gpu_native_capture: 'disabled',
    })).toBe('headless_stream')

    expect(resolveStreamDisplayMode({
      headless_mode: 'enabled',
      linux_use_cage_compositor: 'disabled',
    })).toBe('host_virtual_display')

    expect(resolveStreamDisplayMode({
      headless_mode: 'enabled',
      linux_use_cage_compositor: 'enabled',
      linux_prefer_gpu_native_capture: 'enabled',
    })).toBe('windowed_stream')
  })

  it('reports unavailable hosts clearly', () => {
    const sync = resolveClientSettingsSync({})

    expect(sync.available).toBe(false)
    expect(sync.state).toBe('unavailable')
    expect(sync.mode).toBe('unavailable')
  })

  it('reports pending relaunch when Polaris desired and effective modes differ', () => {
    const sync = resolveClientSettingsSync({
      client_settings_available: true,
      client_settings_relaunch_required: true,
      client_settings_stream_display_mode: 'headless_stream',
      client_settings_effective_stream_display_mode: 'desktop_display',
    })

    expect(sync.available).toBe(true)
    expect(sync.state).toBe('pending_relaunch')
    expect(sync.relaunchRequired).toBe(true)
    expect(sync.desiredModeLabel).toBe('Headless Stream')
    expect(sync.effectiveModeLabel).toBe('Desktop Display')
  })

  it('strips server-only client-settings fields before config save', () => {
    const config = stripClientSettingsResponseOnly({
      headless_mode: 'enabled',
      client_settings_available: true,
      client_settings_endpoint: '/polaris/v1/client-settings',
      ai_auto_quality_enabled: 'enabled',
    })

    expect(config).toEqual({ headless_mode: 'enabled' })
  })
})
