import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'
import fixture from '../../../../tests/fixtures/app-launch-as-v1.json'
import AppsView from './views/AppsView.vue'
import { appPayloadForSave, LAUNCH_AS_ORDER, launchAsOptions, sentence } from './app-launch-as.js'

vi.mock('./composables/useGameScanner', () => ({
  useGameScanner: () => ({ scanning: ref(false), importing: ref(false), steamGames: ref([]),
    lutrisGames: ref([]), heroicGames: ref([]), emulatorGames: ref([]), librarySources: ref([]),
    importedGames: ref([]), error: ref(null), scan: vi.fn(), importSelected: vi.fn(), toggleAll: vi.fn() }),
}))

const i18n = { t(key, params = {}) {
  if (key === 'apps.launch_as_host_default_now') return `Host default, now ${params.mode}`
  if (key === 'apps.launch_as_host_default') return 'Host default'
  if (key === 'apps.launch_as_saved_unavailable') return `This app is set to ${params.mode}: ${params.reason}`
  if (key === 'apps.launch_as_unknown') return `Unknown launch mode: ${params.value}`
  if (key === 'apps.launch_as_pill') return `Launch as ${params.mode}`
  if (key === 'config.av_mode_unavailable') return `Unavailable: ${params.reason}`
  return key
} }
const baseEntry = { name: 'Game', uuid: 'game-1', cmd: 'game', 'image-path': '',
  'launch-as': 'host_default', 'launch-as-basis': 'none', 'desktop-mirror': false, 'virtual-display': false }
const catalogue = fixture.values.slice(1).map(({ id, label }) => ({ value: id, label, available: true }))

describe('Launch as shared fixture', () => {
  it('keeps exact fixture ids, labels and descriptions and excludes the host-only dongle', () => {
    expect(LAUNCH_AS_ORDER).toEqual(fixture.values.map(({ id }) => id))
    const options = launchAsOptions(catalogue)
    expect(options.map(({ id, label }) => ({ id, label }))).toEqual(fixture.values)
    expect(options.every(({ id, descKey }) => descKey === `apps.launch_as_${id}_desc`)).toBe(true)
  })
  it('preserves all non-legacy payload fields without mutating the loaded entry', () => {
    const entry = { ...baseEntry, 'launch-as': 'turbo', env: { KEY: 'value' } }
    const result = appPayloadForSave(entry)
    expect(result).toEqual({ name: 'Game', uuid: 'game-1', cmd: 'game', 'image-path': '', 'launch-as': 'turbo', env: { KEY: 'value' } })
    expect(entry['launch-as-basis']).toBe('none')
  })
  it('adds a full stop once and leaves empty reasons empty', () => {
    expect(sentence('No backend')).toBe('No backend.')
    expect(sentence('No backend.')).toBe('No backend.')
    expect(sentence('')).toBe('')
  })
})

async function editor({ entry = baseEntry, platform = 'linux', options = catalogue } = {}) {
  global.fetch = vi.fn(async (url, request = {}) => ({ ok: true, json: async () => {
    if (String(url).includes('./api/apps')) return request.method === 'POST' ? { status: true } : { apps: [entry], current_app: '', host_name: 'Host' }
    if (String(url).includes('./api/config')) return { platform, stream_display_mode_options: options,
      host_default_stream_path_label: 'Private Stream', stream_path_label: 'Host Virtual Display' }
    return { status: true }
  } }))
  const wrapper = shallowMount(AppsView, { global: { provide: { i18n }, mocks: { $t: i18n.t }, stubs: {
    Button: { props: ['disabled'], emits: ['click'], template: '<button :disabled="disabled" @click="$emit(\'click\')"><slot /></button>' },
    InfoHint: { template: '<span><slot /></span>' }, Checkbox: { template: '<label />' },
  } } })
  await flushPromises()
  wrapper.vm.editApp(entry)
  await nextTick()
  return wrapper
}
function savedBody() {
  const call = global.fetch.mock.calls.find(([url, request]) => String(url).includes('./api/apps') && request?.method === 'POST')
  expect(call, 'actual editor POST').toBeDefined()
  return JSON.parse(call[1].body)
}

describe('approved per-app Launch as editor', () => {
  afterEach(() => { vi.restoreAllMocks(); delete global.fetch; document.body.innerHTML = '' })

  it('renders exactly the seven fixture choices in order and names the saved host default', async () => {
    const wrapper = await editor()
    expect(wrapper.findAll('input[type="radio"][name="launch-as"]').map((input) => input.element.value)).toEqual(fixture.values.map(({ id }) => id))
    expect(wrapper.text()).toContain('Host default, now Private Stream')
    expect(wrapper.find('#desktopMirror').exists()).toBe(false)
    expect(wrapper.find('#virtualDisplay').exists()).toBe(false)
    wrapper.unmount()
  })

  it('keeps a saved unavailable choice checked, disabled and described by the host reason', async () => {
    const wrapper = await editor({ entry: { ...baseEntry, 'launch-as': 'host_virtual_display' },
      options: catalogue.map((option) => option.value === 'host_virtual_display' ? { ...option, available: false, unavailable_reason: 'No virtual display backend' } : option) })
    const choice = wrapper.find('input[value="host_virtual_display"]')
    expect(choice.exists()).toBe(true)
    expect(choice.element.checked).toBe(true)
    expect(choice.element.disabled).toBe(true)
    expect(wrapper.find(`#${choice.attributes('aria-describedby')}`).text()).toContain('No virtual display backend.')
    expect(wrapper.vm.editForm['launch-as']).toBe('host_virtual_display')
    wrapper.unmount()
  })

  it('disables an unadvertised mode with an accessible reason while Host default stays enabled', async () => {
    const wrapper = await editor({ options: [] })
    const choice = wrapper.find('input[value="gamescope_stream"]')
    expect(choice.exists()).toBe(true)
    expect(choice.element.disabled).toBe(true)
    expect(wrapper.find(`#${choice.attributes('aria-describedby')}`).text()).toContain('This mode was not advertised by this host.')
    expect(wrapper.find('input[value="host_default"]').element.disabled).toBe(false)
    wrapper.unmount()
  })

  it.each(['turbo', 'Host_Default', ' host_default'])('keeps unknown saved %s visible and unselected without rewriting it', async (value) => {
    const wrapper = await editor({ entry: { ...baseEntry, 'launch-as': value } })
    expect(wrapper.text()).toContain(`Unknown launch mode: ${value}`)
    expect(wrapper.findAll('input[type="radio"][name="launch-as"]').every((input) => !input.element.checked)).toBe(true)
    expect(wrapper.vm.editForm['launch-as']).toBe(value)
    wrapper.unmount()
  })

  it.each(fixture.values)('posts only the canonical field for $id at the actual save boundary', async ({ id }) => {
    const wrapper = await editor({ entry: { ...baseEntry, 'launch-as': id, 'desktop-mirror': true, 'virtual-display': true, 'launch-as-basis': 'both' } })
    wrapper.vm.save()
    await flushPromises()
    const body = savedBody()
    expect(body['launch-as']).toBe(id)
    expect(Object.hasOwn(body, 'desktop-mirror')).toBe(false)
    expect(Object.hasOwn(body, 'virtual-display')).toBe(false)
    expect(Object.hasOwn(body, 'launch-as-basis')).toBe(false)
    wrapper.unmount()
  })

  it('lets the bundled Mirror Desktop entry become Host default at the actual save boundary', async () => {
    const wrapper = await editor({ entry: { ...baseEntry, name: 'Desktop', 'image-path': 'desktop.png', 'launch-as': 'desktop_display', 'desktop-mirror': true } })
    expect(wrapper.find('input[value="desktop_display"]').element.checked).toBe(true)
    await wrapper.find('input[value="host_default"]').setValue()
    wrapper.vm.save(); await flushPromises()
    expect(savedBody()['launch-as']).toBe('host_default')
    expect(Object.hasOwn(savedBody(), 'desktop-mirror')).toBe(false)
    wrapper.unmount()
  })

  it('makes a new entry Host default explicitly', async () => {
    const wrapper = await editor()
    wrapper.vm.newApp(); await nextTick()
    expect(wrapper.vm.editForm['launch-as']).toBe('host_default')
    wrapper.vm.save(); await flushPromises()
    expect(savedBody()['launch-as']).toBe('host_default')
    wrapper.unmount()
  })

  it('hides the Linux selector on other platforms and preserves the stored canonical value', async () => {
    const wrapper = await editor({ platform: 'windows', entry: { ...baseEntry, 'launch-as': 'host_virtual_display' } })
    expect(wrapper.findAll('input[type="radio"][name="launch-as"]').length).toBe(0)
    wrapper.vm.save(); await flushPromises()
    expect(savedBody()['launch-as']).toBe('host_virtual_display')
    wrapper.unmount()
  })
})
