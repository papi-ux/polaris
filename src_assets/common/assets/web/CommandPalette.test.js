import { mount, flushPromises } from '@vue/test-utils'
import { defineComponent, nextTick, ref } from 'vue'
import { createI18n } from 'vue-i18n'
import { createMemoryHistory, createRouter } from 'vue-router'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import CommandPalette from './CommandPalette.vue'
import { useToast } from './composables/useToast.js'

const messages = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
let wrapper, resolveRestart
const reply = body => ({ ok: true, json: async () => body })
const gameRequests = () => fetch.mock.calls.filter(([url]) => url === './api/apps')
const mutations = () => fetch.mock.calls.filter(([, options]) => options?.method === 'POST')
const search = () => document.querySelector('[aria-label="Search commands"]')

beforeEach(() => {
  localStorage.clear()
  vi.stubGlobal('fetch', vi.fn(async url => {
    if (url === './api/apps') return reply({ apps: [{ uuid: 'game-a', name: 'Recent game' }, { uuid: 'desktop', name: 'Desktop' }] })
    if (url === './api/restart') return new Promise(resolve => { resolveRestart = resolve })
    if (url === './api/update-status') return reply({ platform: 'linux', version: '1.5.0' })
    return reply({ tag_name: 'v1.5.0' })
  }))
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = null
  const { toasts, dismiss } = useToast()
  toasts.value.forEach(toast => dismiss(toast.id, { immediate: true }))
  vi.unstubAllGlobals()
  localStorage.clear()
})

async function start(initiallyOpen = false) {
  const router = createRouter({ history: createMemoryHistory(), routes: [{ path: '/:pathMatch(.*)*', component: { template: '<div />' } }] })
  await router.push('/')
  await router.isReady()
  const i18n = createI18n({ legacy: false, locale: 'en', messages: { en: messages } })
  const host = defineComponent({
    components: { CommandPalette },
    setup: () => ({ open: ref(initiallyOpen) }),
    template: '<CommandPalette v-model="open" />',
  })
  wrapper = mount(host, { attachTo: document.body, global: {
    plugins: [router, i18n], provide: { i18n: i18n.global },
  } })
  await flushPromises()
}
async function open() {
  wrapper.vm.open = true
  await vi.waitFor(() => expect(search()).not.toBeNull())
  await flushPromises()
}
async function requestRestart() {
  await open()
  search().value = 'restart'
  search().dispatchEvent(new Event('input', { bubbles: true }))
  await nextTick()
  const openingKey = new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })
  search().dispatchEvent(openingKey)
  await flushPromises()
  return openingKey
}

describe('command palette first-open lifecycle', () => {
  it('keeps a closed palette idle until opened', async () => {
    await start()
    expect(search()).toBeNull()
    expect(gameRequests()).toHaveLength(0)
    await open()
    expect(gameRequests()).toHaveLength(1)
  })

  it('focuses search and loads recent games on the first open and on reopen', async () => {
    await start(true)
    expect(document.activeElement).toBe(search())
    expect(gameRequests()).toHaveLength(1)
    expect(document.body.textContent).toContain('Launch Recent game')
    expect(document.body.textContent).not.toContain('Launch Desktop')
    const palette = wrapper.findComponent(CommandPalette).vm.$.uid
    search().dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    await flushPromises()
    expect(search()).toBeNull()
    expect(wrapper.findComponent(CommandPalette).vm.$.uid).toBe(palette)
    await open()
    expect(document.activeElement).toBe(search())
    expect(gameRequests()).toHaveLength(2)
  })

  it.each(['cancel', 'confirm'])('keeps dangerous confirmation alive after palette close (%s)', async choice => {
    await start()
    await requestRestart()
    expect(search()).toBeNull()
    expect(wrapper.findComponent(CommandPalette).exists()).toBe(true)
    const cancel = document.querySelector('[data-confirm-cancel]')
    expect(cancel).not.toBeNull()
    expect(document.activeElement).toBe(cancel)
    expect(mutations()).toHaveLength(0)
    if (choice === 'cancel') {
      cancel.click()
      await flushPromises()
      expect(mutations()).toHaveLength(0)
    } else {
      document.querySelector('[data-confirm-confirm]').click()
      await flushPromises()
      expect(mutations()).toHaveLength(1)
      expect(mutations()[0]).toEqual(['./api/restart', { method: 'POST' }])
      expect(document.querySelector('[data-confirm-confirm]').disabled).toBe(true)
      resolveRestart(reply({}))
      await flushPromises()
    }
    expect(document.querySelector('[data-confirm-confirm]')).toBeNull()
    expect(wrapper.findComponent(CommandPalette).exists()).toBe(true)
  })

  it('consumes the opening Enter before it can activate the newly focused Cancel button', async () => {
    await start()
    const openingKey = await requestRestart()
    expect(openingKey.defaultPrevented).toBe(true)
    const cancel = document.querySelector('[data-confirm-cancel]')
    expect(cancel).not.toBeNull()
    expect(document.activeElement).toBe(cancel)
    expect(mutations()).toHaveLength(0)
  })
})
