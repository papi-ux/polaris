import { describe, expect, it, vi } from 'vitest'

import { createNavSections, flattenNavItems, getNavItemByPath } from './nav-metadata.js'
import {
  createCommandActions,
  executeCommandAction,
  filterCommandActions,
  scoreCommandAction,
} from './command-actions.js'

const labels = {
  'navbar.group_streaming': 'Streaming',
  'navbar.group_host': 'Host',
  'navbar.group_support': 'Support',
  'navbar.dashboard': 'Dashboard',
  'navbar.library': 'Library',
  'navbar.pairing': 'Pairing',
  'navbar.browser_stream': 'Browser Stream',
  'navbar.settings': 'Settings',
  'navbar.security': 'Security',
  'navbar.system': 'System',
  'navbar.troubleshoot': 'Troubleshooting',
  'command_palette.actions.audio': 'Audio settings',
  'command_palette.actions.display': 'Display settings',
  'command_palette.actions.ai': 'AI optimization',
  'command_palette.actions.network': 'Network access',
  'command_palette.actions.restart': 'Restart Polaris',
  'command_palette.actions.quit': 'Quit Polaris',
  'command_palette.restart_sent': 'Restart requested',
  'command_palette.quit_sent': 'Quit requested',
}

function t(key) {
  return labels[key] || key
}

describe('navigation metadata shell', () => {
  it('builds the sidebar from shared route metadata with Moonlight-friendly streaming language', () => {
    const sections = createNavSections(t)
    const items = flattenNavItems(sections)

    expect(sections.map((section) => section.key)).toEqual(['streaming', 'host', 'support'])
    expect(items.map((item) => item.shortcut)).toEqual([1, 2, 3, 4, 5, 6, 7, 8])
    expect(items.map((item) => item.to)).toEqual([
      '/',
      '/apps',
      '/pin',
      '/browser-stream',
      '/config',
      '/password',
      '/info',
      '/troubleshooting',
    ])

    const browserStream = getNavItemByPath(sections, '/browser-stream')
    expect(browserStream).toMatchObject({
      label: 'Browser Stream',
      sectionLabel: 'Streaming',
      commandId: 'browser-stream',
    })
    expect(browserStream.aliases).toEqual(expect.arrayContaining(['moonlight', 'stream', 'streaming', 'webrtc']))
  })
})

describe('command action registry', () => {
  it('derives navigation commands and aliases from shared nav metadata', () => {
    const router = { push: vi.fn() }
    const actions = createCommandActions({ t, router })

    expect(actions.filter((action) => action.group === 'Navigation').map((action) => action.id)).toEqual([
      'dashboard',
      'apps',
      'pairing',
      'browser-stream',
      'config',
      'password',
      'info',
      'troubleshooting',
    ])

    expect(filterCommandActions(actions, 'moonlight')[0]).toMatchObject({ id: 'browser-stream', hint: '/browser-stream' })
    expect(filterCommandActions(actions, 'game library')[0]).toMatchObject({ id: 'apps', hint: '/apps' })
    expect(scoreCommandAction(actions.find((action) => action.id === 'apps'), 'library')).toBeGreaterThan(0)
  })

  it('requires confirmation before running dangerous host palette actions', async () => {
    const router = { push: vi.fn() }
    const fetchImpl = vi.fn(() => Promise.resolve({ ok: true }))
    const toast = vi.fn()
    const confirmDangerousAction = vi.fn(() => Promise.resolve(false))
    const actions = createCommandActions({ t, router, fetchImpl, toast })

    const restart = actions.find((action) => action.id === 'restart')
    const quit = actions.find((action) => action.id === 'quit')
    expect(restart.dangerous).toBe(true)
    expect(quit.dangerous).toBe(true)

    await expect(executeCommandAction(restart, { confirmDangerousAction })).resolves.toBe(false)
    expect(confirmDangerousAction).toHaveBeenCalledWith(restart)
    expect(fetchImpl).not.toHaveBeenCalled()

    confirmDangerousAction.mockResolvedValueOnce(true)
    await expect(executeCommandAction(restart, { confirmDangerousAction })).resolves.toBe(true)
    expect(fetchImpl).toHaveBeenCalledWith('./api/restart', { method: 'POST' })
    expect(toast).toHaveBeenCalledWith('Restart requested', 'info')
  })

  it('exposes lightweight roadmap hooks for fuzzy scoring and recent commands', () => {
    const actions = createCommandActions({ t, router: { push: vi.fn() } })

    expect(scoreCommandAction(actions.find((action) => action.id === 'apps'), 'apps')).toBeGreaterThan(
      scoreCommandAction(actions.find((action) => action.id === 'apps'), 'app'),
    )
    expect(filterCommandActions(actions, '', { recentCommandIds: ['config'] })[0]).toMatchObject({ id: 'config' })
  })
})
