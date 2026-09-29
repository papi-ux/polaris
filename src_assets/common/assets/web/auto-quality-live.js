// Live Auto Quality strip: rows and tone from the host's auto_quality policy
// snapshot and the tuning block that ride the stats channel at 1 Hz.

export const AUTO_QUALITY_HOST_STATES = [
  'off',
  'blocked',
  'recovery_queued',
  'recovering_bitrate',
  'holding',
  'insufficient_signal',
  'active',
]

export function autoQualityHostStateKey(autoQuality) {
  const state = String(autoQuality?.state || '').trim()
  return AUTO_QUALITY_HOST_STATES.includes(state) ? state : 'off'
}

// Maps a host state onto the shared status tone vocabulary; `off` is neutral
// and keeps the page's manual tone rather than a status colour.
export function autoQualityHostTone(stateKey) {
  switch (stateKey) {
    case 'holding':
    case 'active':
      return 'pass'
    case 'off':
      return 'neutral'
    default:
      return 'warning'
  }
}

const formatMbps = (kbps) => {
  const value = Number(kbps)
  if (!Number.isFinite(value) || value <= 0) return ''
  const mbps = value / 1000
  return `${Number.isInteger(mbps) ? mbps : mbps.toFixed(1)} Mbps`
}

const formatNumber = (value, digits) => {
  const number = Number(value)
  return Number.isFinite(number) ? number.toFixed(digits) : ''
}

export function buildLiveAutoQualityRows({ autoQuality, tuning }, t) {
  const stateKey = autoQualityHostStateKey(autoQuality)
  const unknown = t('config.av_auto_quality_live_value_unknown')
  // The host reports EWMAs of zero while nothing streams; only a live session
  // makes the network numbers mean anything.
  const streaming = Number(autoQuality?.live_bitrate_kbps) > 0 || tuning?.adaptive_bitrate_active === true
  const liveBitrate = formatMbps(autoQuality?.live_bitrate_kbps) || (streaming ? formatMbps(tuning?.adaptive_target_bitrate_kbps) : '')
  const minMbps = formatMbps(tuning?.adaptive_min_bitrate_kbps)
  // The controller's base tops the range: the client's request, a later live
  // change, or Doctor's target while its step holds. Live tuning never climbs
  // above it, and adaptive_max_bitrate_kbps can sit above it and bounds nothing.
  // The base outlives the stream that set it, so an idle host shows no range.
  const maxMbps = streaming ? formatMbps(tuning?.adaptive_base_bitrate_kbps) : ''
  const rtt = streaming ? formatNumber(tuning?.adaptive_rtt_ewma_ms, 0) : ''
  // The loss Doctor quotes, already a percentage. Live Tuning acts on its own average instead,
  // adaptive_packet_loss_ewma, a percentage too; it was read as a fraction, so 0.078% showed as 7.8%.
  const loss = streaming && tuning?.network_loss_pct != null ? formatNumber(tuning.network_loss_pct, 1) : ''
  const target = formatMbps(autoQuality?.target_bitrate_kbps)
  const reason = String(autoQuality?.blocked_reason || '')
  const stateNote = reason && reason !== 'none' ? reason : String(autoQuality?.summary || '')

  return [
    {
      label: t('config.av_auto_quality_live_row_state'),
      value: t(`config.av_auto_quality_live_state_${stateKey}`),
      note: stateNote === 'none' ? '' : stateNote,
    },
    {
      label: t('config.av_auto_quality_live_row_bitrate'),
      value: liveBitrate || unknown,
      note: minMbps && maxMbps
        ? t('config.av_auto_quality_live_range_note', { min: minMbps, max: maxMbps })
        : '',
    },
    {
      label: t('config.av_auto_quality_live_row_network'),
      value: rtt ? t('config.av_auto_quality_live_rtt_value', { rtt }) : streaming ? unknown : t('config.av_auto_quality_live_not_streaming'),
      note: loss ? t('config.av_auto_quality_live_loss_note', { loss }) : '',
    },
    {
      label: t('config.av_auto_quality_live_row_target'),
      value: target || unknown,
      note: autoQuality?.relaunch_required === true
        ? t('config.av_auto_quality_live_target_relaunch')
        : t('config.av_auto_quality_live_target_live'),
    },
  ]
}
