// What the codec readout says about 4:4:4 and PyroWave. The host decides every fact: the yuv444 and
// pyrowave objects in encoder_codec_support on GET /api/config, and, while a PyroWave stream runs,
// pyrowave_bitrate and each client's pyrowave_route in the stream stats. This only words them.

const CODECS = ['h264', 'hevc', 'av1']
const ENCODERS = new Set(['nvenc', 'vaapi', 'vulkan', 'software', 'quicksync', 'amdvce', 'videotoolbox'])
const ROUTES = new Set(['zero_copy', 'gpu_upload', 'cpu_convert'])
const RULES = new Set(['model_not_16_9', 'below_model_edge', 'above_model_edge', 'flat_fallback'])
const RAISE_LIMITS = new Set(['advice', 'cap', 'max_bitrate'])

function text(value) {
  return typeof value === 'string' ? value.trim() : ''
}

function positive(value) {
  return Number.isFinite(value) && value > 0 ? value : 0
}

/** Join names the way a sentence lists them: "A", "A and B", "A, B and C". */
export function listPhrase(t, items) {
  const names = items.filter(Boolean)
  if (names.length <= 1) return names[0] || ''
  return t('config.codec_support_list_and', { rest: names.slice(0, -1).join(', '), last: names[names.length - 1] })
}

function codecPhrase(t, codecs) {
  return listPhrase(t, CODECS.filter((codec) => codecs.includes(codec)).map((codec) => t(`config.codec_support_${codec}`)))
}

function encoderName(t, encoder) {
  const value = text(encoder)
  return ENCODERS.has(value) ? t(`config.codec_support_encoder_${value}`) : value
}

/** Whole Mbps from kbps, rounded up for a figure a client sets and to the nearest for a live rate. */
export function mbps(kbps, { up = false } = {}) {
  const value = positive(kbps) / 1000
  return up ? Math.ceil(value) : Math.round(value)
}

/**
 * The 4:4:4 row, or null when the host predates the yuv444 report.
 * @param {Function} t The translate function.
 * @param {object} support encoder_codec_support from GET /api/config.
 */
export function describeYuv444(t, support) {
  const yuv444 = support?.yuv444
  if (!yuv444 || typeof yuv444 !== 'object') return null
  const active = CODECS.filter((codec) => yuv444[codec] === true)
  const encoders = Array.isArray(yuv444.encoders) ? yuv444.encoders.filter((entry) => text(entry?.encoder)) : []
  const capable = encoders.filter((entry) => Array.isArray(entry.codecs) && entry.codecs.some((codec) => CODECS.includes(codec)))
  const others = encoders.filter((entry) => !capable.includes(entry))
  const pyrowave = support?.pyrowave?.available === true

  const lines = []
  lines.push(active.length ?
    t('config.codec_support_yuv444_active_some', { codecs: codecPhrase(t, active) }) :
    t('config.codec_support_yuv444_active_none'))

  const capablePhrase = listPhrase(t, capable.map((entry) => t('config.codec_support_yuv444_encoder_codecs', {
    codecs: codecPhrase(t, entry.codecs),
    encoder: encoderName(t, entry.encoder),
  })))
  if (!capable.length) {
    lines.push(t('config.codec_support_yuv444_build_none'))
  } else if (others.length) {
    lines.push(t('config.codec_support_yuv444_build_only', {
      capable: capablePhrase,
      others: listPhrase(t, others.map((entry) => encoderName(t, entry.encoder))),
    }))
  } else {
    lines.push(t('config.codec_support_yuv444_build_all', { capable: capablePhrase }))
  }

  if (support?.pyrowave && typeof support.pyrowave === 'object') {
    lines.push(t(pyrowave ? 'config.codec_support_yuv444_pyrowave_available' : 'config.codec_support_yuv444_pyrowave_unavailable'))
  }

  let status = 'unsupported'
  if (active.length) status = 'supported'
  else if (pyrowave) status = 'pyrowave_only'
  return { status, codecs: active, lines }
}

/**
 * The PyroWave row, or null when the host predates the pyrowave report.
 * @param {Function} t The translate function.
 * @param {object} support encoder_codec_support from GET /api/config.
 */
export function describePyroWave(t, support) {
  const pyrowave = support?.pyrowave
  if (!pyrowave || typeof pyrowave !== 'object') return null
  const available = pyrowave.available === true
  const result = {
    available,
    hdr: available && pyrowave.hdr === true,
    lines: [t('config.codec_support_pyrowave_nova_only')],
    reason: '',
    message: '',
    refusal: null,
  }
  if (!available) {
    // The host's own words, the same capabilities gives a client, and its reason id beside them.
    result.message = text(pyrowave.message) || t('config.codec_support_pyrowave_unavailable_unknown')
    result.reason = text(pyrowave.reason)
    return result
  }
  const refusal = pyrowave.host_mode_refusal
  if (refusal && typeof refusal === 'object' && text(refusal.message)) {
    result.refusal = {
      lead: t('config.codec_support_pyrowave_host_mode_refused'),
      message: text(refusal.message),
      action: text(refusal.action),
    }
  }
  return result
}

function routeLine(t, route) {
  const value = text(route)
  return t(ROUTES.has(value) ? `config.codec_support_pyrowave_route_${value}` : 'config.codec_support_pyrowave_route_unknown')
}

function chromaLabel(t, chroma) {
  return t(text(chroma) === '444' ? 'config.codec_support_chroma_444' : 'config.codec_support_chroma_420')
}

/**
 * What the running PyroWave stream reports, or the last one when none runs; null when neither.
 * @param {Function} t The translate function.
 * @param {object} stats The stream stats from /api/stats/stream.
 */
export function describePyroWaveStream(t, stats) {
  if (!stats || typeof stats !== 'object') return null
  const clients = Array.isArray(stats.clients) ? stats.clients : []
  const pyrowaveClients = clients.filter((client) => text(client?.codec) === 'pyrowave')
  const running = stats.streaming === true && (text(stats.codec) === 'pyrowave' || pyrowaveClients.length > 0)

  if (!running) {
    const last = stats.last_session
    if (!last || text(last.codec) !== 'pyrowave') return null
    return {
      running: false,
      heading: t('config.codec_support_pyrowave_last_session', { client: text(last.client_name) || t('config.codec_support_client_unknown') }),
      routes: [routeLine(t, last.pyrowave_route)],
      lines: [t('config.codec_support_pyrowave_route_note')],
    }
  }

  const advice = stats.pyrowave_bitrate && typeof stats.pyrowave_bitrate === 'object' ? stats.pyrowave_bitrate : null
  const width = positive(advice?.width) || positive(stats.width)
  const height = positive(advice?.height) || positive(stats.height)
  const fps = positive(advice?.fps) || Math.round(positive(stats.encode_target_fps) || positive(stats.session_target_fps))
  const chroma = text(advice?.chroma) || text(stats.stream_chroma)
  const heading = width && height && fps ?
    t('config.codec_support_pyrowave_streaming', { width, height, fps, chroma: chromaLabel(t, chroma) }) :
    t('config.codec_support_pyrowave_streaming_unknown')

  // One route per stream: two clients watching one PyroWave stream can each report their own.
  const routes = pyrowaveClients.length > 1 ?
    pyrowaveClients.map((client) => t('config.codec_support_pyrowave_client_route', {
      client: text(client.name) || t('config.codec_support_client_unknown'),
      route: routeLine(t, client.pyrowave_route),
    })) :
    [routeLine(t, pyrowaveClients[0]?.pyrowave_route)]

  const lines = [t('config.codec_support_pyrowave_route_note')]
  if (!advice || !positive(advice.advice_far_kbps) || !positive(advice.advice_near_kbps)) {
    lines.push(t('config.codec_support_pyrowave_no_advice'))
    return { running: true, heading, routes, lines }
  }

  lines.push(t('config.codec_support_pyrowave_advice', {
    far: mbps(advice.advice_far_kbps, { up: true }),
    near: mbps(advice.advice_near_kbps, { up: true }),
  }))
  // The FEC share the host grossed the advice up for. Not fec_protection.fec_percentage: that records
  // a percentage only once a frame outgrows FEC, so it reads 0 on a healthy stream.
  const fec = advice.assumes?.fec_percentage
  // The own screen figure aims for its calibrated quality and the television figure for the author's
  // default. The host serving this console computed the advice and sends both; the fallbacks are its own
  // two targets, so a missing field never pairs the Retroid Pocket 6 calibration with 35 dB.
  const db = positive(advice.target_db) || 35
  const farDb = positive(advice.far_target_db) || 31
  lines.push(Number.isFinite(fec) && fec >= 0 ?
    t('config.codec_support_pyrowave_advice_conditions', { fec, db, far_db: farDb }) :
    t('config.codec_support_pyrowave_advice_conditions_no_fec', { db, far_db: farDb }))
  // The figures above are the model's readouts and are not capped. Past what Polaris recommends on its
  // own, the line says so, so a television figure of 594 Mbps does not read as advice to set it.
  const recommendedMost = positive(advice.cap_kbps)
  if (recommendedMost && (advice.advice_far_kbps > recommendedMost || advice.advice_near_kbps > recommendedMost)) {
    lines.push(t('config.codec_support_pyrowave_above_cap', { cap: mbps(recommendedMost) }))
  }
  if (RULES.has(text(advice.rule))) {
    lines.push(t(`config.codec_support_pyrowave_rule_${text(advice.rule)}`))
  }

  // What the stream runs at, as the request the advice is in, beside the encoder's own rate: an encoder
  // rate read against a request looks a tenth short when it is not.
  if (positive(advice.request_kbps) && positive(advice.encoder_kbps)) {
    lines.push(t('config.codec_support_pyrowave_rate_now', { request: mbps(advice.request_kbps), encoder: mbps(advice.encoder_kbps) }))
  }
  if (Number.isFinite(advice.ceiling_frame_share)) {
    lines.push(t('config.codec_support_pyrowave_ceiling_share', { share: Math.round(advice.ceiling_frame_share * 100) }))
  }
  if (advice.starved === true) {
    lines.push(t('config.codec_support_pyrowave_starved'))
  }
  const limit = text(advice.raise_goal_limited_by)
  if (positive(advice.raise_goal_kbps) && RAISE_LIMITS.has(limit)) {
    lines.push(t(`config.codec_support_pyrowave_raise_goal_${limit}`, { goal: mbps(advice.raise_goal_kbps, { up: true }) }))
  }
  const cap = advice.request_cap
  if (cap && positive(cap.kbps)) {
    lines.push(t('config.codec_support_pyrowave_request_cap', { mbps: mbps(cap.kbps), source: text(cap.source) }))
  }
  const setAside = advice.cap_set_aside
  if (setAside && positive(setAside.kbps)) {
    lines.push(t('config.codec_support_pyrowave_cap_set_aside', { mbps: mbps(setAside.kbps), source: text(setAside.source) }))
  }
  return { running: true, heading, routes, lines }
}
