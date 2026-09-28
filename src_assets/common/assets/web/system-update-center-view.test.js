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
      kmsCapture: ref(null),
      runningBinary: ref(null),
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

const LATEST_URL = 'https://api.github.com/repos/papi-ux/polaris/releases/latest'
const RELEASES_URL = 'https://api.github.com/repos/papi-ux/polaris/releases'

function releaseOf(tag, prerelease, names) {
  return {
    tag_name: tag,
    name: `Polaris ${tag}`,
    prerelease,
    draft: false,
    html_url: `https://github.com/papi-ux/polaris/releases/tag/${tag}`,
    assets: names.map((name) => ({
      name,
      browser_download_url: `https://github.com/papi-ux/polaris/releases/download/${tag}/${name}`,
      digest: `sha256:${name.includes('-kms-') ? 'kms' : 'base'}`,
    })),
  }
}

// The Update Center papi saw on 2026-09-27: 1.4.13 installed with the helper, betas included, and
// only an older beta published.
async function updateCenter({ kmsHelper }) {
  vi.spyOn(console, 'error').mockImplementation(() => {})
  const stable = releaseOf('v1.4.13', false, ['Polaris-fedora44-x86_64.rpm', 'Polaris-kms-fedora44-x86_64.rpm'])
  const oldBeta = releaseOf('v1.4.13-beta.2', true, ['Polaris-fedora44-x86_64.rpm', 'Polaris-kms-fedora44-x86_64.rpm'])
  globalThis.fetch = vi.fn(async (url) => {
    const bodies = {
      './api/config': { version: '1.4.13', platform: 'linux', notify_pre_releases: 'enabled', configuration_revision: 'r1' },
      './api/update-status': { version: '1.4.13', platform: 'linux', distro: { id: 'fedora', version_id: '44' }, kms_helper_installed: kmsHelper },
      [LATEST_URL]: stable,
      [RELEASES_URL]: [oldBeta, stable],
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
  const summary = wrapper.find('.system-update-state-summary').element.parentElement.textContent
  const kmsPackage = wrapper.find('[data-update-kms-package]')
  const result = {
    summary,
    kmsPackage: kmsPackage.exists() ? kmsPackage.text() : null,
    details: wrapper.find('[data-update-center-details]').text(),
  }
  wrapper.unmount()
  return result
}

describe('Update Center with betas included', () => {
  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('says no newer beta is published beside the status, and lists the helper package too', async () => {
    const view = await updateCenter({ kmsHelper: true })

    expect(view.summary).toContain('Current release · No beta newer than 1.4.13 is published yet, so stable 1.4.13 is the newest build.')
    expect(view.kmsPackage).toBe('Polaris-kms-fedora44-x86_64.rpm')
    expect(view.details).toContain('Polaris-fedora44-x86_64.rpm')
    expect(view.details).toContain('The polaris-kms helper installed on this host. It is built for this exact Polaris, so the install command takes both.')
    // The install command already downloads it, so the card lists it without a second download.
    expect(view.details).toContain('Polaris-kms-fedora44-x86_64.rpm')
    expect(view.details).not.toContain('Download polaris-kms')
  })

  it('names only Polaris on a host without the helper', async () => {
    const view = await updateCenter({ kmsHelper: false })

    expect(view.kmsPackage).toBeNull()
    expect(view.details).not.toContain('Polaris-kms-')
  })
})
