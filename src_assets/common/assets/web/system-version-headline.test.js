import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

import HomeView from './views/HomeView.vue'

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

const LATEST_RELEASE_URL = 'https://api.github.com/repos/papi-ux/polaris/releases/latest'

// The System page as a host answers it: the running version from the host, the latest stable release
// from GitHub. The log tail and every other request fail, which the page already tolerates.
async function systemPage({ running, stable }) {
  vi.spyOn(console, 'error').mockImplementation(() => {})
  globalThis.fetch = vi.fn(async (url) => {
    const bodies = {
      './api/config': { version: running, platform: 'linux', notify_pre_releases: false },
      './api/update-status': { version: running, platform: 'linux', distro: { id: 'fedora', version_id: '44' } },
      [LATEST_RELEASE_URL]: {
        tag_name: stable,
        name: stable,
        prerelease: false,
        draft: false,
        html_url: `https://github.com/papi-ux/polaris/releases/tag/${stable}`,
        assets: [],
      },
    }
    if (!(url in bodies)) return { ok: false, status: 404, json: async () => ({}) }
    return { ok: true, status: 200, json: async () => bodies[url] }
  })
  const wrapper = shallowMount(HomeView, {
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t },
      stubs: { 'router-link': true },
    },
  })
  await flushPromises()
  const versionItem = wrapper.findAll('.system-status-item')
    .find((item) => item.find('.system-status-value').exists() && item.find('.system-status-value').text() === running)
  expect(versionItem, `no version headline for ${running}`).toBeDefined()
  const headline = versionItem.find('.system-status-copy').text()
  const updateCenter = wrapper.find('.system-update-state-summary').text()
  wrapper.unmount()
  return { headline, updateCenter }
}

describe('System page version headline', () => {
  afterEach(() => {
    vi.restoreAllMocks()
  })

  // The headline and the Update Center card read the same two versions. They have to agree that a beta
  // is older than the release it precedes, or the page says "Current public release" beside an offer.
  it('tells a beta host that the release it precedes is out', async () => {
    for (const running of ['1.4.14-beta.3', '1.4.14-rc.2']) {
      const page = await systemPage({ running, stable: 'v1.4.14' })
      expect(page.headline, running).toBe('New stable release available')
      expect(page.updateCenter, running).toBe('Update available')
    }
  })

  it('keeps a beta of the next release newer than the stable one', async () => {
    const page = await systemPage({ running: '1.4.14-beta.1', stable: 'v1.4.13' })
    expect(page.headline).toBe('Running newer than the latest stable tag')
    expect(page.updateCenter).toBe('Ahead of latest release')
  })

  it('calls the release itself current', async () => {
    const page = await systemPage({ running: '1.4.14', stable: 'v1.4.14' })
    expect(page.headline).toBe('Current public release')
    expect(page.updateCenter).toBe('Current release')
  })

  // The published 1.4.13 betas report 1.4.13 itself, so nothing on the page can tell them from the
  // release: docs/updates.md says so and gives the reinstall that replaces them.
  it('cannot tell a 1.4.13 beta from 1.4.13, and offers it 1.4.14', async () => {
    const same = await systemPage({ running: '1.4.13', stable: 'v1.4.13' })
    expect(same.headline).toBe('Current public release')
    expect(same.updateCenter).toBe('Current release')

    const next = await systemPage({ running: '1.4.13', stable: 'v1.4.14' })
    expect(next.headline).toBe('New stable release available')
    expect(next.updateCenter).toBe('Update available')
  })
})
