import {
  browserStreamAppFit,
  browserStreamInputSummary,
  browserStreamIsSupported,
  browserStreamProfileById,
  browserStreamProfiles,
  browserStreamReadiness,
  browserStreamUnsupportedReasons,
  detectBrowserStreamSupport,
  formatBrowserStreamAppOption,
} from './browser-stream-support.js'

describe('Browser Stream support detection', () => {
  it('requires WebTransport and WebCodecs decoders', () => {
    const support = detectBrowserStreamSupport({
      WebTransport: function WebTransport() {},
      VideoDecoder: function VideoDecoder() {},
    })

    expect(browserStreamIsSupported(support)).toBe(false)
    expect(browserStreamUnsupportedReasons(support)).toEqual(['WebCodecs AudioDecoder'])
  })

  it('passes when all browser primitives exist', () => {
    const support = detectBrowserStreamSupport({
      WebTransport: function WebTransport() {},
      VideoDecoder: function VideoDecoder() {},
      AudioDecoder: function AudioDecoder() {},
    })

    expect(browserStreamIsSupported(support)).toBe(true)
    expect(browserStreamUnsupportedReasons(support)).toEqual([])
  })

  it('summarizes readiness from browser and host state', () => {
    const support = detectBrowserStreamSupport({
      WebTransport: function WebTransport() {},
      VideoDecoder: function VideoDecoder() {},
      AudioDecoder: function AudioDecoder() {},
    })

    expect(browserStreamReadiness(support, { build_enabled: true, config_enabled: true }).ready).toBe(true)
    expect(browserStreamReadiness(support, { build_enabled: false, config_enabled: true }).label).toBe('Not built')
    expect(browserStreamReadiness({}, { build_enabled: true, config_enabled: true }).message).toContain('Missing')
  })

  it('marks browser controller input as deferred when reserved but unavailable', () => {
    const inputs = browserStreamInputSummary({
      touch: true,
      pointer: true,
      keyboard: true,
      wheel: true,
      gamepad: false,
      gamepad_reserved: true,
    })

    expect(inputs.find((input) => input.key === 'touch').supported).toBe(true)
    expect(inputs.find((input) => input.key === 'gamepad')).toMatchObject({
      supported: false,
      deferred: true,
    })
  })

  it('maps app metadata to practical browser stream fit labels', () => {
    expect(browserStreamAppFit({ name: 'Desktop', 'game-category': 'desktop' }).label).toBe('Best in browser')
    expect(browserStreamAppFit({ name: 'Slay the Spire 2', 'game-category': 'cinematic' }).label).toBe('Good fit')
    expect(browserStreamAppFit({ name: 'Racing Game', 'game-category': 'fast_action' }).label).toBe('Nova recommended')
    expect(browserStreamAppFit({ name: 'SteamVR', 'game-category': 'vr' }).label).toBe('Nova recommended')
    expect(browserStreamAppFit({ name: 'Unknown Game' }).label).toBe('Try in browser')
  })

  it('formats app select labels with fit guidance', () => {
    expect(formatBrowserStreamAppOption({
      name: 'Desktop',
      uuid: 'desktop',
      'game-category': 'desktop',
    })).toBe('Desktop - Best in browser')
  })

  it('provides practical stream profiles with a balanced fallback', () => {
    expect(browserStreamProfiles.map((profile) => profile.id)).toEqual([
      'low_latency',
      'balanced',
      'battery_saver',
    ])
    expect(browserStreamProfileById('battery_saver').backend).toMatchObject({
      width: 960,
      height: 540,
      fps: 30,
    })
    expect(browserStreamProfileById('unknown').id).toBe('balanced')
  })
})
