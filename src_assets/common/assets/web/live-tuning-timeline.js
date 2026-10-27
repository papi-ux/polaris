import { formatReportAge, reading } from './mission-control.js'

// Live Tuning's own changes to the stream's rate and error correction, read from the served
// live_tuning_history. Each change is placed on the dashboard sparklines' time axis, and the newest
// one is told in a single line. Nothing is inferred: a change, its direction and its cause are the
// entry's recorded kind, cause and values, and a change counts as confirmed only when the history
// records the encoder running at it.

// Live Tuning's own rate decisions. Operator, Doctor and configuration writes are not its changes.
const RATE_CAUSES = new Set(['network_feedback', 'stream_health'])
// The entry that confirms each kind of FEC request, and the entries that close one without it.
const FEC_CONFIRMATION = { parity_trial: 'fec_published', restore_configured_parity: 'fec_restored' }
const FEC_CLOSED = new Set(['fec_refused', 'fec_cancelled', 'fec_retired', 'fec_target'])

// The sparkline that carries each track: the rate on Bitrate, error correction on Loss.
export const TUNING_TRACKS = { rate: 'bitrate', fec: 'loss' }

const integer = value => (Number.isInteger(value) ? value : null)

// Readable only as the host serves a live stream's own record, timed on the host's clock.
export function readableTuningHistory(history) {
  return history?.version === 1 && history.available === true && history.reason === 'recorded' &&
    history.clock === 'host_steady_ms' && reading(history.session_generation) > 0 &&
    reading(history.host_elapsed_ms_at_snapshot) !== null && Array.isArray(history.entries)
}

// Confirmed once an entry records the encoder applying this rate, before any later target replaces it.
function rateOutcome(entries, from, kbps) {
  for (let index = from; index < entries.length; index++) {
    const entry = entries[index]
    if (entry?.applied_proof === 'encoder_confirmed' && entry.applied_bitrate_kbps === kbps) return 'confirmed'
    if (index > from && reading(entry?.requested_bitrate_kbps) !== kbps) return 'unconfirmed'
  }
  return 'pending'
}

function fecOutcome(entries, from, cause) {
  for (let index = from + 1; index < entries.length; index++) {
    const kind = entries[index]?.kind
    if (kind === FEC_CONFIRMATION[cause]) return 'confirmed'
    if (FEC_CLOSED.has(kind)) return 'unconfirmed'
  }
  return 'pending'
}

// The rate and FEC changes Live Tuning recorded, oldest first. A rate change needs the previous
// entry's target, so an entry with an unknown target can neither be one nor stand as the baseline.
export function tuningChanges(history) {
  if (!readableTuningHistory(history)) return []
  const entries = history.entries
  // "The starting rate" is the scope's own first target, known only when nothing has been dropped.
  const complete = history.dropped === 0 && history.truncated === false && entries[0]?.kind === 'scope_started'
  const startKbps = complete ? reading(entries[0].requested_bitrate_kbps) : null
  const changes = []
  let previous = null
  entries.forEach((entry, index) => {
    const at = reading(entry?.host_elapsed_ms)
    const kbps = reading(entry?.requested_bitrate_kbps)
    if (at !== null && entry.kind === 'decision' && RATE_CAUSES.has(entry.cause) && previous > 0 && kbps > 0 && kbps !== previous) {
      changes.push({
        sequence: entry.sequence, at, track: 'rate', cause: entry.cause, direction: kbps < previous ? 'down' : 'up', kbps,
        restored: kbps > previous && kbps === startKbps, outcome: rateOutcome(entries, index, kbps),
      })
    }
    const fec = entry?.fec_transition
    if (at !== null && entry.kind === 'fec_target' && fec && FEC_CONFIRMATION[entry.cause]) {
      const restoring = entry.cause === 'restore_configured_parity'
      // A trial's requested parity is still pending; a restore returns to the configured parity.
      const percent = integer(restoring ? fec.configured_percentage : fec.pending_percentage_after)
      const before = integer(fec.percentage_before)
      if (percent !== null && before !== null && percent !== before) {
        changes.push({
          sequence: entry.sequence, at, track: 'fec', cause: entry.cause, direction: percent < before ? 'down' : 'up', percent,
          restored: restoring, outcome: fecOutcome(entries, index, entry.cause),
        })
      }
    }
    previous = kbps
  })
  return changes
}

// Where each change falls on the sparklines' time axis, keyed by sparkline. The axis is this
// browser's receipt time of each report, so the newest point is when the report carrying `history`
// arrived. A change sits its host-measured age before that, and only inside the plotted window.
export function tuningMarkers(changes, history, timestamps) {
  const markers = { [TUNING_TRACKS.rate]: [], [TUNING_TRACKS.fec]: [] }
  const snapshot = reading(history?.host_elapsed_ms_at_snapshot)
  if (!changes?.length || snapshot === null || !Array.isArray(timestamps) || timestamps.length < 2) return markers
  const start = timestamps[0]
  const anchor = timestamps[timestamps.length - 1]
  if (!(anchor > start)) return markers
  for (const change of changes) {
    const age = (snapshot - change.at) / 1000
    if (!(age >= 0)) continue
    const at = anchor - age
    if (at >= start) markers[TUNING_TRACKS[change.track]].push({ at, direction: change.direction, sequence: change.sequence })
  }
  return markers
}

// One line for the newest change: what Live Tuning did and why, whether the encoder confirmed it,
// and how long ago on the host's clock. Empty when nothing changed.
export function latestTuningChange(changes, history, t = key => key) {
  const change = changes?.[changes.length - 1]
  const snapshot = reading(history?.host_elapsed_ms_at_snapshot)
  if (!change || snapshot === null) return ''
  const action = change.track === 'rate'
    ? `${t(`mission_control.tuning_rate_${change.restored ? 'restored' : change.direction}`, { mbps: (change.kbps / 1000).toFixed(1) })} ${t(`mission_control.tuning_cause_${change.cause}`)}`
    : t(`mission_control.tuning_fec_${change.cause}`, { percent: change.percent })
  return [action, t(`mission_control.tuning_${change.outcome}`),
    t('mission_control.age_ago', { age: formatReportAge(Math.max(0, snapshot - change.at)) })].join(' · ')
}

// Draw a sparkline's change markers on its uPlot canvas: a small triangle on the chart's edge,
// pointing down from the top where Live Tuning lowered the rate or parity and up from the bottom
// where it raised it. Positions come from the chart's own x scale, so they line up with its readings.
export function drawTuningMarkers(u, markers, color, ratio = 1) {
  if (!markers?.length || !color) return
  const { left, top, width, height } = u.bbox
  const size = 3.5 * ratio
  const ctx = u.ctx
  ctx.save()
  ctx.fillStyle = color
  for (const marker of markers) {
    const x = u.valToPos(marker.at, 'x', true)
    if (!(x >= left && x <= left + width)) continue
    const edge = marker.direction === 'down' ? top : top + height
    const tip = marker.direction === 'down' ? edge + size * 1.6 : edge - size * 1.6
    ctx.beginPath()
    ctx.moveTo(x - size, edge)
    ctx.lineTo(x + size, edge)
    ctx.lineTo(x, tip)
    ctx.closePath()
    ctx.fill()
  }
  ctx.restore()
}
