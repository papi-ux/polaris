// What the General tab says under Allow Clients To Sleep This Host, from GET /api/host/power:
// whether logind would suspend this host for Polaris, and how the last request ended. The
// owner learns that polkit would refuse before a client ever asks.

const CONFIGURATION_DOCS = 'https://papi-ux.com/docs/configuration/'

export const HOST_POWER_DOCS = Object.freeze({
  beforeYouTurnItOn: `${CONFIGURATION_DOCS}#before-you-turn-it-on`,
  polkit: `${CONFIGURATION_DOCS}#when-suspend-works-in-a-terminal-but-not-from-polaris`,
})

function readiness(power, t) {
  if (power.sleep_supported === true) {
    return {
      tone: 'ok',
      text: t('config.host_power_ready'),
      link: { href: HOST_POWER_DOCS.beforeYouTurnItOn, label: t('config.host_power_ready_link') },
    }
  }
  const message = String(power.sleep_blocked_message || '').trim()
  switch (power.sleep_blocked_reason) {
    case 'polkit_denied':
      return {
        tone: 'warning',
        text: t('config.host_power_polkit_denied'),
        link: { href: HOST_POWER_DOCS.polkit, label: t('config.host_power_polkit_link') },
      }
    case 'not_available':
      return { tone: 'warning', text: t('config.host_power_not_available') }
    case 'unsupported_platform':
      return { tone: 'warning', text: t('config.host_power_unsupported_platform') }
    case 'logind_unavailable':
      return { tone: 'warning', text: t('config.host_power_logind_unavailable', { message }) }
    default:
      return message ? { tone: 'warning', text: t('config.host_power_blocked', { message }) } : null
  }
}

/** The lines to show, readiness first; none until the host has answered. */
export function hostPowerLines(power, t) {
  if (!power || typeof power !== 'object') return []
  const lines = []
  const ready = readiness(power, t)
  if (ready) lines.push(ready)
  if (power.last_sleep_outcome === 'failed') {
    const message = String(power.last_sleep_message || '').trim()
    lines.push({
      tone: 'warning',
      text: message ? t('config.host_power_last_failed', { message }) : t('config.host_power_last_failed_plain'),
    })
  }
  return lines
}
