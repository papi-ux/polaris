import { describe, expect, it } from 'vitest'
import {
  resolveClientSettingsSync,
  resolveStreamDisplayMode,
  resolveStreamDisplayRuntimeNotice,
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

  it('labels GPU-native as a primary stream mode', () => {
    const sync = resolveClientSettingsSync({
      client_settings_available: true,
      client_settings_stream_display_mode: 'windowed_stream',
    })

    expect(sync.desiredModeLabel).toBe('GPU-Native Stream')
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

  it('uses pending relaunch copy only when desired and effective stream display modes differ', () => {
    const pending = resolveStreamDisplayRuntimeNotice(
      resolveClientSettingsSync({
        client_settings_available: true,
        client_settings_relaunch_required: true,
        client_settings_stream_display_mode: 'windowed_stream',
        client_settings_effective_stream_display_mode: 'headless_stream',
      }),
      'windowed_stream'
    )

    expect(pending.state).toBe('pending_relaunch')
    expect(pending.copy).toContain('Pending relaunch')
    expect(pending.copy).toContain('Headless Stream')
    expect(pending.copy).toContain('GPU-Native Stream')
  })

  it('shows saved active-mode copy after GPU-native stream is synced', () => {
    const notice = resolveStreamDisplayRuntimeNotice(
      resolveClientSettingsSync({
        client_settings_available: true,
        client_settings_relaunch_required: false,
        client_settings_stream_display_mode: 'windowed_stream',
        client_settings_effective_stream_display_mode: 'windowed_stream',
      }),
      'windowed_stream'
    )

    expect(notice.state).toBe('synced')
    expect(notice.copy).toContain('GPU-Native Stream saved')
    expect(notice.copy).toContain('no pending relaunch')
    expect(notice.copy).not.toContain('Requires restart')
  })

  it('calls out unsaved local display-mode changes before relaunch guidance', () => {
    const notice = resolveStreamDisplayRuntimeNotice(
      resolveClientSettingsSync({
        client_settings_available: true,
        client_settings_relaunch_required: false,
        client_settings_stream_display_mode: 'headless_stream',
        client_settings_effective_stream_display_mode: 'headless_stream',
      }),
      'windowed_stream'
    )

    expect(notice.state).toBe('unsaved')
    expect(notice.copy).toContain('GPU-Native Stream selected')
    expect(notice.copy).toContain('Save')
    expect(notice.copy).not.toContain('Requires restart')
  })

  it('strips server-only client-settings fields before config save', () => {
    const config = stripClientSettingsResponseOnly({
      headless_mode: 'enabled',
      client_settings_available: true,
      client_settings_endpoint: 'https://10.0.0.232:47984/polaris/v1/client-settings',
      client_settings_endpoint_path: '/polaris/v1/client-settings',
      client_settings_endpoint_origin: 'gamestream_https',
      client_settings_endpoint_same_origin: false,
      client_settings_endpoint_https_port: 47984,
      client_settings_endpoint_base_url: 'https://10.0.0.232:47984',
      client_settings_endpoint_url: 'https://10.0.0.232:47984/polaris/v1/client-settings',
      ai_auto_quality_enabled: 'enabled',
    })

    expect(config).toEqual({ headless_mode: 'enabled' })
  })
})
