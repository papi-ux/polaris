import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

import { describePyroWave, describePyroWaveStream, describeYuv444, listPhrase, mbps } from './codec-support-readout.js'

const enLocale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))

// The console's English, so an assertion reads what a host sees and a missing key shows as itself.
function t(key, params = {}) {
  const message = key.split('.').reduce((node, part) => node?.[part], enLocale)
  if (typeof message !== 'string') return key
  return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
}

// What a Linux build serves: no GPU encoder table carries 4:4:4, the software encoder does for H.264.
const LINUX_ENCODERS = [
  { encoder: 'nvenc', codecs: [] },
  { encoder: 'vulkan', codecs: [] },
  { encoder: 'vaapi', codecs: [] },
  { encoder: 'software', codecs: ['h264'] },
]

const AVAILABLE = { available: true, hdr: false, reason: null, message: null, host_mode_refusal: null }
const NOT_BUILT = {
  available: false,
  hdr: false,
  reason: 'not_built',
  message: 'This Polaris build has no PyroWave encoder. Install a Polaris release that includes PyroWave on the host to use it.',
  host_mode_refusal: null,
}

function support({ yuv444 = { h264: false, hevc: false, av1: false, encoders: LINUX_ENCODERS }, pyrowave = AVAILABLE } = {}) {
  return { ready: true, encoder: 'nvenc', hevc_supported: true, av1_supported: true, yuv444, pyrowave }
}

// Every string the readout writes, in one place, so the style rules are checked on all of it.
function everyLine(row) {
  return [row?.heading, row?.message, row?.refusal?.lead, ...(row?.routes || []), ...(row?.lines || [])].filter(Boolean)
}

function expectHouseStyle(lines) {
  for (const line of lines) {
    expect(line).not.toMatch(/[\u2013\u2014]/)
    expect(line).not.toContain(' - ')
    expect(line).not.toContain('config.codec_support_')
    expect(line).not.toContain('Play Setup')
  }
}

describe('describeYuv444', () => {
  it('says plainly that on Linux only software H.264 and PyroWave carry 4:4:4', () => {
    const row = describeYuv444(t, support())
    expect(row.status).toBe('pyrowave_only')
    expect(row.lines).toEqual([
      'The active encoder streams 4:2:0 only, so it offers clients no 4:4:4.',
      'Of the encoders Polaris can choose in this build, only H.264 from the software encoder on the CPU can deliver 4:4:4. NVENC, Vulkan Video and VA-API stream 4:2:0.',
      'PyroWave also carries 4:4:4 on this host, on the GPU, and streams only to Nova.',
    ])
    expectHouseStyle(row.lines)
  })

  it('names the codecs the active encoder offers in 4:4:4', () => {
    const row = describeYuv444(t, support({ yuv444: { h264: true, hevc: false, av1: false, encoders: LINUX_ENCODERS } }))
    expect(row.status).toBe('supported')
    expect(row.codecs).toEqual(['h264'])
    expect(row.lines[0]).toBe('The active encoder offers clients 4:4:4 in H.264.')
  })

  it('says 4:4:4 is out of reach when neither the encoder nor PyroWave can give it', () => {
    const row = describeYuv444(t, support({ pyrowave: NOT_BUILT }))
    expect(row.status).toBe('unsupported')
    expect(row.lines).toContain('PyroWave, which also carries 4:4:4, is not available on this host.')
    expect(row.lines.join(' ')).not.toContain('PyroWave also carries 4:4:4')
  })

  it('words a build whose encoders carry nothing, or everything, in 4:4:4', () => {
    const none = describeYuv444(t, support({ yuv444: { h264: false, hevc: false, av1: false, encoders: [{ encoder: 'vaapi', codecs: [] }] } }))
    expect(none.lines).toContain('No encoder Polaris can choose in this build delivers 4:4:4.')
    const all = describeYuv444(t, support({
      yuv444: { h264: true, hevc: true, av1: false, encoders: [{ encoder: 'nvenc', codecs: ['h264', 'hevc', 'av1'] }] },
    }))
    expect(all.lines).toContain('In this build 4:4:4 can come from H.264, HEVC and AV1 from NVENC.')
    expect(all.lines[0]).toBe('The active encoder offers clients 4:4:4 in H.264 and HEVC.')
  })

  it('stays silent on a host that predates the 4:4:4 report', () => {
    expect(describeYuv444(t, { ready: true, encoder: 'vaapi' })).toBeNull()
    expect(describeYuv444(t, null)).toBeNull()
  })

  it('leaves PyroWave out of the row when the host does not report it', () => {
    // null, not undefined: undefined would take the helper's default, an available PyroWave.
    const row = describeYuv444(t, support({ pyrowave: null }))
    expect(row.status).toBe('unsupported')
    expect(row.lines.join(' ')).not.toContain('PyroWave')
  })
})

describe('describePyroWave', () => {
  it('says an available PyroWave streams only to Nova', () => {
    const row = describePyroWave(t, support())
    expect(row.available).toBe(true)
    expect(row.hdr).toBe(false)
    expect(row.lines).toEqual(['PyroWave streams only to Nova. Other clients are not offered it and stream H.264, HEVC or AV1.'])
    expect(row.reason).toBe('')
    expect(row.message).toBe('')
    expect(row.refusal).toBeNull()
    expect(describePyroWave(t, support({ pyrowave: { ...AVAILABLE, hdr: true } })).hdr).toBe(true)
  })

  it('shows the host reason and message, as the host words them, when PyroWave is not available', () => {
    const row = describePyroWave(t, support({ pyrowave: NOT_BUILT }))
    expect(row.available).toBe(false)
    expect(row.reason).toBe('not_built')
    expect(row.message).toBe(NOT_BUILT.message)
    expect(row.refusal).toBeNull()

    const fp16 = describePyroWave(t, support({
      pyrowave: {
        available: false,
        hdr: true,
        reason: 'fp16_capture',
        message: "The host's display is scanned out in sixteen bit float, which is how KDE shows HDR, and PyroWave cannot read that format. Turn HDR off on the host's display to use PyroWave, or choose another codec.",
        host_mode_refusal: null,
      },
    }))
    expect(fp16.reason).toBe('fp16_capture')
    expect(fp16.message).toContain('sixteen bit float')
    // HDR belongs to a codec the host can run at all.
    expect(fp16.hdr).toBe(false)
    expectHouseStyle(everyLine(fp16))
  })

  it('says the host gave no reason rather than rendering an empty line', () => {
    const row = describePyroWave(t, support({ pyrowave: { available: false, reason: null, message: '' } }))
    expect(row.message).toBe('The host did not say why PyroWave is not available.')
  })

  it('says which launch is refused when PyroWave is offered only to modes with their own compositor', () => {
    const row = describePyroWave(t, support({
      pyrowave: {
        ...AVAILABLE,
        host_mode_refusal: {
          code: 'pyrowave_capture_unreadable',
          message: "PyroWave cannot stream this host's desktop.",
          action: 'Turn HDR off on the host\'s display, choose another codec, or use Private Stream, which captures its own session rather than the desktop.',
        },
      },
    }))
    expect(row.available).toBe(true)
    expect(row.refusal.lead).toBe("It is offered because a stream mode with its own compositor can carry it. A PyroWave launch into this host's own stream mode is refused:")
    expect(row.refusal.message).toBe("PyroWave cannot stream this host's desktop.")
    expect(row.refusal.action).toContain('Private Stream')
  })

  it('stays silent on a host that predates the PyroWave report', () => {
    expect(describePyroWave(t, { ready: true })).toBeNull()
  })
})

const PYROWAVE_BITRATE = {
  version: 1,
  model: 'psnr-hvs-m',
  target_db: 35,
  width: 1920,
  height: 1080,
  fps: 60,
  chroma: '420',
  advice_far_kbps: 171759,
  advice_near_kbps: 245904,
  raise_goal_kbps: 171759,
  cap_kbps: 300000,
  encoder_kbps: 20000,
  ceiling_frame_share: 0.929,
  starved: true,
  rule: 'model',
  raise_goal_limited_by: 'advice',
  live_tuning_floor_encoder_kbps: 76785,
  request_cap: null,
  cap_set_aside: { kbps: 15000, source: 'stability_preset_selected' },
  assumes: { fec_percentage: 10, audio_kbps: 512 },
}

function streaming(overrides = {}) {
  return {
    streaming: true,
    codec: 'pyrowave',
    width: 1920,
    height: 1080,
    stream_chroma: '420',
    fec_protection: { fec_percentage: 0, oversized_frames_total: 0 },
    clients: [{ name: 'Retroid Pocket 6', codec: 'pyrowave', pyrowave_route: 'zero_copy' }],
    pyrowave_bitrate: PYROWAVE_BITRATE,
    ...overrides,
  }
}

describe('describePyroWaveStream', () => {
  it('shows the running stream, its route and the advice with its conditions', () => {
    const stream = describePyroWaveStream(t, streaming())
    expect(stream.running).toBe(true)
    expect(stream.heading).toBe('PyroWave stream running: 1920x1080 at 60 fps, 4:2:0.')
    expect(stream.routes).toEqual([
      'Route: zero copy. Captured DMA-BUF frames are imported and their colour is converted on the GPU, with no CPU upload at the encoder input.',
    ])
    expect(stream.lines).toEqual([
      'The route says how frames reach the encoder, not how capture produced them.',
      "Advice for this stream: 172 Mbps on a device's own screen, 246 Mbps on a television or monitor.",
      "Each figure is what a client sets, with 10% FEC and the stream's audio included, for 35 dB of PSNR-HVS-M-H in PyroWave's own bitrate model at this size, frame rate and chroma. The model is an objective estimate from four game clips on SDR, not a measurement on any device.",
      'The encoder runs at 20 Mbps now.',
      "93% of about the last 240 frames hit PyroWave's byte budget.",
      "The host reads this stream as starved: the encoder runs below where Doctor's raise would land it, or more than 80% of recent frames hit the byte budget.",
      "Doctor's PyroWave raise, for a starved stream on a clean network, goes to 172 Mbps, the own screen figure.",
      'A launch cap of 15 Mbps (stability_preset_selected), sized for H.264, was set aside for this stream.',
    ])
    expectHouseStyle(everyLine(stream))
  })

  it('names what limits the raise and which rule gave the figures', () => {
    const stream = describePyroWaveStream(t, streaming({
      pyrowave_bitrate: {
        ...PYROWAVE_BITRATE,
        width: 1920,
        height: 1200,
        chroma: '444',
        fps: 120,
        rule: 'model_not_16_9',
        raise_goal_kbps: 300000,
        raise_goal_limited_by: 'cap',
        starved: false,
        ceiling_frame_share: null,
        request_cap: { kbps: 250000, source: 'max_bitrate' },
        cap_set_aside: null,
      },
    }))
    expect(stream.heading).toBe('PyroWave stream running: 1920x1200 at 120 fps, 4:4:4.')
    expect(stream.lines).toContain('The model was fitted on 16:9 pictures, so for this shape it is keyed on the pixel count.')
    expect(stream.lines).toContain("Doctor's PyroWave raise, for a starved stream on a clean network, goes to 300 Mbps, the most Polaris raises PyroWave to.")
    expect(stream.lines).toContain("A cap of 250 Mbps (max_bitrate) applied to this stream's request.")
    expect(stream.lines.join(' ')).not.toContain('starved:')
    expect(stream.lines.join(' ')).not.toContain('byte budget.')

    const limited = describePyroWaveStream(t, streaming({
      pyrowave_bitrate: { ...PYROWAVE_BITRATE, raise_goal_kbps: 120000, raise_goal_limited_by: 'max_bitrate' },
    }))
    expect(limited.lines).toContain("Doctor's PyroWave raise, for a starved stream on a clean network, goes to 120 Mbps, this host's Maximum Bitrate.")
  })

  it('gives one route per client when more than one watches PyroWave', () => {
    const stream = describePyroWaveStream(t, streaming({
      clients: [
        { name: 'Retroid Pocket 6', codec: 'pyrowave', pyrowave_route: 'gpu_upload' },
        { name: 'Pixel 10 Pro', codec: 'pyrowave' },
      ],
    }))
    expect(stream.routes).toEqual([
      'Retroid Pocket 6: Route: GPU upload. Frames are copied from host memory to the GPU and their colour is converted there.',
      'Pixel 10 Pro: Route: not known until the first captured frame is encoded.',
    ])
  })

  it('says there is no advice yet rather than inventing figures', () => {
    const stream = describePyroWaveStream(t, streaming({ pyrowave_bitrate: undefined, fec_protection: undefined }))
    expect(stream.lines).toEqual([
      'The route says how frames reach the encoder, not how capture produced them.',
      'No bitrate advice yet: the stream has not reported a size and frame rate the model can answer for.',
    ])
  })

  it('words the FEC the host grossed the advice up for, never the oversized frame telemetry', () => {
    const conditions = (stream) => stream.lines.find((line) => line.startsWith('Each figure is what a client sets'))
    const configured = describePyroWaveStream(t, streaming({
      pyrowave_bitrate: { ...PYROWAVE_BITRATE, assumes: { fec_percentage: 20, audio_kbps: 1536 } },
    }))
    expect(conditions(configured)).toContain('with 20% FEC and the stream\'s audio included')
    // Once a frame outgrows FEC, fec_protection carries that frame's percentage; the advice is unchanged.
    const oversized = describePyroWaveStream(t, streaming({ fec_protection: { fec_percentage: 35, oversized_frames_total: 3 } }))
    expect(conditions(oversized)).toContain('with 10% FEC')
    expect(conditions(oversized)).not.toContain('35%')
  })

  it('keeps the conditions when the host sends no FEC figure', () => {
    const stream = describePyroWaveStream(t, streaming({ pyrowave_bitrate: { ...PYROWAVE_BITRATE, assumes: undefined } }))
    expect(stream.lines).toContain("Each figure is what a client sets, with FEC and the stream's audio included, for 35 dB of PSNR-HVS-M-H in PyroWave's own bitrate model at this size, frame rate and chroma. The model is an objective estimate from four game clips on SDR, not a measurement on any device.")
  })

  it('shows the last PyroWave stream when none runs, and nothing for another codec', () => {
    const last = describePyroWaveStream(t, {
      streaming: false,
      clients: [],
      last_session: { state: 'ended', client_name: 'Retroid Pocket 6', codec: 'pyrowave', pyrowave_route: 'cpu_convert' },
    })
    expect(last.running).toBe(false)
    expect(last.heading).toBe('Last PyroWave stream, Retroid Pocket 6')
    expect(last.routes).toEqual([
      'Route: CPU conversion. Colour is converted on the CPU and the planes are copied to the GPU, which costs host CPU time.',
    ])

    expect(describePyroWaveStream(t, { streaming: false, clients: [], last_session: { codec: 'hevc' } })).toBeNull()
    expect(describePyroWaveStream(t, { streaming: true, codec: 'hevc', clients: [{ codec: 'hevc' }] })).toBeNull()
    expect(describePyroWaveStream(t, null)).toBeNull()
  })
})

describe('helpers', () => {
  it('rounds advice up to whole Mbps and live rates to the nearest', () => {
    expect(mbps(171759, { up: true })).toBe(172)
    // A figure a client sets is never rounded below what the model asks for.
    expect(mbps(171200, { up: true })).toBe(172)
    expect(mbps(171200)).toBe(171)
    expect(mbps(undefined)).toBe(0)
  })

  it('lists names the way a sentence does', () => {
    expect(listPhrase(t, [])).toBe('')
    expect(listPhrase(t, ['NVENC'])).toBe('NVENC')
    expect(listPhrase(t, ['NVENC', 'VA-API'])).toBe('NVENC and VA-API')
    expect(listPhrase(t, ['NVENC', 'Vulkan Video', 'VA-API'])).toBe('NVENC, Vulkan Video and VA-API')
  })
})
