import { judgedVideoFrameLoss } from './diagnostics-export.js'

// Presentation helpers for the Mission Control stage. They only reshape data the dashboard
// already reads; a reading the host has not reported becomes a dash with a reason, never a zero.
export const MISSING = '—'

// How long the stream-status channel may stay disconnected before the page says so. The
// channel reconnects on its own (useStreamStats), so a brief gap is not shown as an outage.
export const STATUS_GRACE_MS = 5000

// Only a real number is a reading. false, '', ' ', [] and numeric-looking strings would all coerce
// to a number, and a measured 0 must not come from a value the host never sent.
export function reading(value) {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}
const finite = reading

export function graphemeCount(text) {
  const value = String(text ?? '')
  if (typeof Intl !== 'undefined' && typeof Intl.Segmenter === 'function') {
    return [...new Intl.Segmenter(undefined, { granularity: 'grapheme' }).segment(value)].length
  }
  return Array.from(value).length
}

// Real titles run from "Control" to "The Legend of Zelda: Tears of the Kingdom". The stage title
// steps down for longer names and wraps; it is never truncated.
export function titleStep(text) {
  const count = graphemeCount(text)
  if (count <= 12) return 'full'
  if (count <= 24) return 'medium'
  return 'long'
}

export function formatReportAge(ms) {
  const seconds = Math.max(0, Math.round(Number(ms) / 1000))
  if (seconds < 60) return `${seconds} s`
  const minutes = Math.floor(seconds / 60)
  if (minutes < 60) return `${minutes} min`
  return `${Math.floor(minutes / 60)} h ${minutes % 60} min`
}

// The stream status counts as unavailable only once the channel has been disconnected for the
// grace period, including a page that has never received a report.
export function streamStatusUnavailable({ connected, disconnectedSince, now, graceMs = STATUS_GRACE_MS } = {}) {
  return connected === false && disconnectedSince > 0 && now - disconnectedSince >= graceMs
}

// Which stage the page shows. Spaces activity keeps its own section and no stage.
export function missionStageMode({ unavailable = false, statsLoaded = false, streaming = false, spacesActive = false } = {}) {
  if (unavailable) return 'unavailable'
  if (!statsLoaded) return 'loading'
  if (streaming) return 'live'
  if (spacesActive) return 'spaces'
  return 'idle'
}

// The library cover for an app, or '' when the host has none for it.
export function appCoverSrc(app) {
  if (!app || !app.name || !app['image-path']) return ''
  return './api/covers/image?name=' + encodeURIComponent(app.name)
}

// An app's background (hero) or logo from the host artwork cache, by uuid. The host answers 404
// when it has none yet, and the stage then keeps the cover and the title text.
export function appArtworkSrc(app, kind) {
  if (!app?.uuid) return ''
  return `./api/apps/${encodeURIComponent(app.uuid)}/artwork/${kind}`
}

// SDR is named only when the host reports an SDR mode; a missing mode stays unnamed.
export function dynamicRangeLabel(stats = {}) {
  const reason = String(stats?.hdr_downgrade_reason || 'none')
  if (reason !== 'none') return 'HDR → SDR'
  const mode = String(stats?.hdr_effective_mode || stats?.dynamic_range || '').toLowerCase()
  if (!mode) return ''
  if (mode.startsWith('sdr')) return 'SDR'
  return mode.toUpperCase()
}

export function streamFormatLabel(stats = {}) {
  const codec = stats?.codec ? String(stats.codec).toUpperCase() : ''
  const width = finite(stats?.width)
  const height = finite(stats?.height)
  const size = width > 0 && height > 0 ? `${width}×${height}` : ''
  return [codec, size].filter(Boolean).join(' · ')
}

function cell(key, label, value, extra = {}) {
  return { key, label, value, unit: '', sub: '', tone: '', tag: '', grade: '', title: '', attrs: {}, ...extra }
}

function missingCell(key, label, sub, extra = {}) {
  return cell(key, label, MISSING, { sub, tone: 'text-storm', missing: true, ...extra })
}

// The six live readings. Latency is the round trip the host measures.
export function liveSignalCells({ stats = {}, summary = {}, grade = '', score = 0, t = key => key } = {}) {
  const attrs = metric => ({ 'data-live-summary-metric': metric })
  const notReported = t('mission_control.not_reported')
  const cells = []
  const fps = finite(stats.fps)
  const latency = finite(stats.latency_ms)
  const encode = finite(stats.encode_time_ms)
  const loss = judgedVideoFrameLoss(stats)
  // The grade weighs frame rate, latency, encode time and judged loss. It is shown only when all
  // four were observed; a grade from some of them would claim quality nobody measured.
  const missingInputs = [
    fps === null ? t('mission_control.input_fps') : '',
    latency === null ? t('mission_control.input_latency') : '',
    encode === null ? t('mission_control.input_encode') : '',
    loss === null ? t('mission_control.input_loss') : '',
  ].filter(Boolean)
  cells.push(missingInputs.length
    ? missingCell('quality', 'Quality', missingInputs.length === 4 ? notReported : t('mission_control.quality_waiting', { inputs: missingInputs.join(', ') }), { attrs: attrs('Quality') })
    : cell('quality', 'Quality', String(score), { grade, tone: summary.qualityTone || '', sub: summary.qualityDetail || '', attrs: attrs('Quality'), spark: 'quality' }))
  cells.push(latency === null
    ? missingCell('latency', 'Latency', notReported, { attrs: attrs('Latency') })
    : cell('latency', 'Latency', latency.toFixed(0), { unit: 'ms', sub: t('mission_control.round_trip'), tone: summary.latencyTone || '', attrs: attrs('Latency'), spark: 'latency' }))
  const target = finite(stats.session_target_fps ?? stats.requested_client_fps)
  cells.push(fps === null
    ? missingCell('fps', 'FPS', notReported, { attrs: attrs('FPS') })
    : cell('fps', 'FPS', fps.toFixed(1), { sub: target > 0 ? t('mission_control.fps_target', { fps: target.toFixed(0) }) : t('mission_control.fps_encoded'), tone: summary.fpsTone || '', attrs: attrs('FPS'), spark: 'fps' }))
  cells.push(loss === null
    ? missingCell('loss', 'Loss', t('mission_control.not_judged'), { title: summary.lossTitle || '', attrs: attrs('Loss') })
    : cell('loss', 'Loss', loss.pct.toFixed(1), {
      unit: '%', tone: summary.lossTone || '', title: summary.lossTitle || '',
      tag: loss.state === 'elevated' || loss.state === 'light' ? t(`mission_control.loss_${loss.state}`) : '',
      sub: t('mission_control.loss_window', { seconds: loss.windowSeconds }), attrs: attrs('Loss'), spark: 'loss',
    }))
  const bitrate = finite(stats.bitrate_kbps)
  cells.push(bitrate === null
    ? missingCell('bitrate', 'Bitrate', notReported, { attrs: attrs('Bitrate') })
    : cell('bitrate', 'Bitrate', (bitrate / 1000).toFixed(1), { unit: 'Mbps', sub: t('mission_control.bitrate_current'), tone: 'text-silver', attrs: attrs('Bitrate'), spark: 'bitrate' }))
  cells.push(encode === null
    ? missingCell('encode', 'Encode', notReported, { attrs: attrs('Encode') })
    : cell('encode', 'Encode', encode.toFixed(1), { unit: 'ms', sub: t('mission_control.encode_per_frame'), tone: 'text-silver', attrs: attrs('Encode'), spark: 'encode' }))
  return cells
}

// While the stream status is unavailable, the live readings are blank with the reason, never the
// last values shown as current.
export function unavailableSignalCells({ t = key => key } = {}) {
  const sub = t('mission_control.not_reported')
  return [['quality', 'Quality'], ['latency', 'Latency'], ['fps', 'FPS'], ['loss', 'Loss'], ['bitrate', 'Bitrate'], ['encode', 'Encode']]
    .map(([key, label]) => missingCell(key, label, sub, { attrs: { 'data-unavailable-signal': label } }))
}

// How long the Last session cell recaps a stream this page watched end before it settles back.
export const RECAP_HOLD_MS = 15000
// How far this browser's record of a stream may differ from the span the host reports for it and
// still be that stream watched whole: a report tick at each end and the host's whole-second times.
export const RECAP_SPAN_TOLERANCE_S = 5
export const RECAP_REPORT_ROUTE = '/troubleshooting'

// Only the controller's own decisions count as Live Tuning; Doctor, operator and config changes do not.
const LIVE_TUNING_CAUSES = new Set(['network_feedback', 'stream_health'])

// How many times Live Tuning lowered its requested target in a history captured while the stream
// was live, or null when that history cannot vouch for every decision since the stream began.
export function liveTuningLoweredCount(history) {
  if (history?.version !== 1 || history.available !== true || history.reason !== 'recorded') return null
  if (history.truncated !== false || history.dropped !== 0 || !Array.isArray(history.entries)) return null
  let count = 0
  let previous = null
  for (const entry of history.entries) {
    const requested = finite(entry?.requested_bitrate_kbps)
    if (requested === null) return null
    if (entry.kind === 'decision' && LIVE_TUNING_CAUSES.has(entry.cause) && previous > 0 && requested > 0 && requested < previous) count++
    previous = requested
  }
  return count
}

// The recap for a stream this page watched end, or null. It needs the host's ended record to name
// the captured stream, and this browser's history entry to have been written at that end and to
// span what the host says the stream lasted; an entry from a page that joined late, or one merged
// across a feed gap, does not. Averages come from that entry, and what it lacks is left out.
export function lastSessionRecap({ snapshot = null, lastSession = null, entry = null, t = key => key } = {}) {
  const capturedAt = Date.parse(snapshot?.capturedAt)
  if (snapshot?.origin !== 'completed' || typeof snapshot.streamId !== 'string' || !snapshot.streamId || !Number.isFinite(capturedAt)) return null
  if (lastSession?.state !== 'ended' || lastSession.stream_instance_id !== snapshot.streamId) return null
  const hostSpan = (Date.parse(lastSession.ended_at) - Date.parse(lastSession.started_at)) / 1000
  const duration = finite(entry?.duration_s)
  const endedAt = finite(entry?.ended_at)
  if (!(hostSpan > 0) || !(duration > 0) || !(endedAt >= capturedAt) || Math.abs(duration - hostSpan) > RECAP_SPAN_TOLERANCE_S) return null
  const fps = finite(entry.avg_fps)
  const kbps = finite(entry.avg_bitrate_kbps)
  const lowered = liveTuningLoweredCount(snapshot.stats?.live_tuning_history)
  const detail = [
    fps > 0 ? t('mission_control.recap_fps_avg', { fps: Number(fps.toFixed(1)) }) : '',
    kbps > 0 ? t('mission_control.recap_bitrate_avg', { mbps: (kbps / 1000).toFixed(1) }) : '',
    lowered > 0 ? t('mission_control.recap_live_tuning_lowered', { count: lowered }) : '',
  ].filter(Boolean).join(' · ')
  return { entry, detail }
}

// The idle strip summarises readiness from the checks, clients, history, config and GPU reads the
// dashboard already makes. A source that has not answered shows a dash with its reason.
export function idleSignalCells({
  readiness = { label: '', tone: '', passing: 0, total: 0 },
  clientsKnown = false, pairedClients = 0,
  lastSession = null, modeLabel = '',
  gpu = null, liveTuningLabel = '',
  t = key => key,
} = {}) {
  const attrs = name => ({ 'data-idle-signal': name })
  const cells = [
    cell('readiness', t('mission_control.signal_readiness'), readiness.label, {
      word: true, tone: readiness.tone,
      sub: readiness.total ? t('mission_control.checks_ready', { passing: readiness.passing, total: readiness.total }) : '',
      attrs: attrs('readiness'),
    }),
    clientsKnown
      ? cell('paired', t('mission_control.signal_paired'), String(pairedClients), { attrs: attrs('paired') })
      : missingCell('paired', t('mission_control.signal_paired'), t('mission_control.not_read'), { attrs: attrs('paired') }),
  ]
  if (lastSession) {
    // While a recap holds, the same cell says the stream ended, opens the full report and puts
    // the stream's averages where the client and date go. It adds no row; only that text changes.
    const recap = lastSession.recap
    cells.push(cell('last', t(recap ? 'mission_control.recap_label' : 'mission_control.signal_last_session'), lastSession.duration || MISSING, {
      word: true, grade: lastSession.grade || '', tone: lastSession.tone || '', sub: recap?.detail || lastSession.detail || '', attrs: attrs('last'),
      ...(recap ? { recap: true, link: { to: RECAP_REPORT_ROUTE, label: t('mission_control.recap_link') } } : {}),
    }))
  }
  cells.push(modeLabel
    ? cell('mode', t('dashboard.mode'), modeLabel, { word: true, attrs: attrs('mode') })
    : missingCell('mode', t('dashboard.mode'), t('mission_control.not_read'), { attrs: attrs('mode') }))
  const temperature = finite(gpu?.temperature_c)
  const utilization = finite(gpu?.utilization_pct)
  const power = finite(gpu?.power_draw_w)
  if (gpu) {
    // A reading the GPU does not report shows as "--" in its own unit, as the host context always
    // has: "--°C", "--%", "--W", never 0.
    const sub = `${utilization === null ? '--' : Number(utilization.toFixed(1))}% · ${power === null ? '--' : power.toFixed(0)}W`
    cells.push(cell('gpu', 'GPU', temperature === null ? '--' : String(Number(temperature.toFixed(1))), {
      unit: '°C', sub, attrs: attrs('gpu'),
    }))
  } else {
    cells.push(missingCell('gpu', 'GPU', t('mission_control.telemetry_warming'), { attrs: attrs('gpu') }))
  }
  // The shared Live Tuning labels name the feature themselves ("Live Tuning Off"); the cell already does.
  const tuning = String(liveTuningLabel || '').replace(/^Live Tuning:?\s*/, '')
  cells.push(cell('tuning', t('mission_control.signal_live_tuning'), tuning || MISSING, { word: true, attrs: attrs('tuning') }))
  return cells
}

// The library title the idle stage features: the one picked from the shelf while it is still
// there, otherwise the most recently launched one. Without any launch record it is simply the
// first title in the library, and the stage says so.
export function featuredLibraryApp(recentApps = [], selectedUuid = '') {
  const list = Array.isArray(recentApps) ? recentApps : []
  const picked = selectedUuid ? list.find(app => app?.uuid === selectedUuid) : null
  const app = picked || list[0]
  if (!app) return null
  const launched = finite(app['last-launched'])
  return { app, launchedAt: launched > 0 ? (launched > 1e12 ? launched : launched * 1000) : 0, chosen: Boolean(picked) }
}

export function relativeDay(epochMs, now = Date.now()) {
  if (!(epochMs > 0)) return ''
  const day = 86400000
  const start = new Date(now); start.setHours(0, 0, 0, 0)
  if (epochMs >= start.getTime()) return 'today'
  if (epochMs >= start.getTime() - day) return 'yesterday'
  return new Date(epochMs).toLocaleDateString([], { month: 'short', day: 'numeric' })
}
