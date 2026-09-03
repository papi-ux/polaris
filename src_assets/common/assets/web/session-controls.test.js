import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it, vi } from 'vitest'
import { createCommandActions } from './command-actions.js'

const webSource = (relativePath) => readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web', relativePath),
  'utf8',
)

// Two session controls papi asked for on 2026-09-02: a way to sign out of the
// web console, and a way to clear the session history Mission Control shows.
describe('session controls', () => {
  it('offers Sign out in the sidebar over the existing logout endpoint', () => {
    const app = webSource('App.vue')
    expect(app).toContain('data-sidebar-sign-out')
    expect(app).toContain("fetch('./api/logout', {")
    expect(app).toContain('markWebUiUnauthenticated()')
    expect(app).toContain("i18nReady ? t('navbar.sign_out') : 'Sign out'")
    expect(webSource('public/assets/locale/en.json')).toContain('"sign_out": "Sign out"')
  })

  it('offers Sign out from the command palette and posts to the logout endpoint', async () => {
    const fetchImpl = vi.fn(async () => ({ ok: true }))
    const signOutNavigate = vi.fn()
    const actions = createCommandActions({ t: (key) => key, router: { push: vi.fn() }, fetchImpl, toast: vi.fn(), signOutNavigate })
    const signOut = actions.find((action) => action.id === 'sign-out')
    expect(signOut).toBeTruthy()
    expect(signOut.aliases).toContain('logout')
    await signOut.action()
    expect(fetchImpl).toHaveBeenCalledWith('./api/logout', expect.objectContaining({ method: 'POST' }))
    expect(signOutNavigate).toHaveBeenCalledTimes(1)

    const failing = createCommandActions({ t: (key) => key, router: { push: vi.fn() }, fetchImpl: vi.fn(async () => ({ ok: false, status: 500 })), toast: vi.fn(), signOutNavigate: vi.fn() })
    await expect(failing.find((action) => action.id === 'sign-out').action()).rejects.toThrow('HTTP 500')
  })

  it('clears both history sources from Mission Control behind a confirmation', () => {
    const dashboard = webSource('views/DashboardView.vue')
    expect(dashboard).toContain('data-session-history-host')
    expect(dashboard).toContain('data-session-history-local')
    expect((dashboard.match(/data-clear-session-history/g) || []).length).toBe(2)
    expect(dashboard).toContain('v-model="clearHistoryConfirmOpen"')
    expect(dashboard).toContain("fetch('./api/ai/history/clear', {")
    expect(dashboard).toContain('clearHistory()\n    sessionHistory.value = []')
    expect(dashboard).not.toContain('@click="clearHistory"')
    const confighttp = readFileSync(join(process.cwd(), 'src/confighttp.cpp'), 'utf8')
    expect(confighttp).toContain('server.resource["^/api/ai/history/clear$"]["POST"] = withCsrf(clearAiHistory);')
    expect(readFileSync(join(process.cwd(), 'src/ai_optimizer.cpp'), 'utf8')).toContain('void clear_history() {')
  })
})
