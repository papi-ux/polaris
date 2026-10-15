import { test, expect } from './fixtures/mocked.js'
import { inflateSync } from 'node:zlib'

const themes = ['polaris', 'portable-chrome', 'oled', 'miami', 'director', 'high-contrast']
const catalog = [
  { uuid: 'control', name: 'Control Ultimate Edition', cmd: 'control', favorite: true, 'image-path': 'control.png' },
  { uuid: 'broken', name: 'Missing Artwork', cmd: 'game', 'image-path': 'missing.png' },
  { uuid: 'long', name: 'A longer game title with additional edition information', cmd: 'game' },
]
const gpu = { name: 'Fixture GPU', vendor: 'nvidia', temperature_c: 0, utilization_pct: 0, encoder_pct: null,
  vram_used_mb: 2048, vram_total_mb: 8192 }

async function mockHost(page, theme = 'polaris', systemGpu = gpu, fixtures = {}) {
  await page.addInitScript(skin => {
    localStorage.setItem('polaris-theme', skin)
    window.__mockStats = { streaming: false }
    window.EventSource = class {
      constructor() { this.timer = setInterval(() => { this.onopen?.({}); this.onmessage?.({ data: JSON.stringify(window.__mockStats) }) }, 250) }
      close() { clearInterval(this.timer) }
    }
  }, theme)
  let apps = structuredClone(catalog)
  const writes = []
  let config = { status: true, platform: 'linux', version: '1.4.14-beta.2', sunshine_name: 'Fixture Host',
    configuration_revision: 'a'.repeat(64), linux_stream_mode: 'desktop_display', headless_mode: 'disabled',
    encoder_codec_support: { ready: true, encoder: 'nvenc', hevc_supported: true, pyrowave: { available: true, hdr: false } } }
  await page.route('**/api/**', async route => {
    const request = route.request(), path = new URL(request.url()).pathname
    if (request.method() !== 'GET') {
      expect(request.method()).toBe('POST')
      expect(['/api/apps/reorder', '/api/config']).toContain(path)
      const body = request.postDataJSON(); writes.push({ path, body })
      if (path === '/api/apps/reorder') apps = body.order.map(id => apps.find(app => app.uuid === id))
      else config = { ...config, ...body, configuration_revision: 'b'.repeat(64) }
      return route.fulfill({ json: { status: true, configuration_revision: config.configuration_revision, restart_required: true } })
    }
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/config') return route.fulfill({ json: config })
    if (path === '/api/settings/metadata') return route.fulfill({ status: 404, json: { status: false } })
    if (path === '/api/apps') return route.fulfill({ json: { status: true, apps, current_app: '', host_name: 'Fixture Host', host_uuid: 'fixture' } })
    if (path === '/api/stats/system') return route.fulfill({ json: { gpu: systemGpu, displays: [], audio: { sink: 'default' }, session_type: 'wayland' } })
    if (path === '/api/stats/stream') return route.fulfill({ json: { streaming: false } })
    if (path === '/api/multiseat/profiles') return route.fulfill({ json: { enabled: false, available: true, profiles: [], activity: [] } })
    if (path === '/api/clients/list') return route.fulfill({ json: { status: true, named_certs: [{ uuid: 'device', name: 'Fixture client' }] } })
    if (path === '/api/covers/image') {
      if (new URL(request.url()).searchParams.get('name') !== catalog[0].name) return route.fulfill({ status: 404, body: '' })
      return route.fulfill({ contentType: 'image/svg+xml', body: '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="300"><rect width="200" height="300" fill="#34465f"/><circle cx="100" cy="140" r="60" fill="#8195a8"/></svg>' })
    }
    if (path === '/api/logs') return route.fulfill({ body: '' })
    if (path === '/api/ai/history' || path === '/api/ai/cache') return route.fulfill({ json: [] })
    if (path in fixtures) return route.fulfill({ json: fixtures[path] })
    return route.fulfill({ json: { status: true, streaming: false } })
  })
  return writes
}

async function fits(locator, width) {
  const box = await locator.boundingBox()
  expect(box).not.toBeNull()
  expect(box.x).toBeGreaterThanOrEqual(-1)
  expect(box.x + box.width).toBeLessThanOrEqual(width + 1)
  const overflow = await locator.evaluate(node => ({ client: node.clientWidth, scroll: node.scrollWidth,
    children: [...node.querySelectorAll('*')].filter(child => child.getBoundingClientRect().right > node.getBoundingClientRect().right + 1)
      .map(child => ({ tag: child.tagName, classes: child.className, width: child.getBoundingClientRect().width, text: child.textContent.slice(0, 60) })) }))
  expect(overflow.scroll, JSON.stringify(overflow)).toBeLessThanOrEqual(overflow.client + 1)
}

// A control may break a word only when the word is wider than its card could ever
// give it. Words that fit stay on one line. Compound words may still break after
// a hyphen joining two letters, as in print ("self-" / "test"); version labels,
// whose hyphen follows a digit ("v1.5.0-beta.2"), may not.
async function wholeWords(locator) {
  const cuts = await locator.evaluate(node => {
    const box = node.closest('.section-card, .page-header, main')
    const outer = getComputedStyle(box), own = getComputedStyle(node)
    const room = box.clientWidth - parseFloat(outer.paddingLeft) - parseFloat(outer.paddingRight)
      - parseFloat(own.paddingLeft) - parseFloat(own.paddingRight) - parseFloat(own.borderLeftWidth) - parseFloat(own.borderRightWidth)
    const found = []
    const walker = document.createTreeWalker(node, NodeFilter.SHOW_TEXT)
    for (let text = walker.nextNode(); text; text = walker.nextNode()) {
      for (const match of text.data.matchAll(/\S+/g)) {
        const word = match[0]
        let start = 0
        const segments = []
        for (let i = 1; i < word.length - 1; i++) {
          if (word[i] === '-' && /\p{L}/u.test(word[i - 1]) && /\p{L}/u.test(word[i + 1])) {
            segments.push([start, i + 1]); start = i + 1
          }
        }
        segments.push([start, word.length])
        for (const [from, to] of segments) {
          const range = document.createRange()
          range.setStart(text, match.index + from)
          range.setEnd(text, match.index + to)
          const rects = [...range.getClientRects()].filter(rect => rect.width > 0)
          const lines = new Set(rects.map(rect => Math.round(rect.top))).size
          const width = rects.reduce((sum, rect) => sum + rect.width, 0)
          if (lines > 1 && width <= room) found.push({ word: word.slice(from, to), lines, width: Math.round(width), room: Math.round(room) })
        }
      }
    }
    return found
  })
  expect(cuts, await locator.evaluate(node => node.textContent.trim())).toEqual([])
}

// Read Chromium's actual composited screenshot, including translucent/gradient
// surfaces. PNG filters are reconstructed; unsupported formats fail explicitly.
function pixels(png) {
  expect(png.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))).toBe(true)
  const chunks = []; let width, height, channels
  for (let at = 8; at < png.length;) {
    const size = png.readUInt32BE(at), kind = png.toString('ascii', at + 4, at + 8), data = png.subarray(at + 8, at + 8 + size)
    if (kind === 'IHDR') {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4)
      expect(data[8]).toBe(8); expect([2, 6]).toContain(data[9]); expect(data[12]).toBe(0)
      channels = data[9] === 6 ? 4 : 3
    } else if (kind === 'IDAT') chunks.push(data)
    at += size + 12
  }
  const raw = inflateSync(Buffer.concat(chunks)), stride = width * channels, out = Buffer.alloc(height * stride)
  const paeth = (a, b, c) => { const p = a + b - c, x = Math.abs(p - a), y = Math.abs(p - b), z = Math.abs(p - c); return x <= y && x <= z ? a : y <= z ? b : c }
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)]; expect(filter).toBeLessThanOrEqual(4)
    for (let x = 0; x < stride; x++) {
      const i = y * stride + x, a = x >= channels ? out[i - channels] : 0, b = y ? out[i - stride] : 0,
        c = y && x >= channels ? out[i - stride - channels] : 0
      out[i] = raw[y * (stride + 1) + x + 1] + [0, a, b, Math.floor((a + b) / 2), paeth(a, b, c)][filter]
    }
  }
  return (x, y) => [...out.subarray(y * stride + x * channels, y * stride + x * channels + 3)]
}

const luminance = rgb => rgb.map(v => { const n = v / 255; return n <= 0.04045 ? n / 12.92 : ((n + 0.055) / 1.055) ** 2.4 }).reduce((sum, n, i) => sum + n * [0.2126, 0.7152, 0.0722][i], 0)
async function supportingCopyContrast(page, locator, testInfo, label) {
  expect(await page.evaluate(() => devicePixelRatio)).toBe(1)
  await locator.scrollIntoViewIfNeeded()
  const style = await locator.evaluate(node => {
    const css = getComputedStyle(node), rect = node.getBoundingClientRect(), canvas = document.createElement('canvas')
    canvas.width = canvas.height = 1
    const ctx = canvas.getContext('2d'); ctx.fillStyle = css.color; ctx.fillRect(0, 0, 1, 1)
    return { foreground: [...ctx.getImageData(0, 0, 1, 1).data], fontSize: parseFloat(css.fontSize),
      points: [rect.left + 1, rect.left + rect.width / 2, rect.right - 1].map(x => [Math.floor(x), Math.floor(rect.top + 1)]) }
  })
  expect(style.fontSize).toBeGreaterThanOrEqual(13)
  expect(style.foreground[3]).toBe(255)
  const sample = pixels(await page.screenshot({ animations: 'disabled' }))
  const measurements = style.points.map(([x, y]) => {
    const background = sample(x, y), a = luminance(style.foreground.slice(0, 3)), b = luminance(background)
    return { x, y, background, contrast: (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05) }
  })
  // The blank leading row adjacent to this text samples the rendered background;
  // this does not claim every text/hover state in the application is qualified.
  for (const measurement of measurements) expect(measurement.contrast, `${label}: ${JSON.stringify(measurement)}`).toBeGreaterThanOrEqual(4.5)
  await testInfo.attach(`${label}-rendered-contrast`, { body: JSON.stringify({ ...style, measurements }), contentType: 'application/json' })
}

for (const width of [390, 1280]) {
  for (const theme of themes) {
    test(`look and feel surfaces in ${theme} at ${width}px`, async ({ page }, testInfo) => {
      await page.setViewportSize({ width, height: 960 })
      await page.emulateMedia({ reducedMotion: 'reduce' })
      const writes = await mockHost(page, theme)
      await page.goto('/#/')
      const rail = page.locator('[data-dashboard-play-rail]'), vitals = page.locator('[data-dashboard-idle-vitals]')
      await expect(rail.locator('[data-dashboard-launch-app]')).toHaveCount(3)
      await expect(vitals).toHaveJSProperty('open', false)
      await expect(vitals.locator('summary')).toContainText('0°C temperature')
      await expect(vitals.locator('summary')).toContainText('encoder unknown')
      await supportingCopyContrast(page, vitals.locator('[data-vitals-gpu]'), testInfo, 'vitals-gpu')
      expect(await rail.evaluate((node, selector) => Boolean(node.compareDocumentPosition(document.querySelector(selector)) & Node.DOCUMENT_POSITION_FOLLOWING), '[data-dashboard-idle-vitals]')).toBe(true)
      await fits(rail, width)
      await rail.screenshot({ path: `test-results/look-feel-dashboard-${theme}-${width}.png` })
      await vitals.locator('summary').focus(); await page.keyboard.press('Enter')
      await expect(vitals).toHaveJSProperty('open', true)
      await page.goto('/#/apps')
      const rows = page.locator('[data-library-app]')
      await expect(rows).toHaveCount(3)
      await expect(rows.nth(0).getByRole('img', { name: 'Control Ultimate Edition artwork', exact: true })).toBeVisible()
      await expect(rows.nth(1).getByRole('img', { name: 'Missing Artwork artwork unavailable', exact: true })).toBeVisible()
      await expect(rows.nth(2).getByRole('img', { name: `${catalog[2].name} artwork unavailable`, exact: true })).toBeVisible()
      for (const row of await rows.all()) await fits(row, width)
      await expect(rows.first().getByRole('button', { name: 'Delete Control Ultimate Edition', exact: true })).not.toBeVisible()
      await supportingCopyContrast(page, page.locator('.page-subtitle').first(), testInfo, 'library-subtitle')
      await page.screenshot({ path: `test-results/look-feel-library-${theme}-${width}.png`, animations: 'disabled' })
      await page.goto('/#/config')
      const bar = page.locator('.settings-command-bar')
      await expect(page.locator('[data-command-note]')).toBeVisible()
      await fits(bar, width)
      await supportingCopyContrast(page, page.locator('[data-command-note]'), testInfo, 'settings-command-note')
      if (width < 768) {
        const header = page.locator('[data-mobile-header]')
        await expect(header).toBeVisible()
        expect((await header.boundingBox()).height).toBeLessThanOrEqual(72)
        expect(await bar.evaluate(node => getComputedStyle(node).position)).toBe('static')
        const menu = header.getByRole('button', { name: 'Toggle navigation' })
        const size = await menu.boundingBox(); expect(size.width).toBeGreaterThanOrEqual(44); expect(size.height).toBeGreaterThanOrEqual(44)
      }
      await bar.screenshot({ path: `test-results/look-feel-settings-${theme}-${width}.png` })
      await page.goto('/#/config#pyrowave')
      await supportingCopyContrast(page, page.locator('[data-pyrowave-selection]'), testInfo, 'settings-helper')
      await supportingCopyContrast(page, page.locator('[data-pyrowave-stream] > p').first(), testInfo, 'pyrowave-readout')
      expect(writes).toEqual([])
    })
  }

  test(`keyboard reorder stays local, preserves focus and refuses filtered moves at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    const writes = await mockHost(page)
    await page.goto('/#/apps')
    const rows = page.locator('[data-library-app]'), up = page.locator('[data-move-up="broken"]'), down = page.locator('[data-move-down="broken"]')
    await up.focus(); await page.keyboard.press('Enter')
    await expect(rows.first()).toHaveAttribute('data-library-app', 'broken')
    await expect(up).toBeDisabled(); await expect(down).toBeFocused()
    await expect(page.locator('[data-reorder-status]')).toContainText('Missing Artwork moved to position 1')
    expect(writes).toEqual([])
    await page.keyboard.press('Enter')
    await expect(rows.nth(1)).toHaveAttribute('data-library-app', 'broken')
    await expect(down).toBeFocused()
    await up.focus(); await page.keyboard.press('Enter')
    await expect(rows.first()).toHaveAttribute('data-library-app', 'broken')
    await expect(down).toBeFocused()
    const search = page.getByRole('searchbox', { name: 'Search library apps' })
    await search.fill('Missing')
    await expect(rows).toHaveCount(1)
    await expect(up).toBeDisabled(); await expect(down).toBeDisabled()
    await search.fill('')
    await page.getByRole('button', { name: /Save order/i }).click()
    await expect.poll(() => writes).toEqual([{ path: '/api/apps/reorder', body: { order: ['broken', 'control', 'long'] } }])
    await expect(rows.first()).toHaveAttribute('data-library-app', 'broken')
    await expect(page.getByRole('button', { name: /Save order/i })).toHaveCount(0)
    const more = page.locator('[data-library-more="broken"]')
    await more.locator('summary').focus(); await page.keyboard.press('Enter')
    await expect(more.getByRole('button', { name: 'Delete Missing Artwork', exact: true })).toBeVisible()
    const deletion = more.getByRole('button', { name: 'Delete Missing Artwork', exact: true })
    await deletion.scrollIntoViewIfNeeded()
    expect(await deletion.evaluate(node => { const r = node.getBoundingClientRect(); return node.contains(document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2)) })).toBe(true)
    page.once('dialog', dialog => dialog.dismiss())
    await deletion.click()
    await expect(more).toHaveJSProperty('open', false)
    await expect(more.locator('summary')).toBeFocused()
    await page.keyboard.press('Enter')
    await page.keyboard.press('Escape')
    await expect(more).toHaveJSProperty('open', false)
    await expect(more.locator('summary')).toBeFocused()
    await page.keyboard.press('Enter')
    await search.click()
    await expect(more).toHaveJSProperty('open', false)
    expect(writes).toHaveLength(1)
  })

  test(`pending and saved settings keep one accessible restart consequence at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    const writes = await mockHost(page)
    await page.goto('/#/config')
    await page.locator('#sunshine_name').fill('Edited Fixture Host')
    const bar = page.locator('.settings-command-bar'), restart = bar.getByRole('button', { name: 'Save and restart Polaris', exact: true })
    const mentions = () => bar.locator('*').evaluateAll(nodes => nodes.filter(n => !n.children.length && /disconnects any active streams/.test(n.textContent)).length)
    expect(await mentions()).toBe(1)
    await expect(restart).toHaveAccessibleDescription(/disconnects any active streams/)
    await expect(page.locator('[data-command-note]')).toHaveCount(0)
    await bar.getByRole('button', { name: 'Save', exact: true }).click()
    await expect(page.locator('[data-save-badge]')).toHaveText('Saved · restart needed')
    expect(await mentions()).toBe(1)
    await expect(page.locator('[data-command-note]')).toHaveText('Saved. Polaris uses these settings after it restarts.')
    await expect(restart).toBeEnabled()
    await expect(restart).toHaveAccessibleDescription(/disconnects any active streams/)
    expect(writes).toHaveLength(1); expect(writes[0].path).toBe('/api/config')
    expect(writes[0].body.sunshine_name).toBe('Edited Fixture Host')
    await fits(bar, width)
    await bar.screenshot({ path: `test-results/look-feel-saved-${width}.png` })
  })

  test(`settings and launch surfaces reflow at a 32px default font with reduced motion at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    const cdp = await page.context().newCDPSession(page)
    await cdp.send('Page.setFontSizes', { fontSizes: { standard: 32 } })
    await mockHost(page)
    await page.goto('/#/config')
    expect(await page.evaluate(() => getComputedStyle(document.documentElement).fontSize)).toBe('32px')
    expect(await page.evaluate(() => matchMedia('(min-width:80rem)').matches)).toBe(false)
    const bar = page.locator('.settings-command-bar')
    await fits(bar, width)
    for (const button of await bar.getByRole('button').all()) await fits(button, width)
    const search = page.getByRole('searchbox', { name: 'Search settings', exact: true })
    await search.fill('Server Name'); await search.press('Enter')
    await expect(page.locator('#sunshine_name')).toBeFocused()
    await search.fill(''); await search.press('Escape')
    await expect(search).toBeFocused()
    await bar.screenshot({ path: `test-results/look-feel-text-scale-settings-${width}.png` })
    await page.goto('/#/apps')
    const rows = page.locator('[data-library-app]')
    await expect(rows).toHaveCount(3)
    for (const row of await rows.all()) {
      await fits(row, width)
      expect((await row.locator('h3').boundingBox()).width).toBeGreaterThan(80)
    }
    for (const card of await page.locator('[data-quick-launch-app]').all()) {
      await fits(card, width)
      expect((await card.locator('h3').boundingBox()).width).toBeGreaterThan(80)
      await fits(card.locator('.library-quick-launch-action'), width)
    }
    await rows.first().screenshot({ path: `test-results/look-feel-text-scale-library-${width}.png` })
    const motion = await rows.first().evaluate(node => getComputedStyle(node).transitionDuration)
    expect(motion.split(',').every(duration => parseFloat(duration) <= 0.01)).toBe(true)
    await page.goto('/#/')
    const tiles = page.locator('.dashboard-play-tile')
    await expect(tiles).toHaveCount(3)
    for (const tile of await tiles.all()) { await fits(tile, width); await fits(tile.locator('[data-poster-image]'), width) }
    await page.locator('[data-dashboard-play-rail]').screenshot({ path: `test-results/look-feel-text-scale-dashboard-${width}.png` })
  })
}

// 200% desktop zoom has a 640x480 CSS layout viewport. This models layout
// reflow only; it is not an OS/browser zoom interaction acceptance claim.
test('launch surfaces and settings reflow in a 640px zoom layout', async ({ page }) => {
  await page.setViewportSize({ width: 640, height: 480 })
  await mockHost(page)
  for (const path of ['/#/', '/#/apps', '/#/config']) {
    await page.goto(path)
    if (path === '/#/') await expect(page.locator('[data-dashboard-launch-app]')).toHaveCount(3)
    else if (path === '/#/apps') await expect(page.locator('[data-library-app]')).toHaveCount(3)
    else await expect(page.locator('.settings-command-bar')).toBeVisible()
    for (const surface of await page.locator('[data-dashboard-play-rail], [data-quick-launch-app], [data-library-app], .settings-command-bar').all()) await fits(surface, 640)
  }
})

test('collapsed vitals keep pressure and missing readings legible without colour', async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 960 })
  await mockHost(page, 'polaris', { ...gpu, temperature_c: 85, utilization_pct: null, encoder_pct: 85, vram_used_mb: 7373 })
  await page.goto('/#/')
  const vitals = page.locator('[data-dashboard-idle-vitals]')
  await expect(vitals).toHaveJSProperty('open', false)
  await expect(vitals.locator('summary')).toContainText('85°C temperature · high')
  await expect(vitals.locator('summary')).toContainText('GPU unknown')
  await expect(vitals.locator('summary')).toContainText('85% encoder · high')
  await expect(vitals.locator('summary')).toContainText('90% VRAM · high')
  await fits(vitals, 390)
})

for (const width of [390, 1280]) {
  for (const theme of ['polaris', 'director', 'oled', 'high-contrast']) {
    test(`Doctor cards and controls reflow with 32px text in ${theme} at ${width}px`, async ({ page }, testInfo) => {
      await page.setViewportSize({ width, height: 960 })
      await page.emulateMedia({ reducedMotion: 'reduce' })
      const cdp = await page.context().newCDPSession(page)
      await cdp.send('Page.setFontSizes', { fontSizes: { standard: 32 } })
      const writes = await mockHost(page, theme)
      await page.goto('/#/troubleshooting')
      await expect(page.getByRole('heading', { name: 'Doctor & Support', exact: true })).toBeVisible()
      expect(await page.evaluate(() => getComputedStyle(document.documentElement).fontSize)).toBe('32px')
      // Check both a browser font preference and CSS-root text scaling. The latter
      // enlarges controls without changing the media-query initial font size.
      for (const cssScale of [false, true]) {
        if (cssScale) {
          await cdp.send('Page.setFontSizes', { fontSizes: { standard: 16 } })
          await page.evaluate(() => { document.documentElement.style.fontSize = '32px' })
        }
        await fits(page.locator('main'), width)
        for (const card of await page.locator('main .section-card, [data-support-self-tests] .surface-subtle').all()) await fits(card, width)
        for (const control of await page.locator('[data-fix-my-stream] .troubleshooting-action-button, [data-session-snapshot] .meta-pill, .troubleshooting-page-header .meta-pill').all()) await fits(control, width)
        for (const control of await page.locator('main .troubleshooting-action-button, [data-session-snapshot] .meta-pill, .troubleshooting-page-header .meta-pill').all()) await wholeWords(control)
      }
      await supportingCopyContrast(page, page.locator('.page-subtitle').first(), testInfo, 'doctor-subtitle')
      expect(writes).toEqual([])
      await page.screenshot({ path: `test-results/look-feel-doctor-text-${theme}-${width}.png`, animations: 'disabled' })
    })
  }
}

// Director's Polaris name is a nod to Control and the longest theme name. Every name must read in
// full, as text, in the open picker at phone and desktop widths and with doubled text; the sidebar
// shows the whole current name too, on its own line when it cannot share one with "Appearance".
const themeNames = ['Polaris Aurora', 'Portable Chrome', 'Console OLED', 'Miami Nebula', '< Congratulations, Director >', 'High Contrast']
async function openDirectorSidebar(page, width, font) {
  await page.setViewportSize({ width, height: 960 })
  await page.emulateMedia({ reducedMotion: 'reduce' })
  const writes = await mockHost(page, 'director')
  await page.addInitScript(size => document.addEventListener('DOMContentLoaded', () => { document.documentElement.style.fontSize = `${size}px` }), font)
  await page.goto('/#/')
  await expect(page.locator('[data-dashboard-play-rail]')).toBeVisible()
  expect(await page.evaluate(() => getComputedStyle(document.documentElement).fontSize)).toBe(`${font}px`)
  expect(await page.evaluate(() => document.documentElement.getAttribute('data-theme'))).toBe('director')
  if (width < 768) await page.locator('[data-mobile-header]').getByRole('button', { name: 'Toggle navigation' }).click()
  const toggle = page.getByRole('button', { name: 'Choose theme' })
  await expect(toggle).toBeVisible()
  return { writes, toggle }
}

for (const width of [390, 1280]) {
  for (const font of [16, 32]) {
    test(`sidebar names the current theme whole at ${width}px with ${font}px text`, async ({ page }) => {
      const { writes, toggle } = await openDirectorSidebar(page, width, font)
      await expect(toggle).toHaveAttribute('title', 'Theme: < Congratulations, Director >')
      const current = toggle.getByText('< Congratulations, Director >', { exact: true })
      await expect(current).toBeVisible()
      const clipped = await current.evaluate(node => ({ scroll: node.scrollWidth, client: node.clientWidth, text: node.textContent }))
      expect(clipped.scroll, JSON.stringify(clipped)).toBeLessThanOrEqual(clipped.client + 1)
      // The phone drawer is sized in rem and is wider than a 390px viewport with 32px text; that
      // predates this name, so the drawer itself is only required to fit at desktop widths.
      if (width >= 768) await fits(toggle, width)
      // The fixture host runs ahead of the newest release, an informational state whose light
      // borrows the accent. Director's accent is red, so that light must not read as an alarm.
      const light = page.locator('[data-sidebar-update-status-light]')
      await expect(light).toHaveClass(/\bbg-accent\b/)
      const colors = await light.evaluate(node => {
        const probe = document.createElement('span')
        probe.style.color = 'var(--color-accent)'; document.body.append(probe)
        const accent = getComputedStyle(probe).color; probe.remove()
        return { light: getComputedStyle(node).backgroundColor, accent }
      })
      expect(colors.light, JSON.stringify(colors)).not.toBe(colors.accent)
      expect(writes).toEqual([])
    })

    test(`theme picker shows every theme name whole at ${width}px with ${font}px text`, async ({ page }) => {
      const { writes, toggle } = await openDirectorSidebar(page, width, font)
      await toggle.click()
      const listbox = page.getByRole('listbox', { name: 'Theme' })
      await expect(listbox).toBeVisible()
      await expect(listbox.getByRole('option')).toHaveCount(themeNames.length)
      await expect(listbox.getByRole('option', { selected: true })).toContainText('< Congratulations, Director >')
      await fits(listbox, width)
      // The open list stays on screen above its button; a longer list scrolls inside it.
      const list = await listbox.boundingBox()
      expect(list.y, JSON.stringify(list)).toBeGreaterThanOrEqual(0)
      expect(list.y + list.height, JSON.stringify(list)).toBeLessThanOrEqual(960)
      for (const name of themeNames) {
        const label = listbox.getByRole('option').getByText(name, { exact: true })
        await expect(label).toBeVisible()
        const box = await label.evaluate(node => ({ scroll: node.scrollWidth, client: node.clientWidth, text: node.textContent,
          overflow: getComputedStyle(node).textOverflow, rendered: node.innerText }))
        expect(box.text).toBe(name)
        expect(box.rendered.replace(/\s+/g, ' ').trim(), JSON.stringify(box)).toBe(name)
        expect(box.scroll, JSON.stringify(box)).toBeLessThanOrEqual(box.client + 1)
        // A word may break only when it is wider than the option could ever give a name: its
        // content box less the fixed swatch and its gap.
        const cuts = await label.evaluate(node => {
          const option = node.closest('[role="option"]'), swatch = option.querySelector('.theme-picker-swatch')
          const style = getComputedStyle(option)
          const room = option.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight)
            - swatch.getBoundingClientRect().width - parseFloat(style.columnGap)
          const text = node.firstChild, found = []
          for (const match of text.data.matchAll(/\S+/g)) {
            const range = document.createRange()
            range.setStart(text, match.index); range.setEnd(text, match.index + match[0].length)
            const rects = [...range.getClientRects()].filter(rect => rect.width > 0)
            const width = rects.reduce((sum, rect) => sum + rect.width, 0)
            if (new Set(rects.map(rect => Math.round(rect.top))).size > 1 && width <= room) found.push({ word: match[0], width, room })
          }
          return found
        })
        expect(cuts, name).toEqual([])
        // The name stays beside its swatch: the option never wraps its text below the preview.
        const side = await label.evaluate(node => {
          const swatch = node.closest('[role="option"]').querySelector('.theme-picker-swatch').getBoundingClientRect()
          const text = node.getBoundingClientRect()
          return { swatchRight: swatch.right, textLeft: text.left, swatchTop: swatch.top, textBottom: text.bottom }
        })
        expect(side.textLeft, `${name} ${JSON.stringify(side)}`).toBeGreaterThanOrEqual(side.swatchRight)
        expect(side.textBottom, `${name} ${JSON.stringify(side)}`).toBeGreaterThan(side.swatchTop)
      }
      expect(writes).toEqual([])
      await page.screenshot({ path: `test-results/look-feel-theme-picker-director-${width}-${font}.png`, animations: 'disabled' })
    })
  }
}

// A launcher can be one 255-character file name, and a folder's first warning can name a long
// path. Both wrap inside the ROM folder and readiness cards instead of widening the page, while an
// ordinary folder at normal text size keeps its label and status side by side.
const longLauncher = `/synthetic/Applications/Eden_${'X'.repeat(241)}.AppImage`
const romSources = { status: true, presets: [], sources: [
  { id: 'long', path: '/synthetic/ROMs', emulator: 'eden', label: 'Eden', install: { kind: 'launcher', location: longLauncher },
    prerequisites: [{ id: 'keys', severity: 'warning', message: `Eden has no prod.keys in /synthetic/keys/${'K'.repeat(200)}, so no game will boot.`, action: 'Add prod.keys' }],
    rom_count: 1 },
  { id: 'ready', path: '/synthetic/Games', emulator: 'eden', label: 'Eden', install: { kind: 'native', location: '/usr/bin/eden' },
    prerequisites: [], rom_count: 3 },
] }

for (const width of [390, 1280]) {
  for (const font of [16, 32]) {
    test(`ROM folders and readiness keep long launcher and status text inside their cards at ${width}px with ${font}px text`, async ({ page }) => {
      await page.setViewportSize({ width, height: 960 })
      await page.emulateMedia({ reducedMotion: 'reduce' })
      const writes = await mockHost(page, 'director', gpu, { '/api/library/sources': romSources })
      await page.addInitScript(size => document.addEventListener('DOMContentLoaded', () => { document.documentElement.style.fontSize = `${size}px` }), font)
      await page.goto('/#/apps')
      await expect(page.getByRole('heading', { name: 'Library', exact: true })).toBeVisible()
      await page.getByRole('button', { name: 'Import Games', exact: true }).click()
      const rows = page.locator('[data-rom-readiness] .grid > div')
      await expect(rows).toHaveCount(2)
      await expect(page.locator('[data-rom-folder]')).toHaveCount(2)
      expect(await page.evaluate(() => getComputedStyle(document.documentElement).fontSize)).toBe(`${font}px`)
      await fits(page.locator('main'), width)
      for (const card of await page.locator('[data-rom-folders], [data-rom-folder], [data-rom-readiness], [data-rom-readiness] .grid > div').all()) await fits(card, width)
      for (const text of await page.locator('[data-rom-folder] .text-sm.font-semibold, [data-rom-readiness] .grid > div > span').all()) {
        await fits(text, width)
        await wholeWords(text)
      }
      await expect(rows.first().locator('span').first()).toHaveText(`Eden at ${longLauncher}`)
      if (font === 16) {
        // The ordinary folder reads as before: label on the left, status on the same line at the right.
        const [label, status, row] = await Promise.all([rows.nth(1).locator('span').first().boundingBox(),
          rows.nth(1).locator('span').last().boundingBox(), rows.nth(1).boundingBox()])
        expect(Math.abs(label.y - status.y)).toBeLessThan(4)
        expect(row.x + row.width - (status.x + status.width)).toBeLessThan(20)
        const folder = page.locator('[data-rom-folder]').nth(1)
        const [name, count] = await Promise.all([folder.locator('.text-sm.font-semibold').boundingBox(), folder.locator('.control-chip').boundingBox()])
        expect(Math.abs(name.y + name.height / 2 - (count.y + count.height / 2))).toBeLessThan(4)
      }
      expect(writes).toEqual([])
      // A taller viewport only, so a card that wraps a long path is captured whole; widths are unchanged.
      await page.setViewportSize({ width, height: 4800 })
      await page.locator('[data-rom-folders]').screenshot({ path: `test-results/look-feel-rom-folders-${width}-${font}.png`, animations: 'disabled' })
      await page.locator('[data-rom-readiness]').screenshot({ path: `test-results/look-feel-rom-readiness-${width}-${font}.png`, animations: 'disabled' })
    })
  }
}
