import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

import { HOST_POWER_DOCS, hostPowerLines } from './host-power-status.js'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')
const enLocale = JSON.parse(read('src_assets/common/assets/web/public/assets/locale/en.json'))

function t(key, params = {}) {
  const message = key.split('.').reduce((node, part) => node?.[part], enLocale)
  if (typeof message !== 'string') throw new Error(`missing locale key ${key}`)
  return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
}

const blocked = (reason, message = '') => ({ sleep_supported: false, sleep_blocked_reason: reason, sleep_blocked_message: message, last_sleep_outcome: 'none' })

describe('host power status on the General tab', () => {
  it('shows nothing until the host answers', () => {
    expect(hostPowerLines(null, t)).toEqual([])
    expect(hostPowerLines(undefined, t)).toEqual([])
  })

  it('says a host that can sleep can, and points at the wake checks', () => {
    const [line] = hostPowerLines({ sleep_supported: true, sleep_blocked_reason: '', last_sleep_outcome: 'none' }, t)
    expect(line.tone).toBe('ok')
    expect(line.text).toBe('Linux says this host can sleep. Make sure it wakes again before you rely on it.')
    expect(line.link.href).toBe(HOST_POWER_DOCS.beforeYouTurnItOn)
  })

  it('names why a host cannot sleep, in words, with the polkit fix one click away', () => {
    const polkit = hostPowerLines(blocked('polkit_denied', 'polkit wants interactive authentication'), t)[0]
    expect(polkit.tone).toBe('warning')
    expect(polkit.text).toContain('polkit asks for a password')
    expect(polkit.link.href).toBe(HOST_POWER_DOCS.polkit)

    expect(hostPowerLines(blocked('not_available'), t)[0].text).toBe('This host reports that it cannot suspend, so clients will not offer to sleep it.')
    expect(hostPowerLines(blocked('unsupported_platform'), t)[0].text).toBe('Host sleep only works on Linux hosts.')
    expect(hostPowerLines(blocked('logind_unavailable', 'logind did not answer.'), t)[0].text)
      .toBe('Polaris could not ask logind whether this host can sleep: logind did not answer.')
    expect(hostPowerLines(blocked('something_new', 'A reason from a newer host.'), t)[0].text)
      .toBe('This host cannot sleep from a client: A reason from a newer host.')
    expect(hostPowerLines(blocked('something_new'), t)).toEqual([])
  })

  it('says when the last request was accepted but the host never went down', () => {
    const lines = hostPowerLines({ sleep_supported: true, last_sleep_outcome: 'failed', last_sleep_message: 'A task refused to freeze.' }, t)
    expect(lines).toHaveLength(2)
    expect(lines[1]).toEqual({ tone: 'warning', text: 'The last sleep request did not put this host to sleep: A task refused to freeze.' })
    expect(hostPowerLines({ sleep_supported: true, last_sleep_outcome: 'failed' }, t)[1].text)
      .toBe('The last sleep request did not put this host to sleep.')
    expect(hostPowerLines({ sleep_supported: true, last_sleep_outcome: 'suspended' }, t)).toHaveLength(1)
  })

  it('links to headings the docs site really has', () => {
    const configuration = read('docs/configuration.md')
    expect(configuration).toContain('#### Before you turn it on')
    expect(configuration).toContain('#### When suspend works in a terminal but not from Polaris')
    expect(HOST_POWER_DOCS.beforeYouTurnItOn.endsWith('#before-you-turn-it-on')).toBe(true)
    expect(HOST_POWER_DOCS.polkit.endsWith('#when-suspend-works-in-a-terminal-but-not-from-polaris')).toBe(true)
  })

  it('reads the console endpoint and shows the lines under the setting', () => {
    const general = read('src_assets/common/assets/web/configs/tabs/General.vue')
    expect(general).toContain("fetch('./api/host/power'")
    expect(general).toContain('v-for="line in hostPowerLines(hostPower, $t)"')
    const route = read('src/confighttp.cpp')
    expect(route).toContain('server.resource["^/api/host/power$"]["GET"] = getHostPower;')
  })
})
