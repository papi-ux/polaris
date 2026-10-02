import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

import {
  CAPTURE_SETTING_ROUTE,
  KMS_ENABLE_COMMAND,
  KMS_RESTART_COMMAND,
  KMS_SETUP_COMMAND,
  KMS_START_SERVICE_COMMAND,
  KMS_STATES,
  describeKmsCapture,
} from './kms-capture-readiness.js'
import { applyStreamDisplayModeToConfig } from './client-settings-sync.js'

const enLocale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))

function t(key, params = {}) {
  const message = key.split('.').reduce((node, part) => node?.[part], enLocale)
  if (typeof message !== 'string') return key
  return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
}

const HELPER = '/usr/libexec/polaris/polaris-kms'

// papi's host on 2026-09-27: capture = portal in Mirror Desktop, the service running the helper, and
// Polaris without CAP_SYS_ADMIN because it gave it up for the portal at startup.
const portalHost = {
  state: 'not_in_use',
  route_kind: 'other',
  capture: 'portal',
  capture_setting: 'portal',
  route: 'portal',
  stream_mode: 'desktop_display',
  stream_mode_label: 'Mirror Desktop',
  kms_possible_in_mode: true,
  cap_sys_admin: false,
  capability_set_aside: true,
  helper_installed: true,
  running_helper: true,
  in_service: true,
  lingering: false,
  account: 'papi',
  observed: { when: 'last_session', client_name: 'Pixel 10 Pro', opened: 'portal', route: 'portal_kwin_node' },
}

const kmsHost = {
  ...portalHost,
  state: 'ready',
  route_kind: 'kms',
  capture: 'kms',
  capture_setting: 'kms',
  route: 'kms',
  cap_sys_admin: true,
  capability_set_aside: false,
  observed: { when: 'streaming', client_name: 'Steam Deck', opened: 'kms', route: 'kms' },
}

function describe_(kms, binary = HELPER) {
  return describeKmsCapture(kms, t, binary)
}

function readout(view) {
  return Object.fromEntries(view.readout.map(({ label, value }) => [label, value]))
}

describe('KMS capture readiness wording', () => {
  it('stays out of the way of a host that reports nothing', () => {
    expect(describe_(null)).toBeNull()
    expect(describe_(undefined)).toBeNull()
    expect(describe_({ state: 'something_newer' })).toBeNull()
  })

  it('reads a portal host running the helper as KMS not in use, never as a broken helper', () => {
    const view = describe_(portalHost)

    expect(view.state).toBe('not_in_use')
    expect(view.tone).toBe('off')
    expect(view.toneClass).toBe('system-telemetry-state-muted')
    expect(t(view.statusKey)).toBe('Not in use')
    expect(t(view.headlineKey)).toBe('KMS not in use')
    expect(view.detail).toBe(
      'Mirror Desktop captures through the portal, so KMS is not used and needs nothing. ' +
      'Polaris runs the polaris-kms helper and gave up CAP_SYS_ADMIN at startup on purpose, because the portal and KWin refuse a program that holds it. Nothing is broken. ' +
      'To capture through KMS instead, choose KMS for Force a Specific Capture Method under Settings, Advanced, then restart Polaris. ' +
      'Choosing a stream mode in Settings sets capture again, Mirror Desktop to the portal, so choose KMS after the mode.',
    )
    expect(view.command).toBe('')
    expect(view.link).toEqual({ to: CAPTURE_SETTING_ROUTE, labelKey: 'index.kms_open_capture_setting' })
    expect(view.capability).toBe('Started without capabilities, for the portal and KWin')
    expect(view.detail).not.toMatch(/reinstall|without CAP_SYS_ADMIN|Action needed/i)
    expect(readout(view)).toEqual({
      'Capture setting': 'Portal',
      'Stream mode': 'Mirror Desktop, through Portal',
      'Last stream': 'Portal (KWin output), for Pixel 10 Pro',
    })

    // kwin is the portal under another name, and the setting keeps the name it was given.
    expect(readout(describe_({ ...portalHost, capture_setting: 'kwin' }))['Capture setting']).toBe('KWin (portal)')
  })

  it('says Autodetect passes over KMS in a mode that starts Polaris without capabilities', () => {
    const view = describe_({ ...portalHost, capture: '', capture_setting: '', route: '' })
    expect(view.detail).toContain('Capture is on Autodetect, which passes over KMS in Mirror Desktop, so KMS is not used and needs nothing.')
    expect(view.detail).toContain('Nothing is broken.')
    expect(readout(view)['Capture setting']).toBe('Autodetect')
  })

  it('tells a host without the package how to switch, without a warning', () => {
    const view = describe_({
      state: 'not_in_use',
      route_kind: 'automatic',
      capture: '',
      capture_setting: '',
      route: '',
      stream_mode_label: 'Mirror Desktop',
      kms_possible_in_mode: true,
      cap_sys_admin: false,
      capability_set_aside: false,
      helper_installed: false,
      running_helper: false,
    }, '/usr/bin/polaris-1.4.13')
    expect(view.tone).toBe('off')
    expect(view.detail).toContain('without the polaris-kms package its search uses another backend')
    expect(view.detail).toContain(`run ${KMS_ENABLE_COMMAND}, and choose KMS`)
    expect(view.detail).not.toContain('gave up CAP_SYS_ADMIN')
    expect(view.capability).toBe('No CAP_SYS_ADMIN')
    expect(view.binary).toBe('/usr/bin/polaris-1.4.13')
    expect(readout(view)['Last stream']).toBe('None since Polaris started')
  })

  it('tells the truth about what choosing a stream mode in Settings does to capture', () => {
    // The row says to choose KMS after the mode, because the mode picker writes capture itself.
    expect(applyStreamDisplayModeToConfig({ capture: 'kms' }, 'desktop_display').capture).toBe('portal')
    expect(applyStreamDisplayModeToConfig({ capture: 'kms' }, 'headless_dongle').capture).toBe('kms')
    expect(applyStreamDisplayModeToConfig({ capture: 'kms' }, 'headless_stream').capture).toBe('wlr')
    expect(t('index.kms_mode_resets_capture')).toContain('Mirror Desktop to the portal')
  })

  it('names the modes that use KMS where the host mode never does', () => {
    const privateStream = describe_({ ...portalHost, route: 'wlr', stream_mode: 'headless_stream', stream_mode_label: 'Private Stream', kms_possible_in_mode: false })
    expect(privateStream.detail).toContain('Private Stream captures through wlroots, so KMS is not used and needs nothing.')
    expect(privateStream.detail).toContain('Private Stream captures its own display without KMS. Mirror Desktop and the Headless Dongle capture the real screen through KMS when capture is set to KMS.')
    expect(privateStream.link).toBeNull()

    const setAside = describe_({ ...kmsHost, state: 'mode_sets_kms_aside', route_kind: 'other', route: 'wlr', stream_mode_label: 'Private Stream', kms_possible_in_mode: false })
    expect(t(setAside.headlineKey)).toBe('KMS set aside by the stream mode')
    expect(setAside.tone).toBe('off')
    expect(setAside.detail).toBe('Capture is set to KMS, but Private Stream captures its own display through wlroots, so KMS is not used in it. Mirror Desktop and the Headless Dongle capture the real screen through KMS. Choosing a stream mode in Settings sets capture again, Mirror Desktop to the portal, so choose KMS after the mode.')
  })

  it('shows a host set to KMS that holds the capability as ready, and what it streams with', () => {
    const view = describe_(kmsHost)
    expect(view.tone).toBe('ready')
    expect(t(view.statusKey)).toBe('Ready')
    expect(t(view.headlineKey)).toBe('KMS ready')
    expect(view.detail).toBe('Mirror Desktop captures through KMS, and Polaris holds the CAP_SYS_ADMIN it needs to read the screen.')
    expect(view.capability).toBe('CAP_SYS_ADMIN permitted')
    expect(readout(view)['Streaming now']).toBe('KMS, for Steam Deck')
  })

  it('says Autodetect can reach KMS without nudging anyone to force it', () => {
    const view = describe_({ ...kmsHost, state: 'automatic', route_kind: 'automatic', capture: '', capture_setting: '', route: '' })
    expect(view.tone).toBe('available')
    expect(t(view.statusKey)).toBe('Available')
    expect(view.detail).toBe('Capture is on Autodetect, which tries NvFBC and wlroots before KMS. Polaris holds CAP_SYS_ADMIN, so KMS works whenever the search reaches it.')
    expect(view.link).toBeNull()
  })

  it('says KMS found nothing, and where to look, when capture fell back from it', () => {
    const view = describe_({ ...kmsHost, state: 'kms_found_nothing', route: '', substitute: 'portal' })
    expect(view.tone).toBe('action')
    expect(view.detail).toContain('so capture uses the portal instead')
    expect(view.link.to).toBe('/troubleshooting#logs')
  })

  it('gives each setup step its own command, and a warning only where capture asks for KMS', () => {
    const base = { ...kmsHost, cap_sys_admin: false, running_helper: false }
    const cases = [
      ['not_installed', KMS_ENABLE_COMMAND, 'KMS needs the polaris-kms package'],
      ['helper_broken', '', 'KMS helper without its capability'],
      ['no_capability', '', 'KMS helper runs without CAP_SYS_ADMIN'],
      ['not_enabled', KMS_ENABLE_COMMAND, 'KMS installed, not turned on'],
      ['not_in_group', KMS_ENABLE_COMMAND, 'KMS waits for the polaris-kms group'],
      ['waiting_for_login', KMS_SETUP_COMMAND, 'KMS waits for a new login'],
      ['finish_setup', KMS_SETUP_COMMAND, 'KMS setup has one step left'],
      ['outside_service', KMS_START_SERVICE_COMMAND, 'Polaris started outside its service'],
      ['restart_needed', KMS_RESTART_COMMAND, 'KMS waits for a restart'],
    ]
    for (const [state, command, headline] of cases) {
      const view = describe_({ ...base, state })
      expect(view.command, state).toBe(command)
      expect(t(view.headlineKey), state).toBe(headline)
      expect(view.tone, state).toBe('action')
      expect(t(view.statusKey), state).toBe('Action needed')
    }

    // Someone who installed the package on a host that searches sees the step, without an alarm.
    const searching = describe_({ ...base, state: 'not_enabled', route_kind: 'automatic', capture: '', route: '' })
    expect(searching.tone).toBe('off')
    expect(t(searching.statusKey)).toBe('Not set up')
    expect(searching.command).toBe(KMS_ENABLE_COMMAND)

    // Reinstalling is the advice in exactly one state.
    for (const state of KMS_STATES) {
      const view = describe_({ ...base, state })
      expect(/reinstall/i.test(view.detail), state).toBe(state === 'helper_broken')
    }
  })

  it('names the login that brings the group in, and the reboot where lingering keeps the manager', () => {
    const waiting = { ...kmsHost, state: 'waiting_for_login', cap_sys_admin: false, account: 'papi' }
    expect(describe_(waiting).detail).toBe('papi is in the polaris-kms group, but its session started before that, and a session picks up its groups only when it starts. End every session of papi, SSH logins included, or reboot, then run:')
    expect(describe_({ ...waiting, lingering: true }).detail).toContain('only a reboot brings the group in. Reboot, then run:')
    expect(describe_({ ...waiting, state: 'finish_setup' }).detail).toContain('The session of papi holds the polaris-kms group now')
    expect(describe_({ ...waiting, state: 'not_in_group' }).detail).toContain('and papi is not one')
  })

  it('names an explicit capture that found nothing, rather than calling it Autodetect', () => {
    // capture = wlr in Mirror Desktop on KDE or GNOME captures nothing, so the automatic search
    // stands in and the route reads empty. The row said "Capture is on Autodetect" beside
    // "Capture setting: wlroots".
    const searching = describe_({ ...kmsHost, state: 'automatic', route_kind: 'automatic', capture: 'wlr', capture_setting: 'wlr', route: '' })
    expect(searching.detail).toBe(
      'Capture is set to wlroots, which found nothing to capture in Mirror Desktop, so the automatic search stands in for it. ' +
      'The search tries NvFBC and wlroots before KMS. Polaris holds CAP_SYS_ADMIN, so KMS works whenever the search reaches it.',
    )
    expect(readout(searching)['Capture setting']).toBe('wlroots')

    const portalFoundNothing = describe_({ ...portalHost, route: '' })
    expect(portalFoundNothing.detail).toContain(
      'Capture is set to Portal, which found nothing to capture in Mirror Desktop, so the automatic search stands in for it. ' +
      'The search passes over KMS in Mirror Desktop, so KMS is not used and needs nothing.',
    )

    const withoutPackage = describe_({
      ...portalHost,
      route_kind: 'automatic',
      capture: 'wlr',
      capture_setting: 'wlr',
      route: '',
      capability_set_aside: false,
      helper_installed: false,
      running_helper: false,
    })
    expect(withoutPackage.detail).toContain(
      'Capture is set to wlroots, which found nothing to capture in Mirror Desktop, so the automatic search stands in for it. ' +
      'Without the polaris-kms package, the search uses another backend, so KMS needs nothing.',
    )

    for (const view of [searching, portalFoundNothing, withoutPackage]) {
      expect(view.detail).not.toContain('Autodetect')
    }
  })

  it('keeps the restart advice for a helper an update replaced under the running service', () => {
    // After a package update the running helper reads "(deleted)", so it is not the file on disk
    // now. It is still the helper: switching takes capture = kms and a restart, not the package again.
    const view = describe_({ ...portalHost, running_helper: false, running_replaced_helper: true })
    expect(view.detail).toContain('gave up CAP_SYS_ADMIN at startup on purpose')
    expect(view.detail).toContain('choose KMS for Force a Specific Capture Method under Settings, Advanced, then restart Polaris.')
    expect(view.detail).not.toContain('install the polaris-kms package')
  })

  it('does not ask a host that has the package to install it', () => {
    const view = describe_({ ...portalHost, capture: '', capture_setting: '', route: '', running_helper: false, in_service: false })
    expect(view.detail).toContain(`run ${KMS_ENABLE_COMMAND} and do what it prints, then choose KMS`)
    expect(view.detail).not.toContain('install the polaris-kms package')
    expect(view.detail).not.toContain('gave up CAP_SYS_ADMIN')
  })

  it('says a stream runs rather than none since Polaris started', () => {
    const idle = { ...kmsHost, observed: null }
    expect(readout(describe_({ ...idle, streams_running: 0 }))['Last stream']).toBe('None since Polaris started')
    expect(readout(describe_({ ...idle, streams_running: 1 }))['Streaming now']).toBe('Capture not open yet')
    expect(readout(describe_({ ...idle, streams_running: 2 }))['Streaming now']).toBe('2 clients at once')
    expect(readout(describe_({ ...kmsHost, streams_running: 1 }))['Streaming now']).toBe('KMS, for Steam Deck')
  })

  it('names the account in the console language when the host sends none', () => {
    const view = describe_({ ...kmsHost, state: 'not_in_group', cap_sys_admin: false, account: '' })
    expect(view.detail).toContain('and this account is not one')
    expect(view.detail).not.toMatch(/\{account\}/)
  })

  it('has copy for every state, claims KMS capture only when ready, and keeps to the writing rules', () => {
    // Each state with the routes the host sends it for: KMS not in use on a route that is not KMS,
    // and everything else on a route that asks for KMS or searches.
    const otherRoutes = [portalHost, { ...portalHost, route_kind: 'automatic', route: '' }, { ...portalHost, route: '' }, { ...portalHost, route: 'wlr', kms_possible_in_mode: false }]
    const kmsRoutes = [kmsHost, { ...kmsHost, route_kind: 'automatic', route: '' }]
    for (const state of KMS_STATES) {
      const variants = ['not_in_use', 'mode_sets_kms_aside'].includes(state) ? otherRoutes : kmsRoutes
      for (const variant of variants) {
        const view = describe_({ ...variant, state, substitute: 'portal' })
        expect(t(view.headlineKey), state).not.toBe(view.headlineKey)
        expect(view.detail, state).not.toMatch(/index\./)
        expect(view.detail, state).not.toMatch(/\{\w+\}/)
        if (state !== 'ready') {
          expect(view.detail, state).not.toMatch(/\bcaptures through KMS\b/)
        }
      }
    }
    const copy = Object.entries(enLocale.index).filter(([key]) => key.startsWith('kms_') || key.startsWith('capture_'))
    expect(copy.length).toBeGreaterThan(60)
    for (const [key, text] of copy) {
      expect(text, key).not.toMatch(/[–—]| - /)
    }
  })
})
