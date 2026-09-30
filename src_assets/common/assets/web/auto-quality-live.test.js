import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import {
  AUTO_QUALITY_HOST_STATES,
  autoQualityHostStateKey,
  autoQualityHostTone,
  buildLiveAutoQualityRows,
} from './auto-quality-live'

const en = JSON.parse(readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'),
  'utf8',
))
const t = (key, params = {}) => {
  const value = key.split('.').reduce((node, part) => (node ? node[part] : undefined), en)
  if (typeof value !== 'string') return key
  return value.replace(/\{(\w+)\}/g, (match, name) => (name in params ? String(params[name]) : match))
}

describe('live Auto Quality strip', () => {
  it('maps every host state to a locale key and a tone', () => {
    for (const state of AUTO_QUALITY_HOST_STATES) {
      expect(en.config[`av_auto_quality_live_state_${state}`]).toBeTypeOf('string')
      expect(['pass', 'warning', 'neutral']).toContain(autoQualityHostTone(state))
    }
    expect(autoQualityHostStateKey({ state: 'holding' })).toBe('holding')
    expect(autoQualityHostStateKey({ state: 'something_new' })).toBe('off')
    expect(autoQualityHostStateKey(null)).toBe('off')
    expect(autoQualityHostTone('holding')).toBe('pass')
    expect(autoQualityHostTone('blocked')).toBe('warning')
    expect(autoQualityHostTone('off')).toBe('neutral')
  })

  it('renders the host snapshot as four status rows', () => {
    const rows = buildLiveAutoQualityRows({
      autoQuality: {
        state: 'recovering_bitrate',
        blocked_reason: 'none',
        summary: 'Bitrate is recovering after packet loss.',
        live_bitrate_kbps: 18500,
        target_bitrate_kbps: 30000,
        relaunch_required: false,
      },
      tuning: {
        adaptive_bitrate_active: true,
        adaptive_min_bitrate_kbps: 10000,
        adaptive_base_bitrate_kbps: 40000,
        // The host still reports the ceiling it holds, which can sit above the
        // client's request; live tuning never climbs past that request.
        adaptive_max_bitrate_kbps: 100000,
        adaptive_rtt_ewma_ms: 7.6,
        // A percentage, as the host reports it: the loss note must not scale it again.
        adaptive_packet_loss_ewma: 0.0123,
        network_loss_pct: 1.23,
      },
    }, t)

    expect(rows.map((row) => row.label)).toEqual(['Host state', 'Live bitrate', 'Network', 'Target'])
    expect(rows[0]).toMatchObject({ value: 'Recovering bitrate', note: 'Bitrate is recovering after packet loss.' })
    expect(rows[1]).toMatchObject({ value: '18.5 Mbps', note: 'Adaptive range 10 Mbps to 40 Mbps' })
    expect(rows[2]).toMatchObject({ value: '8 ms round trip', note: '1.2% of video frames lost over 20 s' })
    expect(rows[3]).toMatchObject({ value: '30 Mbps', note: 'Applied live' })
  })

  it('shows no loss until the host has judged some, rather than a zero it never measured', () => {
    const rows = buildLiveAutoQualityRows({
      autoQuality: { state: 'active', live_bitrate_kbps: 20000 },
      tuning: { adaptive_bitrate_active: true, adaptive_rtt_ewma_ms: 5, adaptive_packet_loss_ewma: 0.078, network_loss_pct: null },
    }, t)

    expect(rows[2]).toMatchObject({ value: '5 ms round trip', note: '' })
  })

  it('degrades to "Not reported" instead of NaN when the host is silent', () => {
    const rows = buildLiveAutoQualityRows({ autoQuality: { state: 'off' }, tuning: null }, t)

    expect(rows[0]).toMatchObject({ value: 'Off', note: '' })
    expect(rows[1]).toMatchObject({ value: 'Not reported', note: '' })
    expect(rows[2]).toMatchObject({ value: 'No stream running', note: '' })
    expect(rows[3]).toMatchObject({ value: 'Not reported', note: 'Applied live' })
    expect(JSON.stringify(rows)).not.toContain('NaN')
  })

  it('does not dress an idle host in zero-valued network numbers', () => {
    const rows = buildLiveAutoQualityRows({
      autoQuality: { state: 'off', blocked_reason: 'none', live_bitrate_kbps: 0 },
      tuning: { adaptive_bitrate_active: false, adaptive_target_bitrate_kbps: 20000, adaptive_rtt_ewma_ms: 0, adaptive_packet_loss_ewma: 0, adaptive_min_bitrate_kbps: 2000, adaptive_max_bitrate_kbps: 161000, adaptive_base_bitrate_kbps: 161000 },
    }, t)

    expect(rows[0]).toMatchObject({ value: 'Off', note: '' })
    // The host still reports the last stream's base after it ends, but with no
    // stream there is no client request, so there is no range to show.
    expect(rows[1]).toMatchObject({ value: 'Not reported', note: '' })
    expect(rows[2]).toMatchObject({ value: 'No stream running', note: '' })
  })

  it('names a blocked state by its reason and a pending target by relaunch', () => {
    const rows = buildLiveAutoQualityRows({
      autoQuality: { state: 'blocked', blocked_reason: 'Mirror Desktop cannot change bitrate live.', target_bitrate_kbps: 25000, relaunch_required: true },
      tuning: { adaptive_target_bitrate_kbps: 25000, adaptive_bitrate_active: true },
    }, t)

    expect(rows[0]).toMatchObject({ value: 'Blocked', note: 'Mirror Desktop cannot change bitrate live.' })
    expect(rows[1].value).toBe('25 Mbps')
    expect(rows[3].note).toBe('Applies at the next launch')
  })
})
