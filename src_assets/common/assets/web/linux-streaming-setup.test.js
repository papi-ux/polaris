import { shallowMount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'
import { ref } from 'vue'

import AudioVideo from './configs/tabs/AudioVideo.vue'

const i18n = {
  t(key) {
    return key
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
      },
    },
  })
}

describe('Linux Streaming Setup checklist', () => {
  it('guides desktop Linux operators through display pairing, Auto Quality, and Wayland/NVIDIA copy checks', () => {
    const wrapper = mountAudioVideo()
    const checklist = wrapper.find('[data-linux-streaming-setup]')

    expect(checklist.exists()).toBe(true)
    expect(checklist.text()).toContain('Linux Streaming Setup')
    expect(checklist.text()).toContain('Pair encoder and display')
    expect(checklist.text()).toContain('Discover first')
    expect(checklist.text()).toContain('Decide Auto Quality')
    expect(checklist.text()).toContain('Manual')
    expect(checklist.text()).toContain('Check Wayland / NVIDIA copy risk')
    expect(checklist.text()).toContain('Safe default')
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

    expect(text).toContain('Selected')
    expect(text).toContain('GPU-native requested')
    expect(text).toContain('Enabled')
    expect(text).toContain('avoid the SHM/RAM copy path')
  })
})
