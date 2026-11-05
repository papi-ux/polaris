import { test, expect } from './fixtures/mocked.js'

// Mocked browser acceptance only. No host, process, scan or physical device is touched.
const sample = overrides => ({ version: 1, supported: true, epoch: 'epoch-a', revision: '4', sample_age_ms: 0,
  desktop: 'closed', proof: 'valid', receipt: 'complete', receipt_revision: '2', last_scan: 'complete',
  metadata_available: true, runtime: 'not_selected', prerequisites: 'unknown', state: 'launcher_closed',
  reason: 'choose_game', selected: null, ...overrides })
const themes = ['polaris', 'portable-chrome', 'oled', 'miami', 'director', 'high-contrast']
const checklist = page => page.locator('[data-epic-private-setup]')
const region = page => page.getByRole('region', { name: 'Epic Private Stream setup' })
const retry = page => region(page).getByRole('button', { name: 'Check again' })

async function host(page, theme = 'polaris') {
  const state = { mode: 'valid', facts: sample(), reads: 0, scans: 0, writes: [], pending: [] }
  await page.addInitScript(skin => {
    localStorage.setItem('polaris-theme', skin)
    window.EventSource = class { close() {} }
  }, theme)
  await page.route('**/api/**', async route => {
    const request = route.request(), path = new URL(request.url()).pathname
    if (request.method() !== 'GET') { state.writes.push(path); return route.fulfill({ status: 403, json: { status: false } }) }
    if (path === '/api/epic/setup') {
      state.reads++
      if (state.mode === 'slow') {
        return new Promise(resolve => state.pending.push(async () => {
          await route.fulfill({ json: state.facts }).catch(() => {}); resolve()
        }))
      }
      if (state.mode === 'offline') return route.abort('failed')
      if (state.mode === 'auth') return route.fulfill({ status: 401, contentType: 'text/html', body: 'Sign in' })
      return route.fulfill({ json: state.facts })
    }
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/config') return route.fulfill({ json: { status: true, platform: 'linux', version: '1.5.0-beta.2', sunshine_name: 'Fixture Host' } })
    if (path === '/api/apps') return route.fulfill({ json: { status: true, apps: [], current_app: '', host_name: 'Fixture Host', host_uuid: 'fixture' } })
    if (path === '/api/games/scan') { state.scans++; return route.fulfill({ json: { status: true, epic_games: [] } }) }
    if (path === '/api/rom-sources') return route.fulfill({ json: { status: true, presets: [], sources: [] } })
    if (path === '/api/stats/stream') return route.fulfill({ json: { streaming: false } })
    if (path === '/api/clients/list') return route.fulfill({ json: { status: true, named_certs: [] } })
    if (path === '/api/multiseat/profiles') return route.fulfill({ json: { enabled: false, available: true, profiles: [], activity: [] } })
    return route.fulfill({ json: { status: true } })
  })
  state.release = async () => { for (const release of state.pending.splice(0)) await release() }
  return state
}
async function visit(page) {
  await page.goto('/#/apps')
  await page.getByRole('button', { name: 'Import Games', exact: true }).click()
  await expect(checklist(page).locator('summary')).toBeVisible()
}
async function open(page) {
  await checklist(page).locator('summary').click()
  await expect(region(page)).toBeVisible()
}

for (const theme of themes) {
  test(`zero-title checklist wraps at 320px with enlarged text, reduced motion, forced colors and ${theme}`, async ({ page }) => {
    await page.setViewportSize({ width: 320, height: 900 })
    await page.emulateMedia({ reducedMotion: 'reduce', forcedColors: 'active' })
    const state = await host(page, theme)
    await visit(page)
    expect(state.reads).toBe(0); expect(state.scans).toBe(0)
    await page.evaluate(() => { document.documentElement.style.fontSize = '32px' })
    await open(page)
    await expect(region(page)).toContainText('choose a game to check its setup')
    expect(await page.evaluate(() => document.documentElement.dataset.theme || 'polaris')).toBe(theme)
    const shape = await checklist(page).evaluate(node => {
      const box = node.getBoundingClientRect()
      return { left: box.left, right: box.right, client: node.clientWidth, scroll: node.scrollWidth,
        targets: [...node.querySelectorAll('summary, button, select')].map(el => {
          const r = el.getBoundingClientRect(), css = getComputedStyle(el)
          return { width: r.width, height: r.height, animation: css.animationName, border: css.borderTopColor, outline: css.outlineStyle }
        }), reduced: matchMedia('(prefers-reduced-motion: reduce)').matches, forced: matchMedia('(forced-colors: active)').matches }
    })
    expect(shape.left).toBeGreaterThanOrEqual(0); expect(shape.right).toBeLessThanOrEqual(320)
    expect(shape.scroll).toBeLessThanOrEqual(shape.client + 1)
    expect(shape.reduced && shape.forced).toBe(true)
    for (const target of shape.targets) {
      expect(target.width).toBeGreaterThanOrEqual(44); expect(target.height).toBeGreaterThanOrEqual(44)
      expect(target.animation).toBe('none')
    }
    await page.keyboard.press('Tab'); await page.keyboard.press('Tab')
    await expect(retry(page)).toBeFocused()
    const focusStyle = await retry(page).evaluate(el => ({ width: getComputedStyle(el).outlineWidth, style: getComputedStyle(el).outlineStyle }))
    expect(focusStyle.style).not.toBe('none'); expect(parseFloat(focusStyle.width)).toBeGreaterThanOrEqual(2)
    expect(state.writes).toEqual([]); expect(state.scans).toBe(0)
    // The same theme also keeps native colors and no animation after media preferences change.
    await page.emulateMedia({ forcedColors: 'none', reducedMotion: 'no-preference' })
    expect(await retry(page).evaluate(el => getComputedStyle(el).animationName)).toBe('none')
  })
}

for (const mode of ['touch', 'hybrid']) {
  test.describe(`${mode} input`, () => {
    test.use({ hasTouch: true, isMobile: mode === 'touch', viewport: { width: 390, height: 900 } })
    test('supports tap, Tab/Shift-Tab, Enter/Space, Escape and focus retention during polling', async ({ page }) => {
      const state = await host(page)
      await visit(page)
      await checklist(page).locator('summary').tap()
      await expect(region(page)).toContainText('choose a game')
      expect(await page.evaluate(() => navigator.maxTouchPoints)).toBeGreaterThan(0)
      const entry = checklist(page).locator('summary')
      await entry.focus(); await page.keyboard.press('Space')
      await expect(region(page)).toHaveCount(0)
      await page.keyboard.press('Enter'); await expect(region(page)).toBeVisible()
      await page.keyboard.press('Tab'); await expect(region(page).getByRole('combobox')).toBeFocused()
      await page.keyboard.press('Shift+Tab'); await expect(entry).toBeFocused()
      await page.keyboard.press('Tab'); await page.keyboard.press('Tab'); await expect(retry(page)).toBeFocused()
      await expect(retry(page)).toHaveAttribute('aria-busy', 'false')
      let reads = state.reads
      await page.keyboard.press('Enter'); await expect.poll(() => state.reads).toBeGreaterThan(reads)
      await expect(retry(page)).toHaveAttribute('aria-busy', 'false'); await expect(retry(page)).toBeFocused()
      reads = state.reads
      await page.keyboard.press('Space'); await expect.poll(() => state.reads).toBeGreaterThan(reads)
      await expect(retry(page)).toHaveAttribute('aria-busy', 'false'); await expect(retry(page)).toBeFocused()
      await page.keyboard.press('Escape'); await expect(entry).toBeFocused(); await expect(region(page)).toHaveCount(0)
      expect(state.scans).toBe(0); expect(state.writes).toEqual([])
    })
  })
}

for (const [mode, text] of [['slow', 'timed out'], ['auth', 'Sign in to this PC'], ['offline', 'Check the connection']]) {
  test(`shows ${mode} distinctly and recovers only with a fresh GET`, async ({ page }) => {
    await page.clock.install()
    const state = await host(page); state.mode = mode
    await visit(page); await open(page)
    if (mode === 'slow') { await expect.poll(() => state.pending.length).toBe(1); await page.clock.runFor(8000) }
    await expect(region(page)).toContainText(text)
    await expect(checklist(page).locator('[data-epic-row="closed"]')).toContainText("Can't check")
    state.mode = 'valid'; await state.release()
    await retry(page).click()
    await expect(region(page)).toContainText('choose a game to check its setup')
    expect(state.scans).toBe(0); expect(state.writes).toEqual([])
  })
}

test('expires current facts while a read stalls and discards late replies after closing', async ({ page }) => {
  await page.clock.install()
  const state = await host(page)
  await visit(page); await open(page)
  await expect(region(page)).toContainText('choose a game to check its setup')
  state.mode = 'slow'
  await retry(page).focus()
  await page.clock.runFor(5000); await expect.poll(() => state.pending.length).toBe(1)
  await page.clock.runFor(5000)
  await expect(region(page)).toContainText('last setup check expired')
  await expect(retry(page)).toBeFocused()
  await page.keyboard.press('Escape')
  await state.release()
  state.mode = 'valid'
  state.facts = sample({ epoch: 'epoch-b', revision: '0', receipt: 'required', receipt_revision: '0', proof: 'required', state: 'needs_action' })
  await open(page)
  await expect(region(page)).toContainText('setup epoch changed')
  await expect(checklist(page).locator('[data-epic-row="receipt"]')).toContainText('Needs action')
  expect(state.writes).toEqual([])
})
