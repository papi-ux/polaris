import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'

import CodecSupportPanel from './CodecSupportPanel.vue'

function mountPanel(config) {
  return mount(CodecSupportPanel, {
    props: { config },
    global: {
      mocks: {
        $t: (key) => key,
      },
    },
  })
}

describe('CodecSupportPanel', () => {
  it('renders nothing when the host predates the capability snapshot', () => {
    const wrapper = mountPanel({})
    expect(wrapper.text()).toBe('')
    expect(wrapper.find('.surface-subtle').exists()).toBe(false)
    wrapper.unmount()
  })

  it('shows the probing state until the encoder probe completes', () => {
    const wrapper = mountPanel({
      encoder_codec_support: { ready: false, encoder: '', hevc_supported: false, av1_supported: false },
    })
    expect(wrapper.text()).toContain('config.codec_support_probing')
    expect(wrapper.text()).not.toContain('config.codec_support_unsupported')
    wrapper.unmount()
  })

  it('lists supported codecs with HDR badges once the probe passes', () => {
    const wrapper = mountPanel({
      encoder_codec_support: {
        ready: true,
        encoder: 'vulkan',
        hevc_supported: true,
        av1_supported: true,
        hevc_hdr: true,
        av1_hdr: false,
      },
    })
    expect(wrapper.text()).toContain('config.codec_support_title')
    expect(wrapper.text()).toContain('vulkan')
    // H.264 is always advertised; HEVC and AV1 follow the probe result.
    const supported = wrapper.text().match(/config\.codec_support_supported/g) || []
    expect(supported.length).toBe(3)
    expect(wrapper.text()).toContain('config.codec_support_hdr')
    expect(wrapper.text()).not.toContain('config.codec_support_reason_disabled')
    expect(wrapper.text()).not.toContain('config.codec_support_reason_unavailable')
    wrapper.unmount()
  })

  it('explains a codec that is disabled in configuration', () => {
    const wrapper = mountPanel({
      encoder_codec_support: {
        ready: true,
        encoder: 'vaapi',
        hevc_supported: false,
        av1_supported: true,
        hevc_reason: 'disabled_in_config',
        av1_reason: null,
      },
    })
    expect(wrapper.text()).toContain('config.codec_support_unsupported')
    expect(wrapper.text()).toContain('config.codec_support_reason_disabled')
    expect(wrapper.text()).not.toContain('config.codec_support_reason_unavailable')
    wrapper.unmount()
  })

  it('explains a codec the encoder cannot do', () => {
    const wrapper = mountPanel({
      encoder_codec_support: {
        ready: true,
        encoder: 'vaapi',
        hevc_supported: true,
        av1_supported: false,
        hevc_reason: null,
        av1_reason: 'not_available_on_encoder',
      },
    })
    expect(wrapper.text()).toContain('config.codec_support_unsupported')
    expect(wrapper.text()).toContain('config.codec_support_reason_unavailable')
    expect(wrapper.text()).not.toContain('config.codec_support_reason_disabled')
    wrapper.unmount()
  })

  it('shows no reason line while probing is incomplete even when a codec is off', () => {
    const wrapper = mountPanel({
      encoder_codec_support: {
        ready: false,
        encoder: '',
        hevc_supported: false,
        av1_supported: false,
        hevc_reason: null,
        av1_reason: null,
      },
    })
    expect(wrapper.text()).toContain('config.codec_support_probing')
    expect(wrapper.text()).not.toContain('config.codec_support_reason_disabled')
    expect(wrapper.text()).not.toContain('config.codec_support_reason_unavailable')
    wrapper.unmount()
  })
})
