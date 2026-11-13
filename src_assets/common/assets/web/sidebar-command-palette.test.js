import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import App from './App.vue'
import { initializeWebUiAuthState, markWebUiAuthenticated } from './auth-state.js'

// #672: on Linux the sidebar's Command Palette chip reads Ctrl+K, which is wider than ⌘K. The row
// used to wrap, so the chip dropped onto a line of its own, the row grew from 42 to 70 px and the
// version footer fell off a 1289x864 window. The row must be one line whatever the chip reads.

vi.mock('vue-router', () => ({ useRoute: () => ({ path: '/' }) }))

const platforms = [
  { platform: 'Linux x86_64', chip: 'Ctrl+K', shortcut: 'Ctrl+K to search' },
  { platform: 'Win32', chip: 'Ctrl+K', shortcut: 'Ctrl+K to search' },
  { platform: 'MacIntel', chip: '⌘K', shortcut: '⌘K to search' },
]

async function mountShell(platform, { collapsed = false } = {}) {
  Object.defineProperty(window.navigator, 'platform', { configurable: true, get: () => platform })
  localStorage.setItem('sidebarCollapsed', String(collapsed))
  markWebUiAuthenticated({ status: true, platform: 'linux', version: '1.5.0' })
  const wrapper = mount(App, {
    attachTo: document.body,
    global: {
      mocks: { $route: { path: '/' } },
      stubs: {
        'router-link': { template: '<a><slot /></a>' },
        'router-view': true,
        SpaceParticles: true,
        ThemeToggle: true,
        Toast: true,
        SettingsUnreadableBanner: true,
      },
    },
  })
  await flushPromises()
  return wrapper
}

const paletteRow = (wrapper) => wrapper.get('button[aria-label^="Open command palette"]')

function layout(wrapper) {
  const row = paletteRow(wrapper).element
  return [row, ...row.children].map((node) => node.getAttribute('class'))
}

describe('sidebar Command Palette row (#672)', () => {
  beforeEach(() => {
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, json: async () => null })))
  })

  afterEach(() => {
    document.body.innerHTML = ''
    delete window.navigator.platform
    localStorage.clear()
    vi.unstubAllGlobals()
    initializeWebUiAuthState()
  })

  it.each(platforms)('keeps the $chip chip on the label\'s line on $platform', async ({ platform, chip, shortcut }) => {
    const wrapper = await mountShell(platform)
    const row = paletteRow(wrapper)

    // One flex line: the row itself never wraps, the chip never shrinks, and the label takes the
    // room that is left (it may wrap within itself on a narrow sidebar, but never pushes the chip
    // onto a line of its own).
    expect(row.classes()).toEqual(expect.arrayContaining(['flex', 'items-center']))
    expect(row.classes()).not.toContain('flex-wrap')
    const [icon, label, kbd, ...rest] = row.element.children
    expect(rest).toEqual([])
    expect(icon.tagName.toLowerCase()).toBe('svg')
    expect(icon.classList.contains('shrink-0')).toBe(true)
    expect(label.textContent.trim()).toBe('Command Palette')
    expect([...label.classList]).toEqual(expect.arrayContaining(['min-w-0', 'flex-1']))
    expect(kbd.tagName).toBe('KBD')
    expect(kbd.textContent.trim()).toBe(chip)
    expect(kbd.classList.contains('shrink-0')).toBe(true)
    // Measured in Chromium on Linux at the default 16rem sidebar: the label is 124 px wide. Beside
    // the old monospace px-2 Ctrl+K chip it got 117 px; beside this UI-font px-1.5 chip it gets 128.
    expect([...kbd.classList]).toEqual(expect.arrayContaining(['font-sans', 'px-1.5']))

    // Focus, name and hint are unchanged.
    expect(row.attributes('type')).toBe('button')
    expect(row.classes()).toContain('focus-ring')
    expect(row.attributes('aria-label')).toBe(`Open command palette (${shortcut})`)
    expect(row.attributes('title')).toBe(shortcut)

    wrapper.unmount()
  })

  it('lays the row out identically whether the chip reads Ctrl+K or ⌘K', async () => {
    const layouts = []
    for (const { platform } of platforms) {
      const wrapper = await mountShell(platform)
      layouts.push(layout(wrapper))
      wrapper.unmount()
    }
    expect(layouts[1]).toEqual(layouts[0])
    expect(layouts[2]).toEqual(layouts[0])
  })

  it.each(platforms)('still stacks the icon over its K chip in the collapsed rail on $platform', async ({ platform, shortcut }) => {
    const wrapper = await mountShell(platform, { collapsed: true })
    const row = paletteRow(wrapper)

    expect(row.classes()).toContain('flex-wrap')
    const [icon, kbd, ...rest] = row.element.children
    expect(rest).toEqual([])
    expect(icon.tagName.toLowerCase()).toBe('svg')
    expect(kbd.tagName).toBe('KBD')
    expect(kbd.textContent.trim()).toBe('K')
    expect(row.attributes('aria-label')).toBe(`Open command palette (${shortcut})`)

    wrapper.unmount()
  })
})
