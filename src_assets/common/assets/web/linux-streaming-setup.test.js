import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick, reactive, ref } from 'vue'

import AudioVideo from './configs/tabs/AudioVideo.vue'

const enMessages = JSON.parse(readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'),
  'utf8',
))

// en.json-backed stub: the behavioral assertions below keep reading the real
// rendered English copy, resolved the same way vue-i18n resolves it at runtime.
// Unknown keys fall back to the key itself, matching the old stub.
const i18n = {
  t(key, params = {}) {
    const value = key.split('.').reduce(
      (node, part) => (node && typeof node === 'object' ? node[part] : undefined),
      enMessages,
    )
    if (typeof value !== 'string') {
      return key
    }
    return value.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
  },
}

function linuxConfig(overrides = {}) {
  return {
    adapter_name: '',
    output_name: '',
    headless_mode: 'enabled',
    linux_use_cage_compositor: 'enabled',
    linux_prefer_gpu_native_capture: 'disabled',
    linux_capture_profile: 'disabled',
    adaptive_bitrate_enabled: 'disabled',
    adaptive_bitrate_min: 2000,
    adaptive_bitrate_max: 100000,
    ai_enabled: 'disabled',
    max_bitrate: 0,
    client_settings_available: true,
    stream_display_mode_options: [
      'headless_stream',
      'windowed_stream',
      'gamescope_stream',
      'host_virtual_display',
      'headless_dongle',
      'desktop_display',
    ].map((value) => ({ value, available: true })),
    ...overrides,
  }
}

function mountAudioVideo(config = linuxConfig()) {
  return shallowMount(AudioVideo, {
    props: {
      platform: 'linux',
      config,
      vdisplay: '0',
    },
    global: {
      provide: { i18n, platform: ref('linux') },
      stubs: {
        AdapterNameSelector: true,
        DisplayOutputSelector: true,
        DisplayDeviceOptions: true,
        VirtualDisplayStatus: true,
        Checkbox: true,
        PlatformLayout: { template: '<div><slot name="linux" /></div>', props: ['platform'] },
        // Shared presentational leaves render for real: the assertions below
        // click their buttons and read their text as an operator would.
        SelectableCard: false,
        StatTile: false,
      },
    },
  })
}

describe('Linux Streaming Setup checklist', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('renders the en.json copy for strings routed through the locale system', () => {
    const wrapper = mountAudioVideo()
    const text = wrapper.text()

    // Representative keys across the moved families: mode cards, checklist,
    // Auto Quality, planned modes, and the display planner. Each value must
    // exist in en.json and appear verbatim in the rendered page.
    for (const key of [
      'av_mode_headless_stream_copy',
      'av_checklist_path_title',
      'av_auto_quality_badge_manual',
      'av_planned_family_mode_copy',
      'av_planner_moonlight_title',
    ]) {
      const value = enMessages.config[key]
      expect(typeof value, `config.${key} must exist in en.json`).toBe('string')
      expect(text, `config.${key}`).toContain(value)
    }
  })

  it('guides desktop Linux operators through display pairing, Auto Quality, and AMD/VAAPI capture checks', () => {
    const wrapper = mountAudioVideo()
    const checklist = wrapper.find('[data-linux-streaming-setup]')

    expect(checklist.exists()).toBe(true)
    expect(checklist.text()).toContain('Linux Streaming Setup')
    expect(checklist.text()).toContain('Pick a stream path')
    expect(checklist.text()).toContain('Encoder and quality')
    expect(checklist.text()).toContain('Manual')
    expect(checklist.text()).toContain('labwc GPU-native capture')
    expect(checklist.text()).toContain('Safe default')
  })

  it('puts player impact first and keeps backend vocabulary in one selected-path summary', () => {
    const wrapper = mountAudioVideo()
    const picker = wrapper.find('[data-stream-display-mode-picker]')
    const privateMode = picker.findAll('article').find((card) => card.text().includes('Private Stream'))
    const dongleMode = picker.findAll('article').find((card) => card.text().includes('Headless Dongle'))

    expect(wrapper.text()).toContain('Where games run')
    expect(wrapper.text()).toContain('Polaris chooses the capture method automatically')
    expect(privateMode).toBeDefined()
    expect(privateMode.find('button').text()).toContain('without touching the host monitors')
    expect(privateMode.find('button').text()).not.toContain('labwc')
    expect(privateMode.find('details').exists()).toBe(false)
    expect(wrapper.find('[data-selected-stream-path]').text()).toContain('Current path')
    expect(wrapper.find('[data-selected-stream-path]').text()).toContain('Runtime: labwc')
    expect(wrapper.findAll('[data-selected-stream-path]')).toHaveLength(1)
    expect(dongleMode.find('button').text()).toContain('Privacy mode blanks the real panel after the one-time portal approval is saved')
    expect(dongleMode.find('button').text()).toContain('the panel stays on during that approval')
    expect(dongleMode.find('button').text()).toContain('Off mode keeps the real panel active')
    expect(wrapper.find('[data-capture-path-explainer]').text()).toContain('System-memory capture copies through RAM')
  })

  it('keeps planned modes visible without adding selectable launch buttons', () => {
    const wrapper = mountAudioVideo()
    const pickerButtons = wrapper.find('[data-stream-display-mode-picker]').findAll('button')
    const planned = wrapper.find('[data-planned-stream-modes]')

    expect(pickerButtons).toHaveLength(7)
    expect(pickerButtons.some((button) => button.text().includes('Desktop Takeover'))).toBe(true)
    expect(planned.text()).toContain('Family Mode (isolated)')
    expect(planned.text()).toContain('Headless EVDI')
    expect(pickerButtons.some((button) => button.text().includes('Family Mode'))).toBe(false)
    expect(pickerButtons.some((button) => button.text().includes('Headless EVDI'))).toBe(false)
  })

  it('greys out modes the host reports unavailable and preserves the host reason', async () => {
    const config = linuxConfig({
      linux_stream_mode: 'desktop_display',
      stream_display_mode_options: [
        { value: 'headless_stream', available: true },
        { value: 'windowed_stream', available: true },
        {
          value: 'gamescope_stream',
          available: false,
          unavailable_reason: 'Gamescope is not installed on this host.',
        },
        { value: 'host_virtual_display', available: true },
        { value: 'headless_dongle', available: true },
        { value: 'desktop_display', available: true },
      ],
    })
    const wrapper = mountAudioVideo(config)
    const gamescope = wrapper.findAll('article').find((card) => card.text().includes('Gamescope Stream'))

    expect(gamescope).toBeDefined()
    expect(gamescope.find('button').attributes('disabled')).toBeDefined()
    expect(gamescope.text()).toContain('Unavailable: Gamescope is not installed on this host.')
    await gamescope.find('button').trigger('click')
    expect(config.linux_stream_mode).toBe('desktop_display')
  })

  it('reflects selected display pairing and GPU-native copy intent', () => {
    const wrapper = mountAudioVideo(linuxConfig({
      adapter_name: 'NVIDIA RTX 4090',
      output_name: 'DP-1',
      linux_prefer_gpu_native_capture: 'enabled',
      ai_enabled: 'enabled',
      adaptive_bitrate_enabled: 'enabled',
    }))
    const text = wrapper.find('[data-linux-streaming-setup]').text()

    expect(text).toContain('Private Stream (GPU-native)')
    expect(text).toContain('GPU-native requested')
    expect(text).toContain('Auto Quality: On')
    expect(text).toContain('DMA-BUF capture GPU-resident')
    expect(text).not.toContain('CUDA')
    expect(text).not.toContain('NVIDIA')
  })

  it('warns NVIDIA true-headless hosts when GPU-native capture is disabled', () => {
    const wrapper = mountAudioVideo(linuxConfig({
      encoder: 'nvenc',
      headless_mode: 'enabled',
      linux_use_cage_compositor: 'enabled',
      linux_prefer_gpu_native_capture: 'disabled',
    }))
    const text = wrapper.find('[data-linux-streaming-setup]').text()

    expect(text).toContain('NVIDIA true-headless guard')
    expect(text).toContain('Needs GPU-native preference')
    expect(text).toContain('cold-cache 503')
    expect(text).toContain('enable the preference')
    expect(text).toContain('Private Stream (GPU-native)')
  })

  it('applies normalized empty fields when leaving the dongle preset', async () => {
    const config = linuxConfig({
      linux_stream_mode: 'headless_dongle',
      linux_private_runtime: 'gamescope',
      capture: 'kms',
      linux_auto_manage_displays: 'enabled',
      headless_swap_mode: 'privacy',
    })
    const wrapper = mountAudioVideo(config)
    const privateStream = wrapper.findAll('button').find((button) => {
      const text = button.text()
      return text.includes('Private Stream') && !text.includes('GPU-native')
    })

    expect(privateStream).toBeDefined()
    await privateStream.trigger('click')

    expect(config.linux_stream_mode).toBe('headless_stream')
    expect(config.linux_private_runtime).toBe('labwc')
    expect(config.capture).toBe('wlr')
    expect(config.linux_auto_manage_displays).toBe('disabled')
    expect(config.headless_swap_mode).toBe('')
  })

  it('keeps display planner presets idempotent across repeated clicks', async () => {
    const config = reactive(linuxConfig({ fallback_mode: '1920x1080x60' }))
    const wrapper = mountAudioVideo(config)
    const buttons = wrapper.findAll('button')
    const balanced = buttons.find((button) => button.text().includes('Balanced'))
    const native = buttons.find((button) => button.text().includes('Native'))

    expect(balanced).toBeDefined()
    expect(native).toBeDefined()
    await balanced.trigger('click')
    expect(config.fallback_mode).toBe('1440x810x60')
    await balanced.trigger('click')
    expect(config.fallback_mode).toBe('1440x810x60')
    await native.trigger('click')
    expect(config.fallback_mode).toBe('1920x1080x60')

    await balanced.trigger('click')
    expect(config.fallback_mode).toBe('1440x810x60')
    await wrapper.find('#fallback_mode').setValue('1440x810x60')
    const manuallyConfirmedNative = wrapper.findAll('button').find((button) => button.text().includes('Native'))
    await manuallyConfirmedNative.trigger('click')
    expect(config.fallback_mode).toBe('1440x810x60')

    await wrapper.find('#fallback_mode').setValue('2560x1440x60')
    const updatedBalanced = wrapper.findAll('button').find((button) => button.text().includes('Balanced'))
    await updatedBalanced.trigger('click')
    expect(config.fallback_mode).toBe('1920x1080x60')

    config.fallback_mode = '1600x900x60'
    await nextTick()
    const resetNative = wrapper.findAll('button').find((button) => button.text().includes('Native'))
    await resetNative.trigger('click')
    expect(config.fallback_mode).toBe('1600x900x60')
  })

  it('keeps an initially empty or manually cleared planner source stable', async () => {
    const config = linuxConfig({ fallback_mode: '' })
    const wrapper = mountAudioVideo(config)
    const balanced = () => wrapper.findAll('button').find((button) => button.text().includes('Balanced'))

    await balanced().trigger('click')
    expect(config.fallback_mode).toBe('1440x810x60')
    await balanced().trigger('click')
    expect(config.fallback_mode).toBe('1440x810x60')

    await wrapper.find('#fallback_mode').setValue('')
    await balanced().trigger('click')
    expect(config.fallback_mode).toBe('1440x810x60')
  })

  it('ignores a stale dongle discovery response after another preset is selected', async () => {
    let resolveFetch
    const response = new Promise((resolve) => {
      resolveFetch = resolve
    })
    const fetchMock = vi.fn(() => response)
    vi.stubGlobal('fetch', fetchMock)

    const config = linuxConfig({
      linux_stream_mode: 'desktop_display',
      linux_private_runtime: '',
      capture: 'portal',
      linux_auto_manage_displays: 'disabled',
      headless_swap_mode: '',
    })
    const wrapper = mountAudioVideo(config)
    const buttons = wrapper.findAll('button')
    const dongle = buttons.find((button) => button.text().includes('Headless Dongle'))
    const desktop = buttons.find((button) => button.text().includes('Mirror Desktop'))

    expect(dongle).toBeDefined()
    expect(desktop).toBeDefined()
    await dongle.trigger('click')
    // The page also fetches the settings projection on mount; only the dongle
    // discovery call is under test here.
    const discoveryCalls = () => fetchMock.mock.calls.filter(([url]) => !String(url).includes('settings/metadata'))
    expect(discoveryCalls()).toHaveLength(1)
    await desktop.trigger('click')

    resolveFetch({
      json: async () => ({
        status: true,
        outputs: [{ name: 'HDMI-A-1', connected: true }],
        suggested_streaming_output: 'HDMI-A-1',
        suggested_primary_output: 'DP-1',
      }),
    })
    await Promise.resolve()
    await Promise.resolve()
    await nextTick()

    expect(config.linux_stream_mode).toBe('desktop_display')
    expect(config.linux_auto_manage_displays).toBe('disabled')
    expect(config.headless_swap_mode).toBe('')
    expect(config.linux_streaming_output).toBeUndefined()
    expect(config.linux_primary_output).toBeUndefined()
  })
})
