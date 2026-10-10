import { shallowMount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick, ref } from 'vue'
import AppsView from './views/AppsView.vue'

vi.mock('./composables/useGameScanner', () => ({
  useGameScanner: () => ({
    scanning: ref(false), importing: ref(false), steamGames: ref([]), lutrisGames: ref([]),
    heroicGames: ref([]), emulatorGames: ref([]), librarySources: ref([]),
    error: ref(null), scan: vi.fn(), importSelected: vi.fn(), toggleAll: vi.fn(),
  }),
}))
const catalog = [
  { uuid: 'a', name: 'Control', cmd: 'run-a' },
  { uuid: 'b', name: 'Portal', cmd: 'run-b' },
  { uuid: 'c', name: 'Hades', cmd: 'run-c' },
]
let view
async function render() {
  vi.stubGlobal('fetch', vi.fn(async (url) => ({
    ok: true,
    json: async () => String(url) === './api/apps' ?
      { apps: catalog.map(a => ({ ...a })), current_app: '', current_app_available: true } :
      String(url) === './api/config' ? { platform: 'linux' } : { status: true },
  })))
  const i18n = { t: key => key }
  view = shallowMount(AppsView, { global: { provide: { i18n }, mocks: { $t: i18n.t },
    stubs: { Button: { props: ['disabled'], template: '<button :disabled="disabled"><slot /></button>' } } } })
  await flushPromises()
}
const order = () => view.findAll('[data-library-app]').map(n => n.attributes('data-library-app'))
afterEach(() => { view?.unmount(); view = null; vi.unstubAllGlobals(); vi.restoreAllMocks() })

describe('Library button reorder', () => {
  it('moves a canonical neighbor locally and persists only with explicit Save order', async () => {
    await render()
    expect(view.get('[data-move-up="a"]').attributes('disabled')).toBeDefined()
    expect(view.get('[data-move-down="c"]').attributes('disabled')).toBeDefined()
    await view.get('[data-move-down="a"]').trigger('click')
    expect(order()).toEqual(['b', 'a', 'c'])
    expect(view.get('[data-reorder-status]').text()).toContain('Control moved to position 2 of 3')
    expect(view.text()).toContain('Pending order save')
    expect(fetch.mock.calls.filter(([,o]) => o?.method === 'POST')).toHaveLength(0)
    await view.vm.saveOrder()
    await flushPromises()
    const posted = fetch.mock.calls.filter(([url]) => url === './api/apps/reorder')
    expect(posted).toHaveLength(1)
    expect(JSON.parse(posted[0][1].body)).toEqual({ order: ['b', 'a', 'c'] })
  })
  it('refuses hidden-neighbor reorder while searched, busy or dragging', async () => {
    await render()
    view.vm.librarySearch = 'Control'; await nextTick()
    expect(order()).toEqual(['a'])
    expect(view.get('[data-move-down="a"]').attributes('disabled')).toBeDefined()
    await view.vm.moveLibraryApp(catalog[0], 1)
    view.vm.librarySearch = ''; await nextTick()
    expect(order()).toEqual(['a', 'b', 'c'])
    view.vm.actionDisabled = true; await nextTick()
    await view.vm.moveLibraryApp(catalog[0], 1)
    view.vm.actionDisabled = false
    view.vm.draggingApp = 0; await nextTick()
    await view.vm.moveLibraryApp(catalog[1], -1)
    view.vm.draggingApp = -1; await nextTick()
    expect(order()).toEqual(['a', 'b', 'c'])
    expect(view.vm.listReordered).toBe(false)
    expect(fetch.mock.calls.filter(([,o]) => o?.method === 'POST')).toHaveLength(0)
  })
  it('rejects boundary, unknown entry and invalid direction moves', async () => {
    await render()
    for (const [app, direction] of [[catalog[0],-1],[catalog[2],1],[{uuid:'unknown'},1],[catalog[1],0],[{},1]]) {
      await view.vm.moveLibraryApp(app, direction)
    }
    expect(order()).toEqual(['a', 'b', 'c'])
    expect(view.vm.listReordered).toBe(false)
    expect(view.get('[data-reorder-status]').text()).toBe('')
  })
})

