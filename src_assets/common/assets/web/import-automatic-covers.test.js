import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import AppsView from './views/AppsView.vue'

const i18n = { t: (key) => key }
let wrapper
const calls = () => global.fetch.mock.calls
const coverStarts = () => calls().filter(([url, options]) => url.endsWith('/covers/sweep') && options?.method === 'POST')
async function mountImport({ receipts = [{ uuid: 'new-game', name: 'Imported' }] } = {}) {
  vi.stubGlobal('fetch', vi.fn(async (url, options = {}) => {
    let body = { status: true }
    if (url.endsWith('/apps')) body = { apps: [], host_name: 'Test host' }
    if (url.endsWith('/config')) body = { platform: 'linux' }
    if (url.endsWith('/games/scan')) body = { lutris_games: [{ name: 'Imported', source: 'lutris', slug: 'imported' }] }
    if (url.endsWith('/games/import')) body = { status: true, imported: 1, imported_games: receipts }
    if (url.endsWith('/covers/sweep')) body = { status: true, nothing_to_do: true, sweep: { id: '', state: 'ready', total: 0, proposals: [] } }
    return { ok: true, json: async () => body }
  }))
  wrapper = shallowMount(AppsView, { global: {
    provide: { i18n }, mocks: { $t: i18n.t }, stubs: {
      Button: { props: ['disabled'], emits: ['click'], template: '<button :disabled="disabled" @click="$emit(\'click\')"><slot /></button>' },
      InfoHint: { template: '<span><slot /></span>' }, Checkbox: { template: '<label />' },
    },
  } })
  await flushPromises()
  wrapper.vm.showImport = true
  await wrapper.vm.scanGames()
  await flushPromises()
  return wrapper.get('[data-import-auto-covers] input')
}

describe('Import automatic covers option', () => {
  afterEach(() => { wrapper?.unmount(); vi.unstubAllGlobals(); vi.restoreAllMocks() })
  it('starts unchecked and a normal import does not look up covers', async () => {
    const checkbox = await mountImport()
    expect(checkbox.element.checked).toBe(false)
    await wrapper.vm.doImport()
    expect(coverStarts()).toEqual([])
  })
  it('uses the import response UUIDs when the player opts in', async () => {
    const checkbox = await mountImport()
    await checkbox.setValue(true)
    await wrapper.vm.doImport()
    expect(coverStarts()).toHaveLength(1)
    expect(JSON.parse(coverStarts()[0][1].body)).toEqual({ uuids: ['new-game'] })
    expect(wrapper.vm.showSweepReview).toBe(true)
  })
  it('does not turn a missing import receipt into a whole-library search', async () => {
    const checkbox = await mountImport({ receipts: [] })
    await checkbox.setValue(true)
    await wrapper.vm.doImport()
    expect(coverStarts()).toEqual([])
  })
})
