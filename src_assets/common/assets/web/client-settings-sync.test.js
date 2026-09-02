import { describe, expect, it } from 'vitest'
import {
  CONFIG_RESPONSE_ONLY_KEYS,
  applyStreamDisplayModeToConfig,
  labelForStreamDisplayMode,
  resolveClientSettingsSync,
  resolveConfigResponseOnlyKeys,
  resolveStreamDisplayMode,
  resolveStreamDisplayModeAvailability,
  resolveStreamDisplayRuntimeNotice,
  streamDisplayModeAvailable,
  stripClientSettingsResponseOnly,
  stripConfigResponseOnly,
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

  it('prefers explicit linux_stream_mode over legacy booleans', () => {
    expect(resolveStreamDisplayMode({
      linux_stream_mode: 'desktop_display',
      headless_mode: 'enabled',
      linux_use_cage_compositor: 'enabled',
    })).toBe('desktop_display')
  })

  it('maps private runtimes on apply while catalog-less availability stays fail closed', () => {
    expect(streamDisplayModeAvailable('gamescope_stream')).toBe(false)
    expect(streamDisplayModeAvailable('desktop_display')).toBe(true)
    expect(streamDisplayModeAvailable('family_isolated')).toBe(false)
    expect(streamDisplayModeAvailable('headless_evdi')).toBe(false)
    expect(labelForStreamDisplayMode('gamescope_stream')).toBe('Gamescope Stream')
    expect(labelForStreamDisplayMode('headless_dongle')).toBe('Headless Dongle')

    const labwc = applyStreamDisplayModeToConfig({}, 'headless_stream')
    expect(labwc.linux_stream_mode).toBe('headless_stream')
    expect(labwc.linux_private_runtime).toBe('labwc')
    expect(labwc.headless_mode).toBe('enabled')
    expect(labwc.linux_use_cage_compositor).toBe('enabled')

    const gamescope = applyStreamDisplayModeToConfig({}, 'gamescope_stream')
    expect(gamescope.linux_stream_mode).toBe('gamescope_stream')
    expect(gamescope.linux_private_runtime).toBe('gamescope')
    expect(gamescope.linux_use_cage_compositor).toBe('disabled')
    expect(gamescope.capture).toBe('portal')

    const dongle = applyStreamDisplayModeToConfig({}, 'headless_dongle')
    expect(dongle.linux_stream_mode).toBe('headless_dongle')
    expect(dongle.capture).toBe('portal')
    expect(dongle.linux_auto_manage_displays).toBe('enabled')
    // Explicit kms is preserved for CAP_SYS_ADMIN hosts.
    const dongleKms = applyStreamDisplayModeToConfig({ capture: 'kms' }, 'headless_dongle')
    expect(dongleKms.capture).toBe('kms')
  })

  it('uses the host mode catalog as launch-mode availability authority', () => {
    const options = [
      {
        value: 'gamescope_stream',
        available: false,
        unavailable_reason: 'Gamescope is not installed on this host.',
      },
      { value: 'desktop_display', available: true },
    ]

    expect(resolveStreamDisplayModeAvailability('gamescope_stream', options)).toEqual({
      available: false,
      unavailableReason: 'Gamescope is not installed on this host.',
    })
    expect(resolveStreamDisplayModeAvailability('desktop_display', options)).toEqual({
      available: true,
      unavailableReason: '',
    })
    expect(resolveStreamDisplayModeAvailability('host_virtual_display', options)).toEqual({
      available: false,
      unavailableReason: 'This mode was not advertised by this host.',
    })

    // Missing authority fails closed for runtime/display mutations; Desktop remains
    // the recovery path because it does not create or rearrange a display.
    expect(resolveStreamDisplayModeAvailability('gamescope_stream', undefined).available).toBe(false)
    expect(resolveStreamDisplayModeAvailability('desktop_display', undefined).available).toBe(true)
  })

  it('normalizes stale capture and topology fields when switching stream paths', () => {
    const gamescope = applyStreamDisplayModeToConfig({
      capture: 'kms',
      linux_auto_manage_displays: 'enabled',
      headless_swap_mode: 'privacy',
    }, 'gamescope_stream')
    expect(gamescope.capture).toBe('portal')
    expect(gamescope.linux_private_runtime).toBe('gamescope')
    expect(gamescope.linux_auto_manage_displays).toBe('disabled')
    expect(gamescope.headless_swap_mode).toBe('')

    const labwc = applyStreamDisplayModeToConfig({
      capture: 'portal',
      linux_auto_manage_displays: 'enabled',
      headless_swap_mode: 'privacy',
    }, 'headless_stream')
    expect(labwc.capture).toBe('wlr')
    expect(labwc.linux_private_runtime).toBe('labwc')
    expect(labwc.linux_auto_manage_displays).toBe('disabled')
    expect(labwc.headless_swap_mode).toBe('')

    const desktop = applyStreamDisplayModeToConfig({
      capture: 'kms',
      linux_private_runtime: 'gamescope',
      linux_auto_manage_displays: 'enabled',
      headless_swap_mode: 'privacy',
    }, 'desktop_display')
    expect(desktop.capture).toBe('portal')
    expect(desktop.linux_private_runtime).toBe('')
    expect(desktop.linux_auto_manage_displays).toBe('disabled')
    expect(desktop.headless_swap_mode).toBe('')
  })

  it('labels GPU-native as a Private Stream capture capability', () => {
    const sync = resolveClientSettingsSync({
      client_settings_available: true,
      client_settings_stream_display_mode: 'windowed_stream',
    })

    expect(sync.desiredModeLabel).toBe('Private Stream (GPU-native)')
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
    expect(sync.desiredModeLabel).toBe('Private Stream')
    expect(sync.effectiveModeLabel).toBe('Mirror Desktop')
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
    expect(pending.copy).toContain('Private Stream')
    expect(pending.copy).toContain('Private Stream (GPU-native)')
  })

  it('does not show pending relaunch when backend restart flag is stale but modes match', () => {
    const notice = resolveStreamDisplayRuntimeNotice(
      resolveClientSettingsSync({
        client_settings_available: true,
        client_settings_relaunch_required: true,
        client_settings_stream_display_mode: 'windowed_stream',
        client_settings_effective_stream_display_mode: 'windowed_stream',
      }),
      'windowed_stream'
    )

    expect(notice.state).toBe('synced')
    expect(notice.copy).not.toContain('Pending relaunch')
  })

  it('keeps unknown stream display modes unlabeled so server labels can win', () => {
    expect(labelForStreamDisplayMode('future_stream_mode')).toBe('')

    const notice = resolveStreamDisplayRuntimeNotice(
      resolveClientSettingsSync({
        client_settings_available: true,
        client_settings_stream_display_mode: 'future_stream_mode',
        client_settings_stream_display_mode_label: 'Future Stream',
      }),
      'future_stream_mode'
    )

    expect(notice.copy).toContain('Future Stream saved')
    expect(notice.copy).not.toContain('Desktop Display saved')
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
    expect(notice.copy).toContain('Private Stream (GPU-native) saved')
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
    expect(notice.copy).toContain('Private Stream (GPU-native) selected')
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

  it('strips all GET-only runtime metadata before a config POST', () => {
    const config = stripConfigResponseOnly({
      headless_mode: 'enabled',
      linux_stream_mode: 'headless_stream',
      status: true,
      platform: 'linux',
      version: '1.3.2',
      has_ai_api_key: true,
      has_steamgriddb_api_key: true,
      has_api_key: true,
      vdisplayStatus: 1,
      vdisplayAvailable: true,
      vdisplayBackend: 'evdi',
      runtime_backend: 'Labwc',
      runtime_requested_headless: true,
      runtime_effective_headless: true,
      runtime_gpu_native_override_active: false,
      stream_display_mode: 'Private Stream',
      stream_display_mode_options: [{ value: 'gamescope_stream', available: false }],
      stream_path_id: 'headless_stream',
      stream_path_label: 'Private Stream',
      client_settings_available: true,
    })

    expect(config).toEqual({
      headless_mode: 'enabled',
      linux_stream_mode: 'headless_stream',
    })
  })
})

describe('response-only keys from the host', () => {
  it('prefers the list the host serves and always includes the carrier key', () => {
    const served = ['status', 'platform', 'stream_path_id', 'brand_new_key']
    const keys = resolveConfigResponseOnlyKeys({ config_response_only_keys: served })

    expect(keys).toEqual([...served, 'config_response_only_keys'])
    const config = stripConfigResponseOnly({
      config_response_only_keys: served,
      brand_new_key: 'server says drop me',
      status: true,
      max_bitrate: 20000,
    })
    expect(config).toEqual({ max_bitrate: 20000 })
  })

  it('falls back to the mirror for hosts that predate the served list', () => {
    expect(resolveConfigResponseOnlyKeys({})).toBe(CONFIG_RESPONSE_ONLY_KEYS)
    expect(resolveConfigResponseOnlyKeys({ config_response_only_keys: 'not a list' })).toBe(CONFIG_RESPONSE_ONLY_KEYS)
    expect(CONFIG_RESPONSE_ONLY_KEYS).toContain('config_response_only_keys')
  })
})
