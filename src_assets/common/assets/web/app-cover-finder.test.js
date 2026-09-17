import { flushPromises, shallowMount } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'

import AppsView from './views/AppsView.vue'

const scannerState = {
  scanning: ref(false),
  importing: ref(false),
  steamGames: ref([]),
  lutrisGames: ref([]),
  heroicGames: ref([]),
  emulatorGames: ref([]),
  librarySources: ref([]),
  error: ref(null),
  scan: vi.fn(),
  importSelected: vi.fn(() => Promise.resolve(0)),
  toggleAll: vi.fn(),
}

vi.mock('./composables/useGameScanner', () => ({
  useGameScanner: () => scannerState,
}))

const i18n = {
  t(key) { return key },
}

const webSource = (path) => readFileSync(join(process.cwd(), 'src_assets/common/assets/web', path), 'utf8')

const HOLLOW_UUID = 'A1B2C3D4-0000-4000-8000-00000000C0DE'
const TOKEN_ONE = 'a'.repeat(32)
const TOKEN_TWO = 'b'.repeat(32)
const POSTER_TOKENS = ['c'.repeat(32), 'd'.repeat(32), 'e'.repeat(32)]

const hollowKnight = {
  name: 'Hollow Knight',
  uuid: HOLLOW_UUID,
  cmd: 'hollow-knight',
  'image-path': '',
}

const heroicLauncher = {
  name: 'Heroic Games Launcher',
  uuid: 'A1B2C3D4-0000-4000-8000-0000000E401C',
  source: 'heroic',
  cmd: '',
  detached: ['flatpak run com.heroicgameslauncher.hgl'],
}

function reply(status, body) {
  return Promise.resolve({ ok: status >= 200 && status < 300, status, json: () => Promise.resolve(body) })
}

function candidatesFor(uuid) {
  return [
    { title: 'Hollow Knight', provider_game_id: '4242', steam_appid: '367520', release_year: 2017, confidence: 0.98, token: TOKEN_ONE, preview: `./api/covers/preview/${TOKEN_ONE}?uuid=${uuid}`, expires_at: 1 },
    { title: 'Hollow Knight: Silksong', provider_game_id: '5151', confidence: 0.61, token: TOKEN_TWO, preview: `./api/covers/preview/${TOKEN_TWO}?uuid=${uuid}`, expires_at: 1 },
  ]
}

function postersFor(uuid) {
  return POSTER_TOKENS.map(token => ({ token, preview: `./api/covers/preview/${token}?uuid=${uuid}`, expires_at: 1 }))
}

function mountAppsView({ apps = [], search, choices, select } = {}) {
  global.fetch = vi.fn((url, options = {}) => {
    const target = String(url)
    if (target.startsWith('./api/covers/search')) {
      const params = new URL(target, 'https://host.test/').searchParams
      return search ? search(params) : reply(200, { status: true, query: params.get('name'), candidates: candidatesFor(params.get('uuid')) })
    }
    if (target.startsWith('./api/covers/choices')) {
      const body = JSON.parse(options.body)
      return choices ? choices(body) : reply(200, { status: true, choices: postersFor(body.uuid) })
    }
    if (target.startsWith('./api/covers/select')) {
      const body = JSON.parse(options.body)
      return select ? select(body) : reply(200, { status: true, path: `/covers/${body.uuid}.png` })
    }
    if (target.includes('./api/apps') && options.method === 'POST') {
      return reply(200, { status: true })
    }
    if (target.includes('./api/apps')) {
      return reply(200, { apps, current_app: '', host_name: 'Test Host', host_uuid: 'host-1' })
    }
    if (target.includes('./api/config')) {
      return reply(200, { platform: 'linux' })
    }
    return reply(200, { status: true })
  })

  return shallowMount(AppsView, {
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t.bind(i18n) },
      stubs: {
        Button: { props: ['disabled'], emits: ['click'], template: '<button :disabled="disabled" @click="$emit(\'click\')"><slot /></button>' },
        InfoHint: { template: '<span><slot /></span>' },
        Checkbox: { template: '<label />' },
      },
    },
  })
}

function coverCalls(route) {
  return global.fetch.mock.calls.filter(([url]) => String(url).startsWith(`./api/covers/${route}`))
}

function searchParams(call) {
  return new URL(String(call[0]), 'https://host.test/').searchParams
}

async function openFinder(wrapper) {
  await wrapper.find('button[aria-controls="coverFinder"]').trigger('click')
  await flushPromises()
}

async function openGame(wrapper, index) {
  await wrapper.findAll('[data-cover-candidate]')[index].trigger('click')
  await flushPromises()
}

describe('AppsView Find Cover', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    delete global.fetch
    document.body.innerHTML = ''
  })

  it('searches the host for the entry name and shows every match with its title and year', async () => {
    const wrapper = mountAppsView({ apps: [hollowKnight] })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()

    await openFinder(wrapper)

    expect(wrapper.find('#coverQuery').element.value).toBe('Hollow Knight')
    const [search] = coverCalls('search')
    expect(searchParams(search).get('name')).toBe('Hollow Knight')
    expect(searchParams(search).get('uuid')).toBe(HOLLOW_UUID)

    const tiles = wrapper.findAll('[data-cover-candidate]')
    expect(tiles).toHaveLength(2)
    expect(tiles[0].text()).toContain('Hollow Knight')
    expect(tiles[0].text()).toContain('2017')
    expect(tiles[1].text()).toContain('Hollow Knight: Silksong')
    expect(tiles[0].find('img').attributes('src')).toBe(`./api/covers/preview/${TOKEN_ONE}?uuid=${HOLLOW_UUID}`)
    // The console's CSP blocks outside hosts, so nothing may be fetched from one.
    for (const [url] of global.fetch.mock.calls) expect(String(url).startsWith('./api/')).toBe(true)
    wrapper.unmount()
  })

  it('runs the search again with the name the player typed', async () => {
    const wrapper = mountAppsView({ apps: [hollowKnight] })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()
    await openFinder(wrapper)

    await wrapper.find('#coverQuery').setValue('Silksong')
    await wrapper.find('[data-cover-search]').trigger('submit')
    await flushPromises()

    const searches = coverCalls('search')
    expect(searches).toHaveLength(2)
    expect(searchParams(searches[1]).get('name')).toBe('Silksong')
    wrapper.unmount()
  })

  it('lists a picked game\'s posters, as Nova does, and goes back to every match', async () => {
    const wrapper = mountAppsView({ apps: [hollowKnight] })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()
    await openFinder(wrapper)

    await openGame(wrapper, 0)

    const [list] = coverCalls('choices')
    expect(list[1].method).toBe('POST')
    expect(JSON.parse(list[1].body)).toEqual({ uuid: HOLLOW_UUID, provider_game_id: '4242', title: 'Hollow Knight', steam_appid: '367520' })
    expect(wrapper.find('[data-cover-game]').text()).toContain('Hollow Knight (2017)')
    const posters = wrapper.findAll('[data-cover-poster]')
    expect(posters).toHaveLength(3)
    expect(posters[2].find('img').attributes('src')).toBe(`./api/covers/preview/${POSTER_TOKENS[2]}?uuid=${HOLLOW_UUID}`)
    expect(posters[2].find('img').attributes('alt')).toBe('Poster 3 for Hollow Knight (2017)')
    expect(wrapper.findAll('[data-cover-candidate]')).toHaveLength(0)
    expect(coverCalls('select')).toHaveLength(0)

    await wrapper.find('[data-cover-back]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-cover-game]').exists()).toBe(false)
    expect(wrapper.findAll('[data-cover-candidate]')).toHaveLength(2)

    // A new search also starts from the matches.
    await openGame(wrapper, 1)
    expect(JSON.parse(coverCalls('choices')[1][1].body)).toEqual({ uuid: HOLLOW_UUID, provider_game_id: '5151', title: 'Hollow Knight: Silksong' })
    await wrapper.find('[data-cover-search]').trigger('submit')
    await flushPromises()
    expect(wrapper.find('[data-cover-game]').exists()).toBe(false)
    expect(wrapper.findAll('[data-cover-candidate]')).toHaveLength(2)
    wrapper.unmount()
  })

  it('offers the search poster when a game\'s posters cannot be listed', async () => {
    const wrapper = mountAppsView({
      apps: [hollowKnight],
      choices: () => reply(502, { status: false, code: 'steamgriddb_rate_limited', error: 'SteamGridDB is rate limiting this host. Try again in a minute.', choices: [] }),
    })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()
    await openFinder(wrapper)
    await openGame(wrapper, 1)

    expect(wrapper.find('[data-cover-state="error"]').text()).toContain('rate limiting')
    const posters = wrapper.findAll('[data-cover-poster]')
    expect(posters).toHaveLength(1)
    await posters[0].trigger('click')
    await flushPromises()
    expect(JSON.parse(coverCalls('select')[0][1].body)).toEqual({ uuid: HOLLOW_UUID, token: TOKEN_TWO })
    expect(wrapper.vm.editForm['image-path']).toBe(`/covers/${HOLLOW_UUID}.png`)
    wrapper.unmount()
  })

  it('puts the picked cover in the form and leaves saving to the player', async () => {
    const wrapper = mountAppsView({ apps: [hollowKnight] })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()
    await openFinder(wrapper)

    await openGame(wrapper, 1)
    await wrapper.findAll('[data-cover-poster]')[2].trigger('click')
    await flushPromises()

    const [select] = coverCalls('select')
    expect(select[1].method).toBe('POST')
    expect(JSON.parse(select[1].body)).toEqual({ uuid: HOLLOW_UUID, token: POSTER_TOKENS[2] })
    expect(wrapper.vm.editForm['image-path']).toBe(`/covers/${HOLLOW_UUID}.png`)
    expect(wrapper.find('#coverFinder').exists()).toBe(false)
    const saved = global.fetch.mock.calls.filter(([url, options]) => String(url).includes('./api/apps') && options?.method === 'POST')
    expect(saved).toHaveLength(0)
    wrapper.unmount()
  })

  it('gives a draft one temporary uuid for its search and its pick', async () => {
    const wrapper = mountAppsView()
    await flushPromises()
    wrapper.vm.newApp()
    await nextTick()
    wrapper.vm.editForm.name = 'Celeste'
    await nextTick()
    await openFinder(wrapper)

    const uuid = searchParams(coverCalls('search')[0]).get('uuid')
    expect(uuid).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i)

    await openGame(wrapper, 0)
    expect(JSON.parse(coverCalls('choices')[0][1].body).uuid).toBe(uuid)
    await wrapper.findAll('[data-cover-poster]')[0].trigger('click')
    await flushPromises()
    expect(JSON.parse(coverCalls('select')[0][1].body).uuid).toBe(uuid)
    expect(wrapper.vm.editForm['image-path']).toBe(`/covers/${uuid}.png`)

    // A new draft starts over with its own uuid.
    wrapper.vm.newApp()
    await nextTick()
    wrapper.vm.editForm.name = 'Celeste'
    await nextTick()
    await openFinder(wrapper)
    expect(searchParams(coverCalls('search')[1]).get('uuid')).not.toBe(uuid)
    wrapper.unmount()
  })

  it('says when SteamGridDB has no key and links to the setting that takes one', async () => {
    const wrapper = mountAppsView({
      apps: [hollowKnight],
      search: () => reply(503, {
        status: false,
        code: 'steamgriddb_key_missing',
        error: 'SteamGridDB is not configured on the host. Add a SteamGridDB API key in Polaris settings.',
        candidates: [],
      }),
    })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()
    await openFinder(wrapper)

    const error = wrapper.find('[data-cover-state="error"]')
    expect(error.text()).toContain('Add a SteamGridDB API key in Polaris settings.')
    expect(wrapper.find('[data-cover-key-link]').attributes('href')).toBe('#/config#steamgriddb_api_key')
    expect(wrapper.find('[data-cover-state="empty"]').exists()).toBe(false)
    expect(wrapper.findAll('[data-cover-candidate]')).toHaveLength(0)
    wrapper.unmount()
  })

  it('names the search that found nothing', async () => {
    const wrapper = mountAppsView({
      apps: [hollowKnight],
      search: (params) => reply(200, { status: true, query: params.get('name'), candidates: [] }),
    })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()
    await openFinder(wrapper)

    expect(wrapper.find('[data-cover-state="empty"]').text()).toContain('No covers found for "Hollow Knight"')
    expect(wrapper.find('[data-cover-state="error"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('shows upstream trouble without a key link and keeps the posters when a pick expires', async () => {
    const wrapper = mountAppsView({
      apps: [hollowKnight],
      select: () => reply(410, {
        status: false,
        code: 'cover_preview_expired',
        error: 'That cover is no longer available. Search again and pick it once more.',
      }),
    })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()
    await openFinder(wrapper)

    await openGame(wrapper, 0)
    await wrapper.findAll('[data-cover-poster]')[0].trigger('click')
    await flushPromises()

    expect(wrapper.find('[data-cover-state="error"]').text()).toContain('Search again and pick it once more.')
    expect(wrapper.find('[data-cover-key-link]').exists()).toBe(false)
    expect(wrapper.findAll('[data-cover-poster]')).toHaveLength(3)
    expect(wrapper.vm.editForm['image-path']).toBe('')
    wrapper.unmount()
  })

  it('reports a search the host could not answer', async () => {
    const wrapper = mountAppsView({
      apps: [hollowKnight],
      search: () => reply(502, { status: false, code: 'steamgriddb_rate_limited', error: 'SteamGridDB is rate limiting this host. Try again in a minute.', candidates: [] }),
    })
    await flushPromises()
    wrapper.vm.editApp(hollowKnight)
    await nextTick()
    await openFinder(wrapper)

    expect(wrapper.find('[data-cover-state="error"]').text()).toContain('rate limiting')
    expect(wrapper.find('[data-cover-key-link]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('never goes back to the browser GameDB lookup the CSP blocked', () => {
    const view = webSource('views/AppsView.vue')
    expect(view).not.toContain('raw.githubusercontent.com')
    expect(view).not.toContain('images.igdb.com')
    expect(view).not.toContain('./api/covers/upload')
    expect(view).toContain('./api/covers/search')
    expect(view).toContain('./api/covers/choices')
    expect(view).toContain('./api/covers/select')
  })

  it('links to a key setting the settings page can find', () => {
    expect(webSource('views/ConfigView.vue')).toContain('"steamgriddb_api_key": ""')
    expect(webSource('configs/tabs/General.vue')).toContain('id="steamgriddb_api_key"')
  })
})

describe('AppsView launch command pills', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    delete global.fetch
    document.body.innerHTML = ''
  })

  it('counts a launcher entry that starts through a detached command', async () => {
    const wrapper = mountAppsView({ apps: [heroicLauncher] })
    await flushPromises()
    wrapper.vm.editApp(heroicLauncher)
    await nextTick()

    expect(wrapper.text()).toContain('Command set')
    expect(wrapper.text()).not.toContain('Needs command')
    const chip = wrapper.findAll('.library-health-chip').find((node) => node.text().startsWith('Command'))
    expect(chip.find('strong').text()).toBe('Ready')

    wrapper.vm.editForm.detached = ['   ']
    await nextTick()
    expect(wrapper.text()).toContain('Needs command')
    expect(chip.find('strong').text()).toBe('Missing')
    wrapper.unmount()
  })
})
