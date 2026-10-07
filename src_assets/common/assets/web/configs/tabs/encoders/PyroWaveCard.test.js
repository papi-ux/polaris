import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'

import PyroWaveCard from './PyroWaveCard.vue'

// The live readout reads the shared stream stats; the tests hand it a stream and count subscribers.
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

// The console's English, so the assertions say what a host sees.
const enLocale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))
const i18n = {
  t(key, params = {}) {
    const message = key.split('.').reduce((node, part) => node?.[part], enLocale)
    if (typeof message !== 'string') return key
    return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
  },
}

function config(pyrowave) {
  return { encoder_codec_support: { ready: true, encoder: 'nvenc', hevc_supported: true, av1_supported: true, pyrowave } }
}

const AVAILABLE = { available: true, hdr: true, reason: null, message: null, host_mode_refusal: null }

function mountCard(props) {
  return mount(PyroWaveCard, {
    props: { platform: 'linux', ...props },
    global: { provide: { i18n }, mocks: { $t: i18n.t } },
  })
}

function values(wrapper) {
  return Object.fromEntries(wrapper.findAll('[data-pyrowave-value]').map((node) => [node.attributes('data-pyrowave-value'), node.text()]))
}

describe('PyroWaveCard', () => {
  beforeEach(() => {
    streamStats.value = null
    streamStatsUsers.count = 0
  })

  it('renders nothing off Linux or on a host that predates the PyroWave report', () => {
    for (const props of [
      { platform: 'windows', config: config(AVAILABLE) },
      { config: { encoder_codec_support: { ready: true, encoder: 'vaapi' } } },
      { config: {} },
    ]) {
      const wrapper = mountCard(props)
      expect(wrapper.find('[data-pyrowave-card]').exists()).toBe(false)
      wrapper.unmount()
    }
    expect(streamStatsUsers.count).toBe(0)
  })

  it('names PyroWave as available to Nova, chosen in Nova, and reports nothing it does not know', () => {
    const wrapper = mountCard({ config: config(AVAILABLE) })
    const card = wrapper.find('[data-pyrowave-card]')
    expect(card.attributes('id')).toBe('pyrowave')
    expect(card.find('h3').text()).toBe('PyroWave')
    expect(card.find('[data-codec-status]').text()).toBe('Available')
    expect(card.findAll('[data-codec-row="pyrowave"] .meta-pill').map((pill) => pill.text())).toEqual(['4:4:4', 'HDR', 'Available'])
    expect(card.text()).toContain('PyroWave streams only to Nova. Other clients are not offered it and stream H.264, HEVC or AV1.')
    expect(card.find('[data-pyrowave-selection]').text()).toBe('Nova chooses PyroWave and its bitrate for each stream. This card only reports what the host can do and what a stream used; it changes nothing.')
    expect(card.find('[data-pyrowave-reason]').exists()).toBe(false)
    expect(card.find('[data-pyrowave-refusal]').exists()).toBe(false)
    // No host tuning knobs: the card only reports.
    expect(card.findAll('input, select, button, textarea')).toHaveLength(0)
    // Stats not arrived yet read as unknown, not as no stream.
    expect(card.find('[data-pyrowave-stream-state="unknown"]').text()).toBe('Waiting for stream status from the host.')
    expect(streamStatsUsers.count).toBe(1)
    wrapper.unmount()
  })

  it('keeps the host reason code in details beside its own message when PyroWave is not available', () => {
    const message = "The host's display is scanned out in sixteen bit float, which is how KDE shows HDR, and PyroWave cannot read that format. Turn HDR off on the host's display to use PyroWave, or choose another codec."
    const wrapper = mountCard({ config: config({ available: false, hdr: false, reason: 'fp16_capture', message, host_mode_refusal: null }) })
    expect(wrapper.find('[data-codec-status]').text()).toBe('Not available')
    expect(wrapper.findAll('[data-codec-row="pyrowave"] .meta-pill').map((pill) => pill.text())).toEqual(['Not available'])
    expect(wrapper.find('[data-pyrowave-message]').text()).toBe(message)
    expect(wrapper.find('details [data-pyrowave-reason]').text()).toBe('Reason: fp16_capture')
    // A host that cannot serve PyroWave opens no stream stats for it.
    expect(wrapper.find('[data-pyrowave-stream]').exists()).toBe(false)
    expect(streamStatsUsers.count).toBe(0)
    wrapper.unmount()
  })

  it('says which launch is refused and what the host supports instead', () => {
    const wrapper = mountCard({
      config: config({
        ...AVAILABLE,
        hdr: false,
        host_mode_refusal: {
          code: 'pyrowave_capture_unreadable',
          message: "PyroWave cannot stream this host's desktop.",
          action: "Turn HDR off on the host's display, choose another codec, or use Private Stream, which captures its own session rather than the desktop.",
        },
      }),
    })
    expect(wrapper.find('[data-codec-status]').text()).toBe('Available')
    expect(wrapper.find('[data-pyrowave-refusal]').text()).toBe(
      "It is offered because a stream mode with its own compositor can carry it. A PyroWave launch into this host's own stream mode is refused: PyroWave cannot stream this host's desktop. Turn HDR off on the host's display, choose another codec, or use Private Stream, which captures its own session rather than the desktop.",
    )
    wrapper.unmount()
  })

  it('shows the running stream as labelled values with the model in details', async () => {
    streamStats.value = {
      streaming: true,
      codec: 'pyrowave',
      stream_chroma: '444',
      clients: [{ name: 'Retroid Pocket 6', codec: 'pyrowave', pyrowave_route: 'zero_copy' }],
      pyrowave_bitrate: {
        width: 1920, height: 1080, fps: 60, chroma: '444', target_db: 35, far_target_db: 31,
        advice_far_kbps: 108012, advice_near_kbps: 297507, raise_goal_kbps: 108012, cap_kbps: 300000,
        encoder_kbps: 180000, request_kbps: 201125, ceiling_frame_share: 0.12, starved: false, rule: 'model', raise_goal_limited_by: 'advice',
        request_cap: null, cap_set_aside: null, assumes: { fec_percentage: 10, audio_kbps: 512 },
      },
    }
    const wrapper = mountCard({ config: config(AVAILABLE) })
    await nextTick()
    const stream = wrapper.find('[data-pyrowave-stream]')
    expect(stream.find('[data-pyrowave-stream-heading]').text()).toBe('PyroWave stream running: 1920x1080 at 60 fps, 4:4:4.')
    expect(stream.find('[data-pyrowave-stream-heading]').attributes('data-pyrowave-stream-state')).toBe('running')
    expect(values(stream)).toEqual({ requested: '201 Mbps', encoded: '180 Mbps', route: 'Zero copy' })
    expect(stream.text()).toContain('The route says how frames reach the encoder, not how capture produced them.')
    expect(stream.text()).toContain("PyroWave's model for this stream: 109 Mbps on a device's own screen, 298 Mbps on a television or monitor.")
    const details = stream.find('[data-pyrowave-stream-details]')
    expect(details.element.tagName).toBe('DETAILS')
    expect(details.text()).toContain("with 10% FEC and the stream's audio included, for 31 dB of PSNR-HVS-M-H on a device's own screen and 35 dB on a television or monitor")
    expect(details.text()).toContain('Route: zero copy. Captured DMA-BUF frames are imported')
    expect(stream.text()).not.toContain('reads this stream as starved')
    expect(streamStatsUsers.count).toBe(1)
    wrapper.unmount()
  })

  it('says no PyroWave stream runs once the host answers with none, and names the last one when it has it', async () => {
    streamStats.value = { streaming: false, clients: [], last_session: { codec: 'hevc' } }
    const idle = mountCard({ config: config(AVAILABLE) })
    await nextTick()
    expect(idle.find('[data-pyrowave-stream-state="idle"]').text()).toBe('No PyroWave stream is running, and the host reports no last PyroWave stream.')
    expect(idle.find('[data-pyrowave-value]').exists()).toBe(false)
    idle.unmount()

    streamStats.value = {
      streaming: false,
      clients: [],
      last_session: { state: 'ended', client_name: 'Retroid Pocket 6', codec: 'pyrowave', pyrowave_route: 'gpu_upload' },
    }
    const last = mountCard({ config: config(AVAILABLE) })
    await nextTick()
    const heading = last.find('[data-pyrowave-stream-heading]')
    expect(heading.text()).toBe('Last PyroWave stream, Retroid Pocket 6')
    expect(heading.attributes('data-pyrowave-stream-state')).toBe('last')
    // A finished stream has no live rate, so none is shown, not even as 0.
    expect(values(last)).toEqual({ route: 'GPU upload' })
    last.unmount()
  })
})
