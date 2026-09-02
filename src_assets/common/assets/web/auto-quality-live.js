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
  const liveBitrate = formatMbps(autoQuality?.live_bitrate_kbps) || formatMbps(tuning?.adaptive_target_bitrate_kbps)
  const minMbps = formatMbps(tuning?.adaptive_min_bitrate_kbps)
  const maxMbps = formatMbps(tuning?.adaptive_max_bitrate_kbps)
  const rtt = formatNumber(tuning?.adaptive_rtt_ewma_ms, 0)
  const loss = formatNumber(Number(tuning?.adaptive_packet_loss_ewma) * 100, 1)
  const target = formatMbps(autoQuality?.target_bitrate_kbps)

  return [
    {
      label: t('config.av_auto_quality_live_row_state'),
      value: t(`config.av_auto_quality_live_state_${stateKey}`),
      note: String(autoQuality?.blocked_reason || autoQuality?.summary || ''),
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
      value: rtt ? t('config.av_auto_quality_live_rtt_value', { rtt }) : unknown,
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
