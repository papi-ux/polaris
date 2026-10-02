import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

import HomeView from './views/HomeView.vue'

const reported = vi.hoisted(() => ({ kmsCapture: null, runningBinary: null }))

vi.mock('./composables/useSystemStats', async () => {
  const { ref } = await import('vue')
  return {
    useSystemStats: () => ({
      gpu: ref(null),
      displays: ref([]),
      audio: ref(null),
      sessionType: ref(null),
      displaySession: ref(null),
      gameModeHost: ref(null),
      kmsCapture: ref(reported.kmsCapture),
      runningBinary: ref(reported.runningBinary),
      loading: ref(false),
    }),
  }
})

const enLocale = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))

const i18n = {
  t(key, params = {}) {
    const message = key.split('.').reduce((node, part) => node?.[part], enLocale)
    if (typeof message !== 'string') return key
    return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
  },
}

async function hostNow(kmsCapture, runningBinary = { path: '/usr/libexec/polaris/polaris-kms' }) {
  reported.kmsCapture = kmsCapture
  reported.runningBinary = runningBinary
  vi.spyOn(console, 'error').mockImplementation(() => {})
  globalThis.fetch = vi.fn(async () => ({ ok: false, status: 404, json: async () => ({}) }))
  const wrapper = shallowMount(HomeView, {
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t },
      stubs: { 'router-link': true },
    },
  })
  await flushPromises()
  const row = wrapper.find('[data-kms-capture]')
  const result = row.exists() ?
    {
      text: row.text(),
      status: row.find('[data-kms-capture-status]').text(),
      command: row.find('[data-kms-capture-command]').exists() ? row.find('[data-kms-capture-command] pre').text() : null,
      link: row.find('[data-kms-capture-link]').exists() ? row.find('[data-kms-capture-link]').attributes('to') : null,
      readout: row.findAll('[data-capture-readout] > div').map((item) => `${item.find('dt').text()}: ${item.find('dd').text()}`),
    } :
    null
  wrapper.unmount()
  return result
}

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

describe('System page Capture row', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    reported.kmsCapture = null
    reported.runningBinary = null
  })

  it('shows a portal host running the helper as KMS not in use, with how to switch', async () => {
    const row = await hostNow(portalHost)

    expect(row.status).toBe('Not in use')
    expect(row.text).toContain('KMS not in use')
    expect(row.text).toContain('Mirror Desktop captures through the portal, so KMS is not used and needs nothing.')
    expect(row.text).toContain('gave up CAP_SYS_ADMIN at startup on purpose')
    expect(row.text).toContain('choose KMS for Force a Specific Capture Method under Settings, Advanced, then restart Polaris.')
    expect(row.text).toContain('Choosing a stream mode in Settings sets capture again, Mirror Desktop to the portal, so choose KMS after the mode.')
    expect(row.text).toContain('/usr/libexec/polaris/polaris-kms · Started without capabilities, for the portal and KWin')
    expect(row.text).not.toMatch(/Action needed|Reinstall|Helper without/)
    expect(row.command).toBeNull()
    expect(row.link).toBe('/config#capture')
    expect(row.readout).toEqual([
      'Capture setting: Portal',
      'Stream mode: Mirror Desktop, through Portal',
      'Last stream: Portal (KWin output), for Pixel 10 Pro',
    ])
  })

  it('shows a host set to KMS that holds the capability as ready', async () => {
    const row = await hostNow({
      ...portalHost,
      state: 'ready',
      route_kind: 'kms',
      capture: 'kms',
      capture_setting: 'kms',
      route: 'kms',
      cap_sys_admin: true,
      capability_set_aside: false,
      observed: null,
    })

    expect(row.status).toBe('Ready')
    expect(row.text).toContain('KMS ready')
    expect(row.text).toContain('CAP_SYS_ADMIN permitted')
    expect(row.readout).toContain('Last stream: None since Polaris started')
    expect(row.command).toBeNull()
  })

  it('gives a host set to KMS that has not turned it on the command to copy', async () => {
    const row = await hostNow({
      ...portalHost,
      state: 'not_enabled',
      route_kind: 'kms',
      capture: 'kms',
      capture_setting: 'kms',
      route: 'kms',
      capability_set_aside: false,
      running_helper: false,
    }, { path: '/usr/bin/polaris-1.4.13' })

    expect(row.status).toBe('Action needed')
    expect(row.text).toContain('KMS installed, not turned on')
    expect(row.command).toBe('sudo -H polaris --setup-host --enable-kms')
    expect(row.text).toContain('Copy command')
    expect(row.text).toContain('/usr/bin/polaris-1.4.13 · No CAP_SYS_ADMIN')
  })

  it('stays out of the way of a host that reports nothing', async () => {
    expect(await hostNow(null, null)).toBeNull()
  })
})
