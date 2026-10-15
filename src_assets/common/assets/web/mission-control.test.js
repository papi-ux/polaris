import { describe, expect, it } from 'vitest'
import {
  MISSING, STATUS_GRACE_MS, appArtworkSrc, appCoverSrc, reading, dynamicRangeLabel, featuredLibraryApp, formatReportAge,
  graphemeCount, idleSignalCells, liveSignalCells, missionStageMode, streamFormatLabel,
  streamStatusUnavailable, titleStep, unavailableSignalCells,
} from './mission-control.js'
import { buildLiveSummary, buildQualityGrade, buildQualityScore } from './dashboard-summary.js'

const t = (key, params) => (params ? `${key} ${JSON.stringify(params)}` : key)

function live(stats) {
  const score = buildQualityScore(stats)
  const grade = buildQualityGrade(score)
  return liveSignalCells({ stats, summary: buildLiveSummary({ stats, qualityGrade: grade, qualityScore: score }), grade, score, t })
}

describe('Mission Control stage helpers', () => {
  it('steps long real titles down by grapheme count instead of truncating them', () => {
    expect(titleStep('Control')).toBe('full')
    expect(titleStep('Hades II: Tartarus')).toBe('medium')
    expect(titleStep('The Legend of Zelda: Tears of the Kingdom')).toBe('long')
    // Twelve graphemes, some of them several code units, still count as twelve.
    expect(graphemeCount('Pokémon Zéro')).toBe(12)
    expect(titleStep('Pokémon Zéro')).toBe('full')
    expect(titleStep('🇯🇵 Tokyo Xtreme')).toBe('medium')
  })

  it('uses library artwork only when the host has a cover for the title', () => {
    expect(appCoverSrc({ name: 'Control Ultimate Edition', 'image-path': 'control.png' })).toBe('./api/covers/image?name=Control%20Ultimate%20Edition')
    expect(appCoverSrc({ name: 'No Cover' })).toBe('')
    expect(appCoverSrc(null)).toBe('')
  })

  it('asks the host for a background or logo by uuid, never by name', () => {
    expect(appArtworkSrc({ uuid: 'C0FFEE00-0000-4000-8000-000000000001', name: 'Control' }, 'hero'))
      .toBe('./api/apps/C0FFEE00-0000-4000-8000-000000000001/artwork/hero')
    expect(appArtworkSrc({ uuid: 'a/b', name: 'Odd' }, 'logo')).toBe('./api/apps/a%2Fb/artwork/logo')
    expect(appArtworkSrc({ name: 'No uuid' }, 'hero')).toBe('')
    expect(appArtworkSrc(null, 'logo')).toBe('')
  })

  it('names SDR only when the host reports it and keeps a downgrade visible', () => {
    expect(dynamicRangeLabel({ hdr_effective_mode: 'sdr_10bit' })).toBe('SDR')
    expect(dynamicRangeLabel({ hdr_effective_mode: 'hdr10' })).toBe('HDR10')
    expect(dynamicRangeLabel({ hdr_effective_mode: 'sdr_8bit', hdr_downgrade_reason: 'display_unsupported' })).toBe('HDR → SDR')
    expect(dynamicRangeLabel({})).toBe('')
    expect(streamFormatLabel({ codec: 'hevc', width: 1920, height: 1080 })).toBe('HEVC · 1920×1080')
    expect(streamFormatLabel({ codec: 'av1' })).toBe('AV1')
    expect(streamFormatLabel({})).toBe('')
  })

  it('calls the stream status unavailable only after the grace period, including a page never reported to', () => {
    const now = 100000
    expect(streamStatusUnavailable({ connected: false, disconnectedSince: now - STATUS_GRACE_MS + 1, now })).toBe(false)
    expect(streamStatusUnavailable({ connected: false, disconnectedSince: now - STATUS_GRACE_MS, now })).toBe(true)
    expect(streamStatusUnavailable({ connected: true, disconnectedSince: 1, now })).toBe(false)
    // A stats source that does not say whether it is connected is not judged.
    expect(streamStatusUnavailable({ connected: undefined, disconnectedSince: 1, now })).toBe(false)
    expect(missionStageMode({ unavailable: true, statsLoaded: false })).toBe('unavailable')
    expect(missionStageMode({ statsLoaded: false })).toBe('loading')
    expect(missionStageMode({ statsLoaded: true, streaming: true, spacesActive: true })).toBe('live')
    expect(missionStageMode({ statsLoaded: true, spacesActive: true })).toBe('spaces')
    expect(missionStageMode({ statsLoaded: true })).toBe('idle')
    expect(formatReportAge(18400)).toBe('18 s')
    expect(formatReportAge(185000)).toBe('3 min')
    expect(formatReportAge(3 * 3600000 + 120000)).toBe('3 h 2 min')
  })

  it('shows the six live readings with round-trip latency and the host loss window', () => {
    const cells = live({ streaming: true, fps: 119.6, session_target_fps: 120, latency_ms: 11, bitrate_kbps: 48000, encode_time_ms: 3.14,
      network_verdict: { loss_pct: 0.4, loss_state: 'light', window_seconds: 12 } })
    expect(cells.map(cell => cell.attrs['data-live-summary-metric'])).toEqual(['Quality', 'Latency', 'FPS', 'Loss', 'Bitrate', 'Encode'])
    const byKey = Object.fromEntries(cells.map(cell => [cell.key, cell]))
    expect(byKey.latency).toMatchObject({ value: '11', unit: 'ms', sub: 'mission_control.round_trip' })
    expect(byKey.fps).toMatchObject({ value: '119.6', sub: 'mission_control.fps_target {"fps":"120"}' })
    expect(byKey.loss).toMatchObject({ value: '0.4', unit: '%', tag: 'mission_control.loss_light', sub: 'mission_control.loss_window {"seconds":12}' })
    expect(byKey.bitrate).toMatchObject({ value: '48.0', unit: 'Mbps' })
    expect(byKey.encode).toMatchObject({ value: '3.1', unit: 'ms' })
    expect(byKey.quality.grade).toMatch(/^[A-F]$/)
    expect(cells.every(cell => !cell.missing)).toBe(true)
  })

  it('renders a missing live reading as a dash with its reason, never a zero', () => {
    const cells = live({ streaming: true })
    for (const cell of cells) {
      expect(cell.value, cell.key).toBe(MISSING)
      expect(cell.missing, cell.key).toBe(true)
      expect(cell.sub, cell.key).not.toBe('')
      expect(cell.grade, cell.key).toBe('')
    }
    expect(cells.find(cell => cell.key === 'loss').sub).toBe('mission_control.not_judged')
    // A zero the host actually reported is a reading, not a gap.
    const zero = live({ streaming: true, fps: 0, latency_ms: 0, bitrate_kbps: 0, encode_time_ms: 0 })
    expect(zero.find(cell => cell.key === 'latency').value).toBe('0')
    expect(zero.find(cell => cell.key === 'fps').value).toBe('0.0')
    expect(zero.find(cell => cell.key === 'fps').sub).toBe('mission_control.fps_encoded')
  })

  it('takes only real numbers as readings, keeping a measured zero', () => {
    for (const value of [false, true, '', ' ', '0', '12', [], [5], {}, null, undefined, NaN, Infinity]) {
      expect(reading(value), JSON.stringify(value) ?? String(value)).toBeNull()
    }
    expect(reading(0)).toBe(0)
    expect(reading(-0.5)).toBe(-0.5)
    expect(reading(12.25)).toBe(12.25)
    // Malformed values from a host are gaps, never a measured 0.
    const malformed = live({ streaming: true, fps: ' ', latency_ms: false, bitrate_kbps: [], encode_time_ms: '' })
    for (const cell of malformed) expect(cell.value, cell.key).toBe(MISSING)
  })

  it('grades quality only from a complete set of observed inputs', () => {
    const complete = { streaming: true, fps: 60, latency_ms: 5, encode_time_ms: 2,
      network_verdict: { loss_pct: 0, loss_state: 'clean', window_seconds: 20 } }
    const graded = live(complete).find(cell => cell.key === 'quality')
    // A fully measured stream keeps the existing grade and score.
    expect(graded).toMatchObject({ value: String(buildQualityScore(complete)), grade: buildQualityGrade(buildQualityScore(complete)) })
    expect(graded.missing).toBeUndefined()
    // Frame rate alone would score A 100 in the shared model; the stage does not claim it.
    const fpsOnly = live({ streaming: true, fps: 120 }).find(cell => cell.key === 'quality')
    expect(buildQualityScore({ streaming: true, fps: 120 })).toBe(100)
    expect(fpsOnly).toMatchObject({ value: MISSING, grade: '', missing: true })
    expect(fpsOnly.sub).toBe('mission_control.quality_waiting {"inputs":"mission_control.input_latency, mission_control.input_encode, mission_control.input_loss"}')
    for (const gap of ['fps', 'latency_ms', 'encode_time_ms', 'network_verdict']) {
      const partial = { ...complete }
      delete partial[gap]
      const quality = live(partial).find(cell => cell.key === 'quality')
      expect(quality.value, gap).toBe(MISSING)
      expect(quality.grade, gap).toBe('')
    }
  })

  it('blanks every reading while the stream status is unavailable', () => {
    const cells = unavailableSignalCells({ t })
    expect(cells).toHaveLength(6)
    expect(cells.every(cell => cell.value === MISSING && cell.sub === 'mission_control.not_reported')).toBe(true)
    expect(cells.some(cell => 'data-live-summary-metric' in cell.attrs)).toBe(false)
  })

  it('summarises idle readiness without inventing unread values', () => {
    const unread = idleSignalCells({ readiness: { label: 'Waiting for host checks', tone: 'text-storm', passing: 0, total: 6 }, t })
    const byKey = Object.fromEntries(unread.map(cell => [cell.key, cell]))
    expect(byKey.paired).toMatchObject({ value: MISSING, sub: 'mission_control.not_read' })
    expect(byKey.mode).toMatchObject({ value: MISSING, sub: 'mission_control.not_read' })
    expect(byKey.gpu).toMatchObject({ value: MISSING, sub: 'mission_control.telemetry_warming' })
    expect(byKey.last).toBeUndefined()
    const read = idleSignalCells({ readiness: { label: 'Ready for clients', tone: 'text-success', passing: 6, total: 6 },
      clientsKnown: true, pairedClients: 2, modeLabel: 'Private Stream', liveTuningLabel: 'Live Tuning On — stable',
      gpu: { name: 'GPU', utilization_pct: 3 }, lastSession: { duration: '1 h 12 m', grade: 'A', detail: 'Deck' }, t })
    const known = Object.fromEntries(read.map(cell => [cell.key, cell]))
    expect(known.paired.value).toBe('2')
    expect(known.mode.value).toBe('Private Stream')
    expect(known.tuning.value).toBe('On — stable')
    // A known reading needs no caption repeating its label; only gaps explain themselves.
    expect([known.paired.sub, known.mode.sub, known.tuning.sub]).toEqual(['', '', ''])
    // No temperature reported: the reading says so instead of a number.
    expect(`${known.gpu.value}${known.gpu.unit}`).toBe('--°C')
    expect(known.gpu.sub).toBe('3% · --W')
    expect(known.last).toMatchObject({ value: '1 h 12 m', grade: 'A' })
    expect(known.readiness.sub).toBe('mission_control.checks_ready {"passing":6,"total":6}')
  })

  it('shows an unreported GPU temperature and power as dashes in their units, never as 0', () => {
    const gpuCell = gpu => idleSignalCells({ gpu, t }).find(cell => cell.key === 'gpu')
    // The live dashboard.spec AMD payload: utilization reported, temperature and power absent.
    const amd = gpuCell({ vendor: 'amd', name: 'AMD Radeon RX 7900 XTX', utilization_pct: 55, vram_used_mb: 8192, vram_total_mb: 24576 })
    expect(`${amd.value}${amd.unit}`).toBe('--°C')
    expect(amd.sub).toBe('55% · --W')
    expect(amd.sub).not.toMatch(/(^|\D)0W|NaN|undefined/)
    // A measured zero is a reading; malformed values are gaps.
    expect(gpuCell({ name: 'GPU', temperature_c: 40, utilization_pct: 0, power_draw_w: 0 }).sub).toBe('0% · 0W')
    const malformed = gpuCell({ name: 'GPU', temperature_c: '', utilization_pct: false, power_draw_w: [] })
    expect(`${malformed.value}${malformed.unit} ${malformed.sub}`).toBe('--°C --% · --W')
    expect(gpuCell({ name: 'GPU', temperature_c: 54.25, utilization_pct: 38, power_draw_w: 120.4 }).sub).toBe('38% · 120W')
  })

  it('features the most recently launched library title', () => {
    expect(featuredLibraryApp([])).toBeNull()
    expect(featuredLibraryApp([{ uuid: 'a', name: 'A', 'last-launched': 1700000000 }]).launchedAt).toBe(1700000000000)
    expect(featuredLibraryApp([{ uuid: 'a', name: 'A' }]).launchedAt).toBe(0)
  })

  it('features a title picked from the shelf while it is still there', () => {
    const shelf = [{ uuid: 'a', name: 'A', 'last-launched': 3 }, { uuid: 'b', name: 'B', 'last-launched': 2 }, { uuid: 'c', name: 'C' }]
    expect(featuredLibraryApp(shelf, 'b')).toMatchObject({ app: { uuid: 'b' }, launchedAt: 2000, chosen: true })
    // A pick without a launch record still reads as next up rather than "from your library".
    expect(featuredLibraryApp(shelf, 'c')).toMatchObject({ app: { uuid: 'c' }, launchedAt: 0, chosen: true })
    // A pick that left the shelf falls back to the most recent title.
    expect(featuredLibraryApp(shelf, 'gone')).toMatchObject({ app: { uuid: 'a' }, chosen: false })
    expect(featuredLibraryApp(shelf)).toMatchObject({ app: { uuid: 'a' }, chosen: false })
  })
})
