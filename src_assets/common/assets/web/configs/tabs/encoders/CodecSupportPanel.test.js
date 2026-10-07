import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it, vi } from 'vitest'

import CodecSupportPanel from './CodecSupportPanel.vue'
import NvidiaNvencEncoder from './NvidiaNvencEncoder.vue'
import SoftwareEncoder from './SoftwareEncoder.vue'

// The live PyroWave readout reads the shared stream stats; the tests hand it a stream instead.
const { streamStats, streamStatsUsers } = vi.hoisted(() => ({ streamStats: { value: null }, streamStatsUsers: { count: 0 } }))
vi.mock('../../../composables/useStreamStats', async () => {
  const { ref } = await import('vue')
  const stats = ref(null)
  return {
    useStreamStats: () => {
      streamStatsUsers.count += 1
      stats.value = streamStats.value
      return { stats, connected: ref(true) }
    },
  }
})

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

// The rows below read the console's English, so the assertions say what a host sees.
const enLocale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))
const i18n = {
  t(key, params = {}) {
    const message = key.split('.').reduce((node, part) => node?.[part], enLocale)
    if (typeof message !== 'string') return key
    return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
  },
}

function mountReadout(config) {
  return mount(CodecSupportPanel, {
    props: { config },
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t },
    },
  })
}

const LINUX_YUV444 = {
  h264: false,
  hevc: false,
  av1: false,
  encoders: [
    { encoder: 'nvenc', codecs: [] },
    { encoder: 'vulkan', codecs: [] },
    { encoder: 'vaapi', codecs: [] },
    { encoder: 'software', codecs: ['h264'] },
  ],
}

function hostSupport(overrides = {}) {
  return {
    encoder_codec_support: {
      ready: true,
      encoder: 'nvenc',
      hevc_supported: true,
      av1_supported: true,
      hevc_hdr: true,
      av1_hdr: true,
      yuv444: LINUX_YUV444,
      pyrowave: { available: true, hdr: true, reason: null, message: null, host_mode_refusal: null },
      ...overrides,
    },
  }
}

function row(wrapper, name) {
  const found = wrapper.find(`[data-codec-row="${name}"]`)
  if (!found.exists()) return null
  return {
    status: found.find('[data-codec-status]').text(),
    pills: found.findAll('.meta-pill').map((pill) => pill.text()),
    detail: wrapper.find(`[data-codec-detail="${name}"]`).text(),
  }
}

describe('CodecSupportPanel 4:4:4 and PyroWave rows', () => {
  beforeEach(() => {
    streamStats.value = null
    streamStatsUsers.count = 0
  })

  it('says which encoder can give 4:4:4 on a Linux host with PyroWave', () => {
    const wrapper = mountReadout(hostSupport())
    const yuv444 = row(wrapper, 'yuv444')
    expect(yuv444.status).toBe('PyroWave only')
    expect(yuv444.detail).toContain('The active encoder streams 4:2:0 only, so it offers clients no 4:4:4.')
    expect(yuv444.detail).toContain('only H.264 from the software encoder on the CPU can deliver 4:4:4. NVENC, Vulkan Video and VA-API stream 4:2:0.')
    expect(yuv444.detail).toContain('PyroWave also carries 4:4:4 on this host, on the GPU, and streams only to Nova.')
    wrapper.unmount()
  })

  it('marks 4:4:4 supported when the active encoder offers it', () => {
    const wrapper = mountReadout(hostSupport({ encoder: 'software', yuv444: { ...LINUX_YUV444, h264: true } }))
    expect(row(wrapper, 'yuv444').status).toBe('Supported')
    expect(row(wrapper, 'yuv444').detail).toContain('The active encoder offers clients 4:4:4 in H.264.')
    wrapper.unmount()
  })

  it('says 4:4:4 is not supported when neither the encoder nor PyroWave can give it', () => {
    const wrapper = mountReadout(hostSupport({
      pyrowave: { available: false, hdr: false, reason: 'no_vulkan_device', message: 'The host has no GPU that can run PyroWave.', host_mode_refusal: null },
    }))
    expect(row(wrapper, 'yuv444').status).toBe('Not supported')
    expect(row(wrapper, 'yuv444').detail).toContain('PyroWave, which also carries 4:4:4, is not available on this host.')
    wrapper.unmount()
  })

  it('waits for the encoder probe before the 4:4:4 row', () => {
    const wrapper = mountReadout(hostSupport({ ready: false }))
    expect(wrapper.text()).toContain('Polaris is still validating this host')
    expect(row(wrapper, 'yuv444')).toBeNull()
    wrapper.unmount()
  })

  // PyroWave has one card above the encoder switcher (PyroWaveCard.test.js); a profile repeats none of it.
  it('leaves PyroWave to its own card and opens no stream stats', () => {
    streamStats.value = { streaming: true, codec: 'pyrowave', clients: [{ name: 'Retroid Pocket 6', codec: 'pyrowave' }] }
    const wrapper = mountReadout(hostSupport())
    expect(row(wrapper, 'pyrowave')).toBeNull()
    expect(wrapper.find('[data-pyrowave-stream]').exists()).toBe(false)
    expect(wrapper.find('[data-pyrowave-refusal]').exists()).toBe(false)
    expect(streamStatsUsers.count).toBe(0)
    // The 4:4:4 row still says what PyroWave adds, since that is a fact about 4:4:4 on this host.
    expect(row(wrapper, 'yuv444').detail).toContain('PyroWave also carries 4:4:4 on this host')
    wrapper.unmount()
  })

  it('renders neither row for a host that predates them', () => {
    const wrapper = mountReadout({
      encoder_codec_support: { ready: true, encoder: 'vaapi', hevc_supported: true, av1_supported: false },
    })
    expect(row(wrapper, 'yuv444')).toBeNull()
    expect(row(wrapper, 'pyrowave')).toBeNull()
    expect(wrapper.text()).toContain('Advertised codec support')
    wrapper.unmount()
  })
})

describe('Encoder tabs that carry the codec readout', () => {
  // An NVIDIA host never saw the readout while it lived only on the VA-API and Vulkan Video tabs.
  // Off Linux the panel stays away: PyroWave's not_built message would name a release that does not
  // exist there.
  for (const [name, component] of [['NVENC', NvidiaNvencEncoder], ['Software', SoftwareEncoder]]) {
    it(`shows it on the ${name} tab on Linux only`, () => {
      const mountTab = (platform) => mount(component, {
        props: { platform, config: hostSupport() },
        global: { provide: { i18n }, mocks: { $t: i18n.t }, stubs: { Checkbox: true } },
      })
      const linux = mountTab('linux')
      expect(linux.text()).toContain('Advertised codec support')
      expect(row(linux, 'yuv444').status).toBe('PyroWave only')
      expect(row(linux, 'pyrowave')).toBeNull()
      linux.unmount()

      const windows = mountTab('windows')
      expect(windows.text()).not.toContain('Advertised codec support')
      expect(row(windows, 'pyrowave')).toBeNull()
      windows.unmount()
    })
  }
})
