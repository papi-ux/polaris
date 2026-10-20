import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { flushPromises, shallowMount } from '@vue/test-utils'
import { describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'

import ConfigView from './views/ConfigView.vue'

const mockToast = vi.hoisted(() => vi.fn())

vi.mock('./composables/useToast', () => ({
  useToast: () => ({ toast: mockToast }),
}))

vi.mock('./restart-host.js', () => ({
  requestHostRestart: vi.fn(() => Promise.resolve()),
}))

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

const css = webSource('app.css')

function rule(selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  const match = css.match(new RegExp(`(?:^|\\n)\\s*${escaped}\\s*\\{([^}]*)\\}`))
  expect(match, `${selector} rule`).toBeTruthy()
  return match[1]
}

describe('look and feel shell (#366)', () => {
  it('sets decision copy in the supporting role at 13-14px, not the caption tier', () => {
    const decisionCopy = [
      '.page-subtitle',
      '.header-support-copy',
      '.settings-field-label',
      '.stat-copy',
      '.section-copy',
      '.settings-command-note',
      '.settings-tab-copy',
      '.config-page .settings-section-copy',
      '.settings-summary-copy',
    ]
    for (const selector of decisionCopy) {
      const body = rule(selector)
      expect(body, `${selector} colour`).toContain('text-support')
      expect(body, `${selector} colour`).not.toContain('text-storm')
      expect(body, `${selector} size`).toMatch(/text-sm|text-\[13px\]/)
      expect(body, `${selector} size`).not.toMatch(/text-xs|text-\[1[01]px\]/)
    }
    expect(rule('.pairing-flow-text')).toContain('color: var(--color-support)')
    expect(rule('.pairing-inline-note')).toContain('color: var(--color-support)')
    expect(rule('.pairing-inline-note')).toContain('font-size: 0.8125rem')
  })

  it('keeps eyebrows and meta pills in the subdued caption tier', () => {
    for (const selector of ['.section-kicker', '.stat-kicker', '.eyebrow-label', '.meta-pill']) {
      expect(rule(selector), selector).toContain('text-storm')
    }
  })

  it('reads settings help text in the supporting role while faded variants stay subdued', () => {
    expect(webSource('views/ConfigView.vue')).toContain('class="settings-tab-content"')
    const helpText = css.match(/\.settings-tab-content \.text-storm(:not\([^{]*\)) \{([^}]*)\}/)
    expect(helpText, 'settings help-text rule').toBeTruthy()
    expect(helpText[2]).toContain('color: var(--color-support)')
    // Unlayered, so it must not override an element's own hover or focus colours.
    for (const variant of ['hover:', 'focus:', 'group-']) expect(helpText[1]).toContain(`[class*="${variant}"]`)
    expect(rule('.settings-tab-content p.text-xs.text-storm:not([class*=":text-"])')).toContain('font-size: 0.8125rem')
  })

  it('drops the second blur on nested glass and keeps nested radii at the card radius', () => {
    expect(css).toMatch(/:is\(\.glass, \.card, \.section-card[^)]*\) :is\(\.glass, \.card, \.section-card[^)]*\)\s*\{\s*backdrop-filter: none;/)
    // Top-level glass keeps its blur; High Contrast still removes it everywhere.
    expect(rule('.card')).toContain('backdrop-filter: blur(12px)')
    expect(css).toContain('[data-theme="high-contrast"] .section-card,')
    for (const selector of ['.stat-tile', '.surface-subtle', '.action-tile']) {
      expect(rule(selector), selector).toContain('rounded-xl')
      expect(rule(selector), selector).not.toContain('rounded-2xl')
    }
    expect(rule('.section-card')).toContain('p-4 sm:p-5')
  })

  it('keeps the phone header to one row with a 44px menu target and no repeated section pill', () => {
    const app = webSource('App.vue')
    const header = app.slice(app.indexOf('data-mobile-header'), app.indexOf('</div>', app.indexOf('data-mobile-page-label')))

    expect(header).toContain('aria-controls="polaris-sidebar"')
    expect(header).toContain(':aria-expanded="sidebarOpen"')
    expect(header).toMatch(/aria-label="Toggle navigation"[^>]*class="[^"]*\bh-11 w-11\b/)
    expect(header).toContain('{{ currentPageLabel }}')
    expect(app).not.toContain('currentPageSection')
    // Route headings stay the page's own: the page name is not inside the shell <h1>.
    const shellHeading = header.slice(header.indexOf('<h1'), header.indexOf('</h1>'))
    expect(shellHeading).not.toContain('currentPageLabel')
  })

  it('keeps the phone action center static and its buttons touch sized', () => {
    expect(css).toMatch(/@media \(max-width: 47\.99rem\)[\s\S]*?\.settings-command-bar\.sticky\s*\{\s*position: static;/)
    expect(rule('.settings-action-button')).toContain('min-h-11 sm:min-h-10')
    expect(rule('.settings-command-tools')).toContain('grid-cols-[auto_minmax(0,1fr)]')
  })
})

const disconnect = 'Save and restart Polaris disconnects any active streams while Polaris restarts.'
const messages = {
  'config.action_center': 'Action Center',
  'config.unsaved_banner': 'You have pending host changes.',
  'config.saved_banner': 'No pending host changes.',
  'config.command_unsaved_restart_note': 'Save writes your edits. Save and restart Polaris also restarts it, which disconnects any active streams, so wait until you are ready.',
  'config.command_saved_restart_note': 'Search or choose a section. Save writes changes; Save and restart Polaris restarts it so they take effect.',
  'config.save_and_restart_disclosure': disconnect,
  'config.save_and_restart': 'Save and restart Polaris',
  '_common.save': 'Save',
  'config.reset_local': 'Reset Local Changes',
  'config.pending_badge': 'Pending save',
  'config.saved_badge': 'Saved',
  'config.saved_restart_needed_badge': 'Saved · restart needed',
  'config.saved_restart_needed_note': 'Saved. Polaris uses these settings after it restarts; Save and restart Polaris disconnects any active streams.',
  'config.saved_restart_needed_command_note': 'Saved. Polaris uses these settings after it restarts.',
  'config.saved_live_note': 'Saved. Polaris is already using these settings, so no restart is needed.',
  'config.restart_now': 'Restart Polaris',
}
const i18n = { t: (key) => messages[key] || key }

function mountConfigView() {
  global.fetch = vi.fn(() => Promise.resolve({
    status: 200,
    json: () => Promise.resolve({
      platform: 'linux',
      status: {},
      version: 'test',
      sunshine_name: 'Old Host',
      max_bitrate: 0,
      client_settings_live_fields: ['max_bitrate'],
      client_settings_restart_fields: ['sunshine_name'],
    }),
  }))
  return shallowMount(ConfigView, {
    attachTo: document.body,
    global: {
      provide: { i18n },
      mocks: { $t: i18n.t },
      stubs: {
        Skeleton: { template: '<div />' },
        General: { props: ['config'], template: '<div><input id="sunshine_name" v-model="config.sunshine_name"></div>' },
        AudioVideo: { props: ['config'], template: '<div><input id="max_bitrate" v-model="config.max_bitrate"></div>' },
      },
    },
  })
}

async function flushConfigLoad() {
  await Promise.resolve()
  await Promise.resolve()
  await nextTick()
}

function disconnectMentions(bar) {
  return bar.findAll('*').filter((node) => node.element.children.length === 0 && /disconnects any active streams/.test(node.text()))
}

function expectOneDisclosedConsequence(bar) {
  const mentions = disconnectMentions(bar)
  expect(mentions).toHaveLength(1)
  expect(mentions[0].text()).toBe(disconnect)
  expect(bar.find('[data-save-and-restart]').attributes('aria-describedby')).toBe(mentions[0].attributes('id'))
}

const saveAnswer = (body) => ({ status: 200, json: async () => ({ status: true, configuration_revision: 'e'.repeat(64), ...body }) })

describe('settings action center restart consequence', () => {
  it('states the disconnect once, on the line Save and restart Polaris references', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()

    wrapper.vm.config.sunshine_name = 'Nova Host'
    await nextTick()

    const bar = wrapper.find('.settings-command-bar')
    expect(bar.text()).toContain('You have pending host changes.')
    expectOneDisclosedConsequence(bar)
    expect(bar.find('[data-save-and-restart]').text()).toBe('Save and restart Polaris')
    // Save stays its own action next to the restart.
    expect(bar.findAll('button').map((button) => button.text())).toEqual(['Reset Local Changes', 'Save', 'Save and restart Polaris'])
    expect(bar.find('[data-command-note]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('says a restart is still needed without repeating the disconnect, while the save toast keeps it', async () => {
    mockToast.mockClear()
    const wrapper = mountConfigView()
    await flushConfigLoad()

    wrapper.vm.config.sunshine_name = 'Nova Host'
    await nextTick()
    global.fetch.mockResolvedValueOnce(saveAnswer({ restart_required: true }))
    await expect(wrapper.vm.save()).resolves.toBe(true)
    await flushPromises()

    const bar = wrapper.find('.settings-command-bar')
    expect(bar.find('[data-save-badge]').text()).toBe('Saved · restart needed')
    expect(bar.find('[data-command-note]').text()).toBe('Saved. Polaris uses these settings after it restarts.')
    expectOneDisclosedConsequence(bar)
    expect(bar.find('[data-save-and-restart]').attributes('disabled')).toBeUndefined()
    // The toast stands alone with its own Restart action, so it keeps the consequence.
    expect(mockToast).toHaveBeenCalledWith(messages['config.saved_restart_needed_note'], 'success', 8000, expect.objectContaining({ label: 'Restart Polaris' }))

    // A later save the host applies live does not make the earlier change live.
    wrapper.vm.config.max_bitrate = 42000
    await nextTick()
    global.fetch.mockResolvedValueOnce(saveAnswer({ restart_required: false }))
    await expect(wrapper.vm.save()).resolves.toBe(true)
    await flushPromises()
    expect(bar.find('[data-save-badge]').text()).toBe('Saved · restart needed')
    expect(bar.find('[data-command-note]').text()).toBe('Saved. Polaris uses these settings after it restarts.')
    expectOneDisclosedConsequence(bar)
    wrapper.unmount()
  })

  it('keeps the browse note when nothing is pending', async () => {
    const wrapper = mountConfigView()
    await flushConfigLoad()

    expect(wrapper.find('[data-command-note]').text()).toBe(messages['config.command_saved_restart_note'])
    wrapper.unmount()
  })
})
