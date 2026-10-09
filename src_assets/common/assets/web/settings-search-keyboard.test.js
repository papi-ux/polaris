import { shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'
import { readFileSync } from 'node:fs'

import ConfigView from './views/ConfigView.vue'

vi.mock('./composables/useToast', () => ({ useToast: () => ({ toast: vi.fn() }) }))
vi.mock('./restart-host.js', () => ({ requestHostRestart: vi.fn(() => Promise.resolve()) }))

// The real English strings, so a missing locale key fails here instead of rendering a raw key.
const en = JSON.parse(readFileSync('src_assets/common/assets/web/public/assets/locale/en.json', 'utf8'))
const i18n = {
  t(key, params = {}) {
    const value = key.split('.').reduce((node, part) => node?.[part], en)
    if (typeof value !== 'string') return key
    return value.replace(/\{(\w+)\}/g, (_, name) => String(params[name] ?? ''))
  },
}

let wrapper
const NO_MATCH_QUERY = 'qxzvjkqp'

function mountConfigView(stubOverrides = {}) {
  global.fetch = vi.fn(() => Promise.resolve({
    status: 200,
    json: () => Promise.resolve({ platform: 'linux', status: {}, version: 'test', sunshine_name: 'Host', max_bitrate: 0 }),
  }))
  wrapper = shallowMount(ConfigView, {
    attachTo: document.body,
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t.bind(i18n) },
      stubs: {
        Skeleton: { template: '<div />' },
        General: { props: ['config'], template: '<div><label for="sunshine_name">Name</label><input id="sunshine_name" data-setting-key="sunshine_name" v-model="config.sunshine_name"></div>' },
        AudioVideo: { props: ['config'], template: '<div><input id="max_bitrate" data-setting-key="max_bitrate" v-model="config.max_bitrate"></div>' },
        Inputs: { template: '<div />' },
        Network: { template: '<div />' },
        Files: { template: '<div />' },
        Advanced: { template: '<div />' },
        AiOptimizer: { template: '<div />' },
        ContainerEncoders: { template: '<div />' },
        ...stubOverrides,
      },
    },
  })
  return wrapper
}

async function settle() {
  for (let i = 0; i < 4; i += 1) {
    await Promise.resolve()
    await nextTick()
  }
}

describe('settings search keyboard and accessibility', () => {
  afterEach(() => {
    wrapper?.unmount()
    wrapper = null
  })

  it('names the search field for assistive technology and offers a search keyboard', async () => {
    mountConfigView()
    await settle()
    const input = wrapper.get('[data-settings-search]')
    expect(input.attributes('type')).toBe('search')
    expect(input.attributes('aria-label')).toBe(en.config.search_label)
    expect(input.attributes('aria-label')).not.toBe('config.search_label')
    expect(input.attributes('enterkeyhint')).toBe('search')
  })

  it('moves keyboard focus from the search field to the matched setting on Enter', async () => {
    mountConfigView()
    await settle()
    const input = wrapper.get('[data-settings-search]')
    input.element.focus()
    await input.setValue(en.config.sunshine_name)
    await settle()
    expect(document.activeElement).toBe(input.element)

    await input.trigger('keydown', { key: 'Enter' })
    await settle()
    expect(wrapper.vm.currentTab).toBe('general')
    expect(document.activeElement?.id).toBe('sunshine_name')
  })

  it('ignores the Enter that confirms an input-method composition', async () => {
    mountConfigView()
    await settle()
    const input = wrapper.get('[data-settings-search]')
    input.element.focus()
    await input.setValue(en.config.sunshine_name)
    await settle()
    const event = new KeyboardEvent('keydown', { key: 'Enter', isComposing: true, bubbles: true, cancelable: true })
    input.element.dispatchEvent(event)
    await settle()
    expect(event.defaultPrevented).toBe(false)
    expect(document.activeElement).toBe(input.element)
  })

  it('clears a search with Escape and keeps focus in the field', async () => {
    mountConfigView()
    await settle()
    const input = wrapper.get('[data-settings-search]')
    input.element.focus()
    await input.setValue('bitrate')
    await settle()
    expect(wrapper.vm.searchQuery).toBe('bitrate')

    await input.trigger('keydown', { key: 'Escape' })
    await settle()
    expect(wrapper.vm.searchQuery).toBe('')
    expect(input.element.value).toBe('')
    expect(document.activeElement).toBe(input.element)
  })

  it('leaves Escape alone when there is nothing to clear', async () => {
    mountConfigView()
    await settle()
    const input = wrapper.get('[data-settings-search]')
    const event = new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })
    input.element.dispatchEvent(event)
    await settle()
    expect(event.defaultPrevented).toBe(false)
  })

  it('does nothing on Enter when the search matches no setting', async () => {
    mountConfigView()
    await settle()
    const input = wrapper.get('[data-settings-search]')
    input.element.focus()
    // One token that appears in no tab, option, label or alias, so this really is a no-match.
    await input.setValue(NO_MATCH_QUERY)
    await settle()
    expect(wrapper.text()).toContain(en.config.search_empty_heading)
    expect(wrapper.find('.settings-tab-content').exists()).toBe(false)
    const tabBefore = wrapper.vm.currentTab
    await input.trigger('keydown', { key: 'Enter' })
    await settle()
    expect(wrapper.vm.currentTab).toBe(tabBefore)
    expect(document.activeElement).toBe(input.element)
  })

  it('keeps the current section when Enter is pressed in an empty search', async () => {
    mountConfigView()
    await settle()
    wrapper.vm.currentTab = 'advanced'
    await settle()
    const input = wrapper.get('[data-settings-search]')
    input.element.focus()
    await input.trigger('keydown', { key: 'Enter' })
    await settle()
    expect(wrapper.vm.currentTab).toBe('advanced')
    expect(document.activeElement).toBe(input.element)
  })

  it('opens the closed disclosures around a matched setting, outermost first, and only those', async () => {
    // The real Advanced tab keeps legacy_ordering inside a closed disclosure; this mirrors that shape.
    const opened = []
    mountConfigView({
      Advanced: {
        props: ['config'],
        template: `<div>
          <details data-outer><summary>Compatibility</summary>
            <details data-inner><summary>Ordering</summary>
              <label for="legacy_ordering"><input type="checkbox" id="legacy_ordering"></label>
            </details>
          </details>
          <details data-unrelated><summary>Capture</summary><select id="capture"></select></details>
        </div>`,
      },
    })
    await settle()
    // The fixture lives on the Advanced tab, as legacy_ordering does; open that tab before reaching into it.
    wrapper.vm.currentTab = 'advanced'
    await settle()
    const outer = wrapper.get('[data-outer]').element
    const inner = wrapper.get('[data-inner]').element
    for (const [name, node] of [['outer', outer], ['inner', inner]]) {
      const descriptor = Object.getOwnPropertyDescriptor(HTMLDetailsElement.prototype, 'open')
      Object.defineProperty(node, 'open', {
        configurable: true,
        get() { return descriptor.get.call(this) },
        set(value) { if (value) opened.push(name); descriptor.set.call(this, value) },
      })
    }
    expect(outer.open).toBe(false)
    expect(inner.open).toBe(false)

    const input = wrapper.get('[data-settings-search]')
    input.element.focus()
    await input.setValue(en.config.legacy_ordering)
    await settle()
    await input.trigger('keydown', { key: 'Enter' })
    await settle()

    expect(wrapper.vm.currentTab).toBe('advanced')
    expect(outer.open).toBe(true)
    expect(inner.open).toBe(true)
    expect(opened).toEqual(['outer', 'inner'])
    expect(wrapper.get('[data-unrelated]').element.open).toBe(false)
    expect(document.activeElement?.id).toBe('legacy_ordering')
  })
})
