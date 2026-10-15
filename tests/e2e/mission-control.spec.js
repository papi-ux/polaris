import { test, expect } from './fixtures/mocked.js'
import { writeFileSync } from 'node:fs'
import { inflateSync } from 'node:zlib'

// Mission Control's stage, readings, route, Doctor note and Diagnostics in a browser, against a
// mocked host: live, idle, unavailable and loading; the opt-in display preview; dialogs, keyboard
// and focus; and phones and tablets in both orientations with normal and doubled text. These are
// software browser checks, not physical device acceptance.

const themes = ['polaris', 'portable-chrome', 'oled', 'miami', 'director', 'high-contrast']
const catalog = [
  { uuid: 'control', name: 'Control Ultimate Edition', cmd: 'control', source: 'steam', 'image-path': 'control.png', 'last-launched': 1760500000 },
  { uuid: 'hades', name: 'Hades II', cmd: 'hades', source: 'steam', 'image-path': 'hades.png', 'last-launched': 1760400000 },
  { uuid: 'long', name: 'A longer game title with additional edition information', cmd: 'game', 'last-launched': 1760300000 },
  { uuid: 'balatro', name: 'Balatro', cmd: 'balatro', source: 'heroic', 'image-path': 'missing.png', 'last-launched': 1760200000 },
]
const gpu = { name: 'Fixture GPU', vendor: 'nvidia', temperature_c: 54, utilization_pct: 38, encoder_pct: 21,
  vram_used_mb: 6349, vram_total_mb: 24576, power_draw_w: 120 }
// A pale cover is the hardest case for text over artwork: the scrim must keep copy readable.
const cover = (fill, mark) => `<svg xmlns="http://www.w3.org/2000/svg" width="400" height="600"><rect width="400" height="600" fill="${fill}"/><circle cx="200" cy="250" r="120" fill="${mark}"/><rect y="440" width="400" height="160" fill="#ffffff"/></svg>`
const covers = { [catalog[0].name]: cover('#e8e4dc', '#c8261d'), [catalog[1].name]: cover('#f2d27a', '#2a4a5a') }
// Steam backgrounds and logos from the host artwork cache. Control's pale background is the worst
// case for copy over it; Hades has no background and a logo too dark for the stage; Balatro has a
// pale background and a light grey logo that holds 3:1 only if no more than half of a bright
// background shows through the scrim.
const paleHero = '<svg xmlns="http://www.w3.org/2000/svg" width="1920" height="620"><rect width="1920" height="620" fill="#ece8df"/><rect x="1100" width="820" height="620" fill="#b8231b"/></svg>'
const heroes = { control: paleHero, balatro: paleHero }
const logo = fill => `<svg xmlns="http://www.w3.org/2000/svg" width="640" height="200"><rect x="20" y="40" width="600" height="120" rx="24" fill="${fill}"/></svg>`
// The long title's logo is white at 2% opacity: present, but too faint to stand in for its name.
const logos = { control: logo('#f4f1ea'), hades: logo('#15110f'), long: logo('#ffffff" fill-opacity="0.02'), balatro: logo('#c8c8c8') }
const live = {
  streaming: true, fps: 119.6, session_target_fps: 120, latency_ms: 11, bitrate_kbps: 48000, encode_time_ms: 3.1,
  codec: 'hevc', width: 1920, height: 1080, hdr_effective_mode: 'sdr_8bit', hdr_downgrade_reason: 'none',
  client_name: 'Fixture handheld', active_sessions: 1,
  clients: [{ name: 'Fixture handheld', ip: '192.0.2.42', client_family: 'nova', fps: 119.6, latency_ms: 11, codec: 'hevc', width: 1920, height: 1080 }],
  capture_path: 'cpu_copy', capture_transport: 'shm', capture_path_reason: 'headless_shm_default', capture_cpu_copy: true,
  stream_display_mode: 'Private Stream', runtime_backend: 'labwc', runtime_effective_headless: true,
  dropped_frame_ratio: 0.001, duplicate_frame_ratio: 0, frame_interval_error_ms: 0.4,
  network_verdict: { loss_pct: 0.4, loss_state: 'light', window_seconds: 20, frames_lost: 4, frames_expected: 1000 },
  doctor: {
    traffic_light: 'amber', primary_issue: 'network_loss', summary: 'Light video frame loss. The stream is holding.',
    confidence: { level: 'high' },
    recommendation: { title: 'Watch the loss', body: 'If it keeps rising, Doctor can apply one guarded bitrate step.' },
    safe_recovery_action: { kind: 'lower_bitrate', label: 'Fix and verify', endpoint: '/api/doctor/action', requires_confirmation: true,
      rollback: 'Undo restores the previous bitrate.', payload_preview: { action_id: 'lower_bitrate', app_session_id: 'fixture-session', session_generation: 3 } },
  },
}
const idle = { streaming: false }
const spacesState = extra => ({ enabled: true, available: true, changing: false, failed: false,
  profiles: [{ id: 'living', name: 'Living room', family: 'steam', clients: ['device'] }], activity: [], ...extra })
const spacesOff = { enabled: false, available: true, changing: false, failed: false, profiles: [], activity: [] }

async function mockHost(page, { theme = 'director', stats = idle, certs = [{ uuid: 'device', name: 'Fixture handheld', client_family: 'nova', last_seen_at: 0 }],
  currentApp = '', systemGpu = gpu, history = [], spaces = spacesOff } = {}) {
  const host = { stats, spaces, streamDown: false, systemDown: false, requests: [], writes: [] }
  await page.addInitScript(({ skin, initial }) => {
    localStorage.setItem('polaris-theme', skin)
    window.__mockStats = initial
    window.__holdStats = false
    window.__streams = []
    window.EventSource = class {
      constructor() {
        window.__streams.push(this)
        this.timer = setInterval(() => {
          if (!window.__holdStats) { this.onopen?.({}); this.onmessage?.({ data: JSON.stringify(window.__mockStats) }) }
        }, 250)
      }
      close() { clearInterval(this.timer) }
    }
    // Drop the stats channel: the open stream errors, and its fallback polling is refused by the route.
    window.__dropStats = () => { window.__holdStats = true; for (const stream of window.__streams) { clearInterval(stream.timer); stream.onerror?.({}) } }
  }, { skin: theme, initial: stats })
  const config = { status: true, platform: 'linux', version: '1.5.0-beta.2', sunshine_name: 'Fixture Host',
    configuration_revision: 'a'.repeat(64), linux_stream_mode: 'headless_stream', headless_mode: 'enabled',
    enable_discovery: 'enabled', enable_pairing: 'enabled', stream_audio: 'enabled',
    encoder_codec_support: { ready: true, encoder: 'nvenc', hevc_supported: true } }
  await page.route('**/api/**', async route => {
    const request = route.request(), url = new URL(request.url()), path = url.pathname
    host.requests.push(`${request.method()} ${path}`)
    if (request.method() !== 'GET') {
      // Mission Control's own checks never confirm a host action; anything else fails the case.
      host.writes.push(`${request.method()} ${path}`)
      return route.fulfill({ status: 403, json: { status: false } })
    }
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/config') return route.fulfill({ json: config })
    if (path === '/api/settings/metadata') return route.fulfill({ status: 404, json: { status: false } })
    if (path === '/api/apps') return route.fulfill({ json: { status: true, apps: catalog, current_app: currentApp, host_name: 'Fixture Host', host_uuid: 'fixture' } })
    if (path === '/api/stats/system') {
      if (host.systemDown || !systemGpu) return route.fulfill({ status: 503, json: { status: false, error: 'unavailable' } })
      return route.fulfill({ json: { gpu: systemGpu, displays: [{ name: 'HEADLESS-1' }], audio: { sink: 'alsa_output.usb-Fixture_DAC-00.analog-stereo' }, session_type: 'wayland' } })
    }
    if (path === '/api/stats/stream') {
      if (host.streamDown) return route.fulfill({ status: 503, json: { status: false } })
      return route.fulfill({ json: host.stats })
    }
    if (path === '/api/multiseat/profiles') return route.fulfill({ json: host.spaces })
    if (path === '/api/clients/list') return route.fulfill({ json: { status: true, platform: 'linux', named_certs: certs } })
    const artwork = path.match(/^\/api\/apps\/([^/]+)\/artwork\/(hero|logo)$/)
    if (artwork) {
      const svg = (artwork[2] === 'hero' ? heroes : logos)[artwork[1]]
      return svg ? route.fulfill({ contentType: 'image/svg+xml', body: svg }) : route.fulfill({ status: 404, body: '' })
    }
    if (path === '/api/covers/image') {
      const svg = covers[url.searchParams.get('name')]
      return svg ? route.fulfill({ contentType: 'image/svg+xml', body: svg }) : route.fulfill({ status: 404, body: '' })
    }
    if (path === '/api/display/stream' || path === '/api/display/screenshot') {
      return route.fulfill({ contentType: 'image/svg+xml', body: '<svg xmlns="http://www.w3.org/2000/svg" width="1920" height="1080"><rect width="1920" height="1080" fill="#20303a"/></svg>' })
    }
    if (path === '/api/logs') return route.fulfill({ body: '' })
    if (path === '/api/ai/history') return route.fulfill({ json: history })
    if (path === '/api/ai/cache') return route.fulfill({ json: [] })
    return route.fulfill({ json: { status: true } })
  })
  host.setStats = async next => { host.stats = next; await page.evaluate(value => { window.__mockStats = value }, next) }
  host.displayReads = () => host.requests.filter(entry => /\/api\/display\/(stream|screenshot)$/.test(entry))
  return host
}

// The app's main pane scrolls on its own, so a page screenshot shows one viewport. Each capture
// takes the top of the page and, when asked, a second frame scrolled to the deck below the stage.
async function shot(page, testInfo, name, { deck = false } = {}) {
  await page.locator('#polaris-main').evaluate(node => node.scrollTo(0, 0))
  await page.screenshot({ path: testInfo.outputPath(name.replace(/\.png$/, '-top.png')), animations: 'disabled' })
  if (!deck) return
  await page.locator('.mc-deck').evaluate(node => node.scrollIntoView({ block: 'start' }))
  await page.screenshot({ path: testInfo.outputPath(name.replace(/\.png$/, '-deck.png')), animations: 'disabled' })
}

// Each word of Director's greeting sits on one line, and the greeting is not cut off.
async function greetingWhole(page, label) {
  const greeting = page.locator('[data-director-greeting]')
  await expect(greeting).toHaveText('< Congratulations, Director >')
  const shape = await greeting.evaluate(node => {
    const text = node.textContent, words = [], range = document.createRange()
    const walker = document.createTreeWalker(node, NodeFilter.SHOW_TEXT)
    for (let current = walker.nextNode(); current; current = walker.nextNode()) {
      for (const match of current.textContent.matchAll(/\S+/g)) {
        range.setStart(current, match.index); range.setEnd(current, match.index + match[0].length)
        const lines = new Set([...range.getClientRects()].filter(rect => rect.width > 0).map(rect => Math.round(rect.top)))
        words.push({ word: match[0], lines: lines.size })
      }
    }
    return { text, fontSize: parseFloat(getComputedStyle(node).fontSize), split: words.filter(word => word.lines > 1).map(word => word.word),
      scroll: node.scrollWidth, client: node.clientWidth }
  })
  expect(shape.split, `${label} ${JSON.stringify(shape)}`).toEqual([])
  expect(shape.scroll, `${label} ${JSON.stringify(shape)}`).toBeLessThanOrEqual(shape.client + 1)
  // It may start below the fold on a short landscape screen; scrolled to, it fits on screen.
  await greeting.scrollIntoViewIfNeeded()
  await withinViewport(greeting, `${label} greeting`)
  return shape.fontSize
}

const stage = page => page.locator('[data-mission-stage]')
async function stageMode(page, mode) {
  await expect(stage(page)).toHaveAttribute('data-stage-mode', mode, { timeout: 15000 })
}

async function withinViewport(locator, label) {
  const box = await locator.boundingBox()
  const view = await locator.page().evaluate(() => ({ width: innerWidth, height: innerHeight }))
  expect(box, label).not.toBeNull()
  const detail = `${label} ${JSON.stringify({ box, view })}`
  expect(box.x, detail).toBeGreaterThanOrEqual(-1)
  expect(box.y, detail).toBeGreaterThanOrEqual(-1)
  expect(box.x + box.width, detail).toBeLessThanOrEqual(view.width + 1)
  expect(box.y + box.height, detail).toBeLessThanOrEqual(view.height + 1)
  return { box, view }
}

async function noSidewaysPage(page, label) {
  const sizes = await page.evaluate(() => {
    const main = document.querySelector('#polaris-main'), right = main.getBoundingClientRect().left + main.clientWidth
    const over = [...main.querySelectorAll('*')].filter(node => node.getBoundingClientRect().right > right + 1)
    const cut = over.filter(node => !over.some(other => other !== node && node.contains(other)))
      .slice(0, 4).map(node => `${node.tagName}.${String(node.className).slice(0, 60)} ${Math.round(node.getBoundingClientRect().width)}`)
    return { doc: document.documentElement.scrollWidth, body: document.body.scrollWidth, view: innerWidth, main: main.scrollWidth, mainClient: main.clientWidth, cut }
  })
  expect(sizes.doc, `${label} ${JSON.stringify(sizes)}`).toBeLessThanOrEqual(sizes.view)
  expect(sizes.body, `${label} ${JSON.stringify(sizes)}`).toBeLessThanOrEqual(sizes.view)
  expect(sizes.main, `${label} ${JSON.stringify(sizes)}`).toBeLessThanOrEqual(sizes.mainClient + 1)
}

// Scrolled into view, inside the viewport, the element a tap lands on, and its label whole.
async function reachable(locator, label) {
  await locator.scrollIntoViewIfNeeded()
  await withinViewport(locator, label)
  const hit = await locator.evaluate(node => {
    const rect = node.getBoundingClientRect()
    const top = document.elementFromPoint(rect.left + rect.width / 2, rect.top + rect.height / 2)
    const ellipsized = [node, ...node.querySelectorAll('*')].filter(el => getComputedStyle(el).textOverflow === 'ellipsis' && el.scrollWidth > el.clientWidth + 1)
      .map(el => el.textContent.trim().slice(0, 40))
    return { hit: Boolean(top && (top === node || node.contains(top))), at: top ? `${top.tagName}.${String(top.className).slice(0, 60)}` : null,
      scroll: node.scrollWidth, client: node.clientWidth, ellipsized }
  })
  expect(hit.hit, `${label} ${JSON.stringify(hit)}`).toBe(true)
  expect(hit.scroll, `${label} ${JSON.stringify(hit)}`).toBeLessThanOrEqual(hit.client + 1)
  expect(hit.ellipsized, `${label} ${JSON.stringify(hit)}`).toEqual([])
}

// Every visible control Mission Control shows takes at least a 44 px square on a coarse pointer.
async function touchTargets(page, label) {
  const small = await page.evaluate(() => {
    const view = document.querySelector('.mission-control-view')
    return [...view.querySelectorAll('a[href], button, summary, [role="switch"], input, select')]
      .filter(node => {
        const rect = node.getBoundingClientRect(), style = getComputedStyle(node)
        return rect.width > 0 && rect.height > 0 && style.visibility !== 'hidden' && !node.closest('[hidden]')
      })
      .map(node => ({ node, rect: node.getBoundingClientRect() }))
      .filter(({ rect }) => rect.width < 43.5 || rect.height < 43.5)
      .map(({ node, rect }) => `${node.tagName} "${(node.getAttribute('aria-label') || node.textContent).trim().slice(0, 40)}" ${Math.round(rect.width)}x${Math.round(rect.height)}`)
  })
  expect(small, label).toEqual([])
}

// Read Chromium's composited screenshot to measure copy against what is actually behind it.
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

// Copy over the stage is sampled against the rendered pixels just above its first line.
async function readableOverArt(page, locator, label) {
  await locator.scrollIntoViewIfNeeded()
  const style = await locator.evaluate(node => {
    // The first line of the text itself, not the block's full width, which can reach past the copy.
    const range = document.createRange(); range.selectNodeContents(node)
    const css = getComputedStyle(node), rect = [...range.getClientRects()].find(r => r.width > 0) || node.getBoundingClientRect(), canvas = document.createElement('canvas')
    canvas.width = canvas.height = 1
    const ctx = canvas.getContext('2d'); ctx.fillStyle = css.color; ctx.fillRect(0, 0, 1, 1)
    return { foreground: [...ctx.getImageData(0, 0, 1, 1).data], fontSize: parseFloat(css.fontSize),
      points: [rect.left + 1, rect.left + rect.width / 2, rect.right - 2].map(x => [Math.floor(x), Math.max(0, Math.floor(rect.top) - 1)]) }
  })
  const sample = pixels(await page.screenshot({ animations: 'disabled' }))
  const contrasts = style.points.map(([x, y]) => {
    const a = luminance(style.foreground.slice(0, 3)), b = luminance(sample(x, y))
    return { x, y, background: sample(x, y), contrast: (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05) }
  })
  for (const measurement of contrasts) expect(measurement.contrast, `${label} ${JSON.stringify({ ...style, measurement })}`).toBeGreaterThanOrEqual(4.5)
  return { fontSize: style.fontSize, contrasts }
}

test.describe('Mission Control stage', () => {
  test('live stage: artwork, readings, route, opt-in preview, dialogs and Diagnostics', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 1440, height: 1000 })
    const host = await mockHost(page, { stats: live, currentApp: 'control' })
    await page.goto('/#/')
    await stageMode(page, 'live')
    const title = stage(page).locator('.mc-stage-title')
    await expect(title).toHaveText('Control Ultimate Edition')
    // The running app's real cover, framed; it is decorative next to the title it names.
    const art = stage(page).locator('[data-stage-artwork] img')
    await expect(art).toHaveAttribute('src', './api/covers/image?name=Control%20Ultimate%20Edition')
    await expect.poll(() => art.evaluate(img => img.complete && img.naturalWidth)).toBeGreaterThan(0)
    await expect(stage(page).locator('.mc-stage-backdrop')).toHaveCount(1)
    // The title's Steam background fills the stage, and its logo stands in for the title text,
    // which stays the heading.
    await expect(stage(page).locator('[data-stage-hero]')).toHaveAttribute('src', './api/apps/control/artwork/hero')
    await expect(stage(page).locator('[data-stage-hero]')).toHaveClass(/is-loaded/)
    await expect(stage(page).locator('[data-stage-logo]')).toBeVisible()
    await expect(title.locator('span')).toHaveClass(/sr-only/)
    for (const metric of ['Quality', 'Latency', 'FPS', 'Loss', 'Bitrate', 'Encode']) {
      await expect(page.locator(`[data-live-summary-metric="${metric}"]`)).toBeVisible()
    }
    await expect(page.locator('[data-live-summary-metric="Latency"]')).toContainText('round trip')
    await expect(page.locator('[data-live-summary-metric="Loss"]')).toContainText('last 20 s')
    const route = stage(page).locator('[data-mission-route]')
    await expect(route.locator('[data-route-node="host"]')).toContainText('Fixture Host')
    await expect(route.locator('[data-route-node="stream"]')).toContainText('HEVC · 1920×1080')
    await expect(route.locator('[data-route-node="client-0"]')).toContainText('Fixture handheld')
    await expect(route.locator('[data-route-node="client-0"]')).toContainText('11 ms round trip')
    for (const copy of [stage(page).locator('.mc-kicker'), stage(page).locator('.mc-stage-note'), stage(page).locator('.mc-stage-meta')]) {
      const measured = await readableOverArt(page, copy, 'live stage copy')
      expect(measured.fontSize).toBeGreaterThanOrEqual(13)
    }
    await shot(page, testInfo, 'live-desktop-director.png', { deck: true })

    // The preview is opt-in: telemetry ticks never open a capture; Show Display does.
    await page.waitForTimeout(1500)
    expect(host.displayReads()).toEqual([])
    await page.getByRole('button', { name: 'Show Display', exact: true }).click()
    await expect(page.locator('.dashboard-preview-image')).toBeVisible()
    await expect(page.getByRole('button', { name: 'Hide Display', exact: true })).toBeFocused()
    await expect.poll(() => host.displayReads().length).toBeGreaterThan(0)
    await expect(stage(page).locator('.mc-stage-backdrop')).toHaveCount(0)
    // The captured display is shown whole: contained at its own aspect ratio, never blurred or cropped.
    const image = page.locator('.dashboard-preview-image')
    expect(await image.evaluate(node => [getComputedStyle(node).objectFit, getComputedStyle(node).filter])).toEqual(['contain', 'none'])
    const frame = stage(page).locator('[data-stage-preview-frame]')
    const compact = await frame.boundingBox()
    await shot(page, testInfo, 'live-desktop-preview.png', { deck: true })
    await page.getByRole('button', { name: 'Expand Display', exact: true }).click()
    await expect(page.getByRole('button', { name: 'Collapse Display', exact: true })).toHaveAttribute('aria-pressed', 'true')
    await expect.poll(async () => (await frame.boundingBox()).width).toBeGreaterThan(compact.width + 100)
    await shot(page, testInfo, 'live-desktop-preview-expanded.png')
    await page.getByRole('button', { name: 'Collapse Display', exact: true }).click()
    await page.getByRole('button', { name: 'Hide Display', exact: true }).click()
    await expect(page.locator('.dashboard-preview-image')).toHaveCount(0)
    await expect(page.getByRole('button', { name: 'Show Display', exact: true })).toBeFocused()

    // Doctor's guarded fix confirms first; Escape cancels it and returns focus to the button.
    const fix = page.locator('[data-dashboard-doctor]').getByRole('button', { name: 'Fix and verify', exact: true })
    await fix.click()
    const dialog = page.getByRole('dialog')
    await expect(dialog).toBeVisible()
    await expect(dialog.locator('[data-confirm-cancel]')).toBeFocused()
    await page.keyboard.press('Escape')
    await expect(dialog).toHaveCount(0)
    await expect(fix).toBeFocused()

    // Disconnect targets the connected client's paired identity, behind its confirmation.
    const disconnect = route.locator('[data-disconnect-client]')
    await expect(disconnect).toBeVisible()
    await disconnect.click()
    await expect(dialog).toBeVisible()
    await expect(dialog).toContainText('Fixture handheld will be removed from the active stream.')
    await expect(dialog.locator('[data-confirm-cancel]')).toBeFocused()
    await page.keyboard.press('Escape')
    await expect(dialog).toHaveCount(0)
    await expect(disconnect).toBeFocused()

    // Diagnostics: closed by default; Session diagnostics opens it and moves there; updates keep it open.
    const toggle = page.locator('[data-mission-diagnostics-toggle]')
    await expect(toggle).toHaveAttribute('aria-expanded', 'false')
    await page.getByRole('button', { name: 'Session diagnostics', exact: true }).click()
    await expect(toggle).toHaveAttribute('aria-expanded', 'true')
    await expect(toggle).toBeFocused()
    await expect(page.locator('[data-stream-pacing-readout]')).toBeVisible()
    await expect(page.locator('[data-dashboard-frame-health]')).toBeVisible()
    await page.keyboard.press('Tab')
    const focusedAfterTab = await page.evaluate(() => document.activeElement?.outerHTML.slice(0, 80))
    await host.setStats({ ...live, fps: 118.2 })
    await page.waitForTimeout(1200)
    await expect(toggle).toHaveAttribute('aria-expanded', 'true')
    expect(await page.evaluate(() => document.activeElement?.outerHTML.slice(0, 80))).toBe(focusedAfterTab)

    // Sparklines draw at the real width of their slot, and follow it when the layout changes.
    const sparkWidths = () => page.locator('.mc-signal-spark').evaluateAll(slots => slots.map(slot => {
      const canvas = slot.querySelector('canvas')
      return { slot: Math.floor(slot.getBoundingClientRect().width), canvas: canvas ? Math.round(canvas.getBoundingClientRect().width) : 0 }
    }))
    await expect.poll(async () => (await sparkWidths()).filter(({ slot, canvas }) => canvas > 0 && Math.abs(slot - canvas) <= 2).length).toBe(6)
    await page.setViewportSize({ width: 1024, height: 768 })
    await expect.poll(async () => (await sparkWidths()).filter(({ slot, canvas }) => canvas > 0 && Math.abs(slot - canvas) <= 2).length).toBe(6)
    await shot(page, testInfo, 'live-desktop-diagnostics.png', { deck: true })
    expect(host.writes).toEqual([])
  })

  test('idle stage: next title, readiness, attention first, recent rail and history', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 1440, height: 1000 })
    const history = [{ key: 'Fixture handheld', quality_grade: 'A' }, { key: 'Fixture tablet', quality_grade: 'C' }]
    await mockHost(page, { history })
    await page.goto('/#/')
    await stageMode(page, 'idle')
    const hero = page.locator('[data-dashboard-idle-hero]')
    await expect(hero.locator('.mc-kicker')).toHaveText('Next up')
    await expect(hero.locator('.mc-stage-title')).toHaveText('Control Ultimate Edition')
    await expect(hero.locator('[data-idle-signal="paired"]')).toContainText('1')
    await expect(hero.locator('[data-idle-signal="gpu"]')).toContainText('54')
    const launch = hero.locator('[data-stage-launch]')
    await expect(launch).toHaveAccessibleName('Launch Control Ultimate Edition')
    await expect(launch).toHaveClass(/dashboard-action-button-primary/)
    const shelf = page.locator('[data-dashboard-play-rail]')
    await expect(shelf.locator('[data-dashboard-launch-app]')).toHaveCount(4)
    await expect(shelf.locator('[data-dashboard-feature-app]')).toHaveCount(4)
    await expect(shelf.locator('[data-dashboard-feature-app="control"]')).toHaveAttribute('aria-pressed', 'true')
    // One way to the Library, on the shelf; the stage keeps to the title and its launch.
    await expect(hero.getByRole('link', { name: 'Library' })).toHaveCount(0)
    await expect(shelf.getByRole('link', { name: /^Library/ })).toHaveCount(1)
    await expect(page.locator('[data-dashboard-idle-vitals]')).toHaveJSProperty('open', false)
    // Every check passes, so the stage carries no note: the readiness reading already says so.
    await expect(hero.locator('.mc-stage-note')).toHaveCount(0)
    await expect(hero.locator('.mc-stage-backdrop.is-loaded')).toHaveCount(1)
    await readableOverArt(page, hero.locator('.mc-kicker'), 'idle stage kicker')
    await readableOverArt(page, hero.locator('.mc-stage-meta'), 'idle stage meta')
    // The header is the title and its guide; the state is the stage slate's, announced once.
    await expect(page.locator('[data-tab-docs-link]')).toHaveAttribute('href', 'https://papi-ux.com/docs/mission-control/')
    await expect(page.locator('[data-mission-status]')).toHaveText('STANDBY')
    await expect(page.locator('[data-mission-status]')).toHaveClass(/sr-only/)
    const toggle = page.locator('[data-mission-diagnostics-toggle]')
    await toggle.click()
    await expect(page.locator('[data-session-history-host]')).toContainText('Fixture tablet')
    await shot(page, testInfo, 'idle-desktop-director.png', { deck: true })
  })

  test('the shelf features a poster on click and launches only from Play', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 1440, height: 1000 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    const host = await mockHost(page)
    await page.goto('/#/')
    await stageMode(page, 'idle')
    const hero = page.locator('[data-dashboard-idle-hero]')
    const keyart = hero.locator('[data-stage-artwork]')
    await expect(hero.locator('.mc-stage-backdrop.is-loaded')).toHaveCount(1)
    // The key art's place on the stage, independent of how far the page has scrolled.
    const placed = () => keyart.evaluate(node => {
      const art = node.getBoundingClientRect(), on = node.closest('[data-mission-stage]').getBoundingClientRect()
      return { x: art.x - on.x, y: art.y - on.y, width: art.width, height: art.height }
    })
    const before = await placed()
    // Play stays out of the way until its poster is hovered or focused.
    const pick = page.locator('[data-dashboard-feature-app="hades"]')
    const play = page.locator('[data-dashboard-launch-app="hades"]')
    await expect(play).toHaveCSS('opacity', '0')
    await pick.hover()
    await expect(play).toHaveCSS('opacity', '1')
    await page.mouse.move(1, 1)
    await expect(play).toHaveCSS('opacity', '0')
    await pick.focus()
    await expect(play).toHaveCSS('opacity', '1')
    await page.keyboard.press('Tab')
    await expect(play).toBeFocused()
    await expect(play).toHaveCSS('opacity', '1')
    // Featuring swaps the stage, in place, without launching anything.
    await pick.focus()
    await page.keyboard.press('Enter')
    await expect(pick).toHaveAttribute('aria-pressed', 'true')
    await expect(page.locator('[data-dashboard-feature-app="control"]')).toHaveAttribute('aria-pressed', 'false')
    await expect(hero.locator('.mc-stage-title')).toHaveText('Hades II')
    await expect(hero.locator('[data-stage-launch]')).toHaveAccessibleName('Launch Hades II')
    await expect(keyart.locator('img')).toHaveCount(1)
    await expect(keyart.locator('img')).toHaveAttribute('src', './api/covers/image?name=Hades%20II')
    await expect(hero.locator('.mc-stage-backdrop.is-loaded')).toHaveCount(1)
    // No Steam background: the softened cover stands in. A logo too dark for the stage keeps the text.
    await expect(hero.locator('[data-stage-hero]')).toHaveCount(0)
    await expect(hero.locator('[data-stage-logo]')).toHaveCount(0)
    await expect(hero.locator('.mc-stage-title span')).not.toHaveClass(/sr-only/)
    const after = await placed()
    for (const side of ['x', 'y', 'width', 'height']) expect(Math.abs(after[side] - before[side]), `${side} ${JSON.stringify({ before, after })}`).toBeLessThanOrEqual(1)
    // The crossfade animates opacity and the settle transform only, never layout or the blur.
    expect((await hero.locator('.mc-stage-backdrop').evaluate(node => getComputedStyle(node).transitionProperty)).split(/,\s*/).sort()).toEqual(['opacity', 'transform'])
    await readableOverArt(page, hero.locator('.mc-kicker'), 'featured kicker')
    await readableOverArt(page, hero.locator('.mc-stage-meta'), 'featured meta')
    await shot(page, testInfo, 'idle-featured-hades.png')
    // A logo too faint to read keeps the title's name on the stage.
    await page.locator('[data-dashboard-feature-app="long"]').click()
    await expect(hero.locator('.mc-stage-title')).toHaveText(catalog[2].name)
    await expect(hero.locator('[data-stage-logo]')).toHaveCount(0)
    await expect(hero.locator('.mc-stage-title span')).toBeVisible()
    await expect(hero.locator('.mc-stage-title span')).not.toHaveClass(/sr-only/)
    // Without a cover or a background, that title's stage is typographic.
    await expect(hero.locator('[data-stage-artwork]')).toHaveCount(0)
    await expect(hero.locator('.mc-stage-backdrop')).toHaveCount(0)
    // A pale background with a light grey logo: the logo would not hold 3:1 where the scrim is
    // thinnest, so the name stays visible; the missing cover leaves no key art.
    await page.locator('[data-dashboard-feature-app="balatro"]').click()
    await expect(hero.locator('.mc-stage-title')).toHaveText('Balatro')
    await expect(hero.locator('[data-stage-artwork]')).toHaveCount(0)
    // The crossfade briefly holds the leaving and entering layers; wait for it to settle.
    await expect(hero.locator('[data-stage-hero]')).toHaveCount(1)
    await expect(hero.locator('[data-stage-hero]')).toHaveAttribute('src', './api/apps/balatro/artwork/hero')
    await expect(hero.locator('[data-stage-hero]')).toHaveClass(/is-loaded/)
    await expect(hero.locator('[data-stage-logo]')).toHaveCount(0)
    await expect(hero.locator('.mc-stage-title span')).toBeVisible()
    await expect(hero.locator('.mc-stage-title span')).not.toHaveClass(/sr-only/)
    await readableOverArt(page, hero.locator('.mc-kicker'), 'pale background kicker')
    await shot(page, testInfo, 'idle-featured-balatro-pale-midtone.png')
    expect(host.writes).toEqual([])
    // Play is the one shelf control that launches.
    await page.locator('[data-dashboard-feature-app="long"]').hover()
    await page.locator('[data-dashboard-launch-app="long"]').click()
    await expect.poll(() => host.writes).toEqual(['POST /api/apps/launch'])
  })

  test('a tap where Play is hidden features the poster and never launches', async ({ page }, testInfo) => {
    // A fine, hovering primary pointer receiving real touch input, as on a touchscreen laptop.
    // Chromium cannot report any-pointer: coarse beside a fine primary pointer, so this is the case
    // where Play stays hidden until the tap lands. Every measurement is attached to the result.
    await page.setViewportSize({ width: 1440, height: 1000 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    const host = await mockHost(page)
    await page.goto('/#/')
    await stageMode(page, 'idle')
    const evidence = {}
    evidence.media = await page.evaluate(() => Object.fromEntries(['(pointer: fine)', '(pointer: coarse)', '(any-pointer: fine)', '(any-pointer: coarse)', '(hover: hover)', '(hover: none)', '(any-hover: hover)']
      .map(query => [query, matchMedia(query).matches])))
    expect(evidence.media).toMatchObject({ '(pointer: fine)': true, '(hover: hover)': true, '(any-pointer: coarse)': false })
    const pick = page.locator('[data-dashboard-feature-app="hades"]')
    const play = page.locator('[data-dashboard-launch-app="hades"]')
    await pick.scrollIntoViewIfNeeded()
    await page.mouse.move(1, 1)
    const style = () => play.evaluate(node => ({ opacity: getComputedStyle(node).opacity, pointerEvents: getComputedStyle(node).pointerEvents }))
    evidence.hiddenPlay = await style()
    expect(evidence.hiddenPlay).toEqual({ opacity: '0', pointerEvents: 'none' })
    const box = await play.boundingBox()
    evidence.tapPoint = { x: Math.round(box.x + box.width / 2), y: Math.round(box.y + box.height / 2) }
    // Nothing unseen sits under the finger: the poster's feature button takes the hit.
    evidence.hitAtPlayCentre = await page.evaluate(({ x, y }) => {
      const node = document.elementFromPoint(x, y)
      return { feature: node?.getAttribute('data-dashboard-feature-app') || null, launch: node?.getAttribute('data-dashboard-launch-app') || null }
    }, evidence.tapPoint)
    expect(evidence.hitAtPlayCentre).toEqual({ feature: 'hades', launch: null })
    const cdp = await page.context().newCDPSession(page)
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [evidence.tapPoint] })
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] })
    await expect(pick).toHaveAttribute('aria-pressed', 'true')
    await expect(page.locator('[data-dashboard-idle-hero] .mc-stage-title')).toHaveText('Hades II')
    await page.waitForTimeout(500)
    evidence.writesAfterTap = [...host.writes]
    expect(evidence.writesAfterTap).toEqual([])
    // Revealed by the pointer, Play is the explicit control that launches.
    await pick.hover()
    await expect(play).toHaveCSS('opacity', '1')
    evidence.revealedPlay = await style()
    expect(evidence.revealedPlay).toEqual({ opacity: '1', pointerEvents: 'auto' })
    await page.screenshot({ path: testInfo.outputPath('shelf-play-revealed-desktop.png'), animations: 'disabled' })
    await play.click()
    await expect.poll(() => host.writes).toEqual(['POST /api/apps/launch'])
    evidence.writesAfterPlayClick = [...host.writes]
    writeFileSync(testInfo.outputPath('hybrid-touch-evidence.json'), JSON.stringify(evidence, null, 2))
    await testInfo.attach('hybrid-touch-evidence', { path: testInfo.outputPath('hybrid-touch-evidence.json'), contentType: 'application/json' })
  })

  test('a weak logo keeps the name on a medium-width stage', async ({ page }, testInfo) => {
    // A 40 to 52rem stage is where the copy column reaches furthest into the thinner scrim.
    await page.setViewportSize({ width: 1024, height: 768 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    const host = await mockHost(page)
    await page.goto('/#/')
    await stageMode(page, 'idle')
    const hero = page.locator('[data-dashboard-idle-hero]')
    const width = await hero.evaluate(node => node.getBoundingClientRect().width / parseFloat(getComputedStyle(document.documentElement).fontSize))
    expect(width, 'stage width in rem').toBeGreaterThanOrEqual(40)
    expect(width, 'stage width in rem').toBeLessThan(52)
    await page.locator('[data-dashboard-feature-app="balatro"]').click()
    await expect(hero.locator('.mc-stage-title')).toHaveText('Balatro')
    // The crossfade briefly holds the leaving and entering layers; wait for it to settle.
    await expect(hero.locator('[data-stage-hero]')).toHaveCount(1)
    await expect(hero.locator('[data-stage-hero]')).toHaveAttribute('src', './api/apps/balatro/artwork/hero')
    await expect(hero.locator('[data-stage-hero]')).toHaveClass(/is-loaded/)
    await expect(hero.locator('[data-stage-logo]')).toHaveCount(0)
    await expect(hero.locator('.mc-stage-title span')).toBeVisible()
    await expect(hero.locator('.mc-stage-title span')).not.toHaveClass(/sr-only/)
    // Control's light logo still stands in for its name at this width.
    await page.locator('[data-dashboard-feature-app="control"]').click()
    await expect(hero.locator('[data-stage-logo]')).toBeVisible()
    await shot(page, testInfo, 'idle-medium-stage-control-logo.png')
    expect(host.writes).toEqual([])
  })

  test('inside Nova\'s Host Console: Nova\'s theme for the visit, no guide link, and Play goes to Nova', async ({ page }, testInfo) => {
    // Nova's panel is about 520 CSS pixels wide; its WebView opens the console with ?client=nova.
    await page.setViewportSize({ width: 520, height: 900 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    const host = await mockHost(page, { theme: 'miami' })
    // Chromium has no art:// handler; the navigation Nova's WebView would take still shows as a request.
    const handedToNova = []
    page.on('request', request => { if (request.url().startsWith('art://')) handedToNova.push(request.url()) })
    await page.goto('/?client=nova&theme=director#/')
    await stageMode(page, 'idle')
    expect(await page.evaluate(() => [document.documentElement.getAttribute('data-theme'), localStorage.getItem('polaris-theme')])).toEqual(['director', 'miami'])
    await expect(page.locator('[data-tab-docs-link]')).toHaveCount(0)
    await shot(page, testInfo, 'nova-host-console-idle.png')
    const play = page.locator('[data-dashboard-launch-app="hades"]')
    await play.focus()
    await page.keyboard.press('Enter')
    await expect.poll(() => handedToNova).toEqual(['art://launch?host_uuid=fixture&host_name=Fixture%20Host&app_uuid=hades&app_name=Hades%20II'])
    expect(host.writes).toEqual([])
    // Another page of the same visit keeps Nova's theme and hides its guide link too.
    await page.goto('/#/apps')
    await expect(page.locator('[data-library-app]').first()).toBeVisible()
    expect(await page.evaluate(() => document.documentElement.getAttribute('data-theme'))).toBe('director')
    await expect(page.locator('[data-tab-docs-link]')).toHaveCount(0)
    expect(host.writes).toEqual([])
  })

  test('settings keeps each tab guide in a browser and hides it throughout a Nova visit', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 900 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    const host = await mockHost(page, { theme: 'miami' })
    const tabs = [
      ['General', 'general-tab'], ['Input', 'input-tab'], ['Network', 'network-tab'],
      ['Config Files', 'files'], ['Advanced', 'advanced-tab'],
    ]
    const selectTab = async name => {
      await page.locator('.settings-nav-button').filter({ hasText: new RegExp(`^${name}$`) }).click()
      await expect(page.locator('.settings-tab-title')).toHaveText(name)
    }
    await page.goto('/#/config')
    for (const [name, anchor] of tabs) {
      await selectTab(name)
      await expect(page.locator(`.config-page [data-tab-docs-link] a[href$="#${anchor}"]`)).toBeVisible()
    }
    // Nova's presentation hint hides only page-level guides, leaving the settings usable.
    await page.goto('/?client=nova&theme=director#/config')
    for (const [name] of tabs) {
      await selectTab(name)
      await expect(page.locator('[data-tab-docs-link]')).toHaveCount(0)
      await expect(page.locator('.config-page').first()).toBeVisible()
      expect(await page.locator('.config-page input, .config-page select, .config-page textarea').count()).toBeGreaterThan(0)
    }
    expect(await page.evaluate(() => [document.documentElement.getAttribute('data-theme'), localStorage.getItem('polaris-theme')])).toEqual(['director', 'miami'])
    expect(host.writes).toEqual([])
  })

  test('inside Nova, a theme picked in the console survives a reload of the address it is on', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 900 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    // The browser's saved theme is OLED; Nova asks for Director for this visit.
    const host = await mockHost(page, { theme: 'oled' })
    await page.goto('/?client=nova&theme=director&lang=en#/')
    await stageMode(page, 'idle')
    const theme = () => page.evaluate(() => document.documentElement.getAttribute('data-theme'))
    expect(await theme()).toBe('director')
    // Once the visit holds Nova's word, the address no longer carries it; the rest stays.
    const address = new URL(page.url())
    expect([address.searchParams.get('client'), address.searchParams.get('theme'), address.searchParams.get('lang'), address.hash]).toEqual([null, null, 'en', '#/'])
    await page.reload()
    await stageMode(page, 'idle')
    expect(await theme()).toBe('director')
    await expect(page.locator('[data-tab-docs-link]')).toHaveCount(0)
    // The player picks a theme in the console; a reload keeps it, and keeps Nova's mode.
    await page.getByRole('button', { name: 'Choose theme' }).click()
    await page.getByRole('option', { name: /OLED/ }).click()
    expect(await theme()).toBe('oled')
    await page.reload()
    await stageMode(page, 'idle')
    expect(await theme()).toBe('oled')
    await expect(page.locator('[data-tab-docs-link]')).toHaveCount(0)
    expect(host.writes).toEqual([])
  })

  test('Spaces appears only when it needs attention or a Space is running', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 1280, height: 900 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    const host = await mockHost(page, { spaces: spacesState() })
    await page.goto('/#/')
    await stageMode(page, 'idle')
    await expect(page.locator('[data-dashboard-spaces]')).toHaveCount(0)
    host.spaces = spacesState({ failed: true })
    await page.reload()
    await stageMode(page, 'idle')
    await expect(page.locator('[data-spaces-attention]')).toHaveText('Needs attention')
    await shot(page, testInfo, 'spaces-attention-idle.png')
    host.spaces = spacesState({ activity: [{ profile_id: 'living', client_id: 'device', state: 'running' }] })
    await page.reload()
    await expect(page.locator('[data-space-session]')).toContainText('Living room')
    await expect(page.locator('[data-space-session]')).toContainText('Fixture handheld')
    await expect(page.locator('[data-mission-stage]')).toHaveCount(0)
    await shot(page, testInfo, 'spaces-active.png', { deck: true })
    expect(host.writes).toEqual([])
  })

  test('idle stage puts a launch check that needs attention before Launch', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 1280, height: 900 })
    await mockHost(page, { certs: [] })
    await page.goto('/#/')
    await stageMode(page, 'idle')
    const actions = page.locator('[data-dashboard-idle-hero] .mc-stage-actions')
    const fix = actions.locator('[data-stage-priority-fix]')
    await expect(fix).toHaveText('Open priority fix')
    await expect(fix).toHaveClass(/dashboard-action-button-primary/)
    await expect(actions.locator('[data-stage-launch]')).toHaveClass(/dashboard-action-button-ghost/)
    await expect(page.locator('[data-dashboard-idle-hero] .mc-stage-note')).toContainText('need attention')
    await page.locator('[data-dashboard-idle-hero]').screenshot({ path: testInfo.outputPath('idle-attention.png'), animations: 'disabled' })
  })

  test('a stats channel that drops reads as unavailable, blanks the readings and recovers', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 1440, height: 1000 })
    const host = await mockHost(page, { stats: live, currentApp: 'control' })
    await page.goto('/#/')
    await stageMode(page, 'live')
    host.streamDown = true
    await page.evaluate(() => window.__dropStats())
    await stageMode(page, 'unavailable')
    const unavailable = stage(page)
    await expect(unavailable.locator('.mc-stage-title')).toHaveText('Waiting for host')
    await expect(unavailable).toContainText(/Last report \d+ s ago/)
    await expect(unavailable).toContainText('Retrying automatically')
    await expect(page.locator('[data-live-summary-metric]')).toHaveCount(0)
    await expect(page.locator('[data-unavailable-signal] .mc-signal-number')).toHaveText(['—', '—', '—', '—', '—', '—'])
    await expect(page.getByRole('button', { name: 'Show Display', exact: true })).toHaveCount(0)
    await expect(unavailable.getByRole('link', { name: 'Doctor & Support', exact: true })).toBeVisible()
    await expect(page.locator('[data-dashboard-doctor-waiting]')).toBeVisible()
    await shot(page, testInfo, 'unavailable-desktop-director.png', { deck: true })
    host.streamDown = false
    await page.evaluate(() => { window.__holdStats = false })
    await stageMode(page, 'live')
    expect(host.writes).toEqual([])
  })

  test('a host that has not answered loads, then says so without inventing a report age', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 1440, height: 1000 })
    const host = await mockHost(page, { stats: live })
    host.streamDown = true
    await page.addInitScript(() => { window.__holdStats = true })
    await page.goto('/#/')
    await stageMode(page, 'loading')
    await expect(stage(page)).toHaveAttribute('aria-busy', 'true')
    await page.screenshot({ path: testInfo.outputPath('loading-desktop-director.png'), animations: 'disabled' })
    await page.evaluate(() => window.__dropStats())
    await stageMode(page, 'unavailable')
    await expect(stage(page)).toContainText('No report received yet')
    await expect(stage(page)).not.toContainText('Last report')
  })

  test('reduced motion draws no sparklines and keeps the live marker still', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 900 })
    await page.emulateMedia({ reducedMotion: 'reduce' })
    await mockHost(page, { stats: live, currentApp: 'control' })
    await page.goto('/#/')
    await stageMode(page, 'live')
    await page.waitForTimeout(1000)
    await expect(page.locator('.mc-signal-spark')).toHaveCount(0)
    await expect(page.locator('.mc-signals canvas')).toHaveCount(0)
    expect(await page.locator('.mc-marker.is-live').evaluate(node => getComputedStyle(node).animationName)).toBe('none')
    await page.locator('[data-mission-diagnostics-toggle]').click()
    await expect(page.getByText('Live charts are paused while reduced motion is enabled.')).toBeVisible()
  })

  for (const theme of themes) {
    test(`every theme keeps its own palette and a readable stage in ${theme}`, async ({ page }, testInfo) => {
      await page.setViewportSize({ width: 1280, height: 900 })
      await page.emulateMedia({ reducedMotion: 'reduce' })
      const host = await mockHost(page, { theme, stats: live, currentApp: 'hades' })
      await page.goto('/#/')
      await stageMode(page, 'live')
      if (theme === 'polaris') expect(await page.evaluate(() => document.documentElement.getAttribute('data-theme'))).toBeNull()
      else expect(await page.evaluate(() => document.documentElement.getAttribute('data-theme'))).toBe(theme)
      const greeting = page.locator('[data-director-greeting]')
      if (theme === 'director') await expect(greeting).toHaveText('< Congratulations, Director >')
      else await expect(greeting).toHaveCount(0)
      await readableOverArt(page, stage(page).locator('.mc-stage-note'), `${theme} live note`)
      await readableOverArt(page, page.locator('[data-live-summary-metric="Latency"] .mc-signal-sub'), `${theme} strip sub`)
      await page.screenshot({ path: testInfo.outputPath(`theme-live-${theme}.png`), animations: 'disabled' })
      await host.setStats(idle)
      await stageMode(page, 'idle')
      const hero = page.locator('[data-dashboard-idle-hero]')
      await expect(hero.locator('.mc-stage-backdrop.is-loaded')).toHaveCount(1)
      // Control's light logo passes the logo screen against this theme's scrim and brightest backdrop.
      await expect(hero.locator('[data-stage-logo]')).toBeVisible()
      await readableOverArt(page, hero.locator('.mc-kicker'), `${theme} idle kicker`)
      await readableOverArt(page, hero.locator('.mc-stage-meta'), `${theme} idle meta`)
      await page.screenshot({ path: testInfo.outputPath(`theme-idle-${theme}.png`), animations: 'disabled' })
      // The palest cover, featured from the shelf, still leaves the copy readable.
      await page.locator('[data-dashboard-feature-app="hades"]').click()
      await expect(hero.locator('.mc-stage-title')).toHaveText('Hades II')
      await expect(hero.locator('.mc-stage-backdrop.is-loaded')).toHaveCount(1)
      await readableOverArt(page, hero.locator('.mc-kicker'), `${theme} featured kicker`)
      await readableOverArt(page, hero.locator('.mc-stage-meta'), `${theme} featured meta`)
      await page.screenshot({ path: testInfo.outputPath(`theme-idle-featured-${theme}.png`), animations: 'disabled' })
    })
  }

  for (const width of [360, 390, 768, 1280]) {
    test(`Director's greeting grows with enlarged text and keeps its words whole at ${width}px`, async ({ page }) => {
      await page.setViewportSize({ width, height: 900 })
      await mockHost(page, { stats: idle })
      await page.goto('/#/')
      await stageMode(page, 'idle')
      const normal = await greetingWhole(page, `${width} normal`)
      // The fixed 13 px console subtitle this replaces would not have grown at all.
      expect(normal).toBeGreaterThanOrEqual(16 * 1.15 * 0.999)
      await page.evaluate(() => { document.documentElement.style.fontSize = '32px' })
      const enlarged = await greetingWhole(page, `${width} enlarged`)
      expect(enlarged / normal, `${width} ${normal} -> ${enlarged}`).toBeGreaterThanOrEqual(1.5)
    })
  }

  test('reflows at 320 CSS pixels without hiding the main actions', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 320, height: 640 })
    const host = await mockHost(page, { stats: live, currentApp: 'control' })
    await page.goto('/#/')
    await stageMode(page, 'live')
    await noSidewaysPage(page, '320 live')
    await reachable(page.locator('[data-start-preview]'), '320 live Show Display')
    await reachable(page.locator('[data-disconnect-client]'), '320 live Disconnect')
    await page.locator('[data-mission-diagnostics-toggle]').click()
    await noSidewaysPage(page, '320 live diagnostics')
    await shot(page, testInfo, 'reflow-320-live.png', { deck: true })
    await host.setStats(idle)
    await stageMode(page, 'idle')
    await noSidewaysPage(page, '320 idle')
    await reachable(page.locator('[data-stage-launch]'), '320 idle Launch')
    // A fine pointer that is not over the poster leaves Play hidden and taking no hits, so keyboard
    // focus reveals it before its hit test; where the mouse happens to rest must not matter.
    const firstPlay = page.locator('[data-dashboard-launch-app]').first()
    await firstPlay.focus()
    await expect(firstPlay).toHaveCSS('opacity', '1')
    await reachable(firstPlay, '320 idle rail launch')
    await shot(page, testInfo, 'reflow-320-idle.png', { deck: true })
  })
})

const viewports = [[360, 800], [800, 360], [390, 844], [844, 390], [768, 1024], [1024, 768]]
test.describe('Mission Control on phones and tablets', () => {
  test.use({ hasTouch: true, isMobile: true })
  for (const [width, height] of viewports) {
    for (const text of ['normal', 'root-200', 'browser-200']) {
      test(`stage states, targets and a rotated dialog at ${width}x${height} with ${text} text`, async ({ page }, testInfo) => {
        test.setTimeout(90000)
        const label = `${width}x${height} ${text}`
        await page.setViewportSize({ width, height })
        await page.emulateMedia({ reducedMotion: 'reduce' })
        if (text === 'browser-200') await (await page.context().newCDPSession(page)).send('Page.setFontSizes', { fontSizes: { standard: 32 } })
        if (text === 'root-200') await page.addInitScript(() => document.addEventListener('DOMContentLoaded', () => { document.documentElement.style.fontSize = '32px' }))
        const host = await mockHost(page, { stats: idle })
        await page.goto('/#/')
        await stageMode(page, 'idle')
        expect(await page.evaluate(() => getComputedStyle(document.documentElement).fontSize)).toBe(text === 'normal' ? '16px' : '32px')
        const measured = {}
        // Enlarged text really enlarges the stage title; it is not held back by a width cap.
        measured.idleTitle = await page.locator('.mc-stage-title').evaluate(node => parseFloat(getComputedStyle(node).fontSize))
        if (text !== 'normal') expect(measured.idleTitle, label).toBeGreaterThanOrEqual(1.5 * 32 * 0.999)
        // Director's greeting keeps every word whole on one line and inside the page.
        measured.greeting = await greetingWhole(page, label)
        const toggle = page.locator('[data-mission-diagnostics-toggle]')
        await toggle.click()
        await expect(toggle).toHaveAttribute('aria-expanded', 'true')
        await noSidewaysPage(page, `${label} idle`)
        await reachable(page.locator('[data-stage-launch]'), `${label} idle Launch`)
        await reachable(page.locator('[data-dashboard-launch-app]').first(), `${label} idle rail launch`)
        // Touch has no hover, so Play is always shown and takes the tap.
        await expect(page.locator('[data-dashboard-launch-app]').first()).toHaveCSS('opacity', '1')
        await expect(page.locator('[data-dashboard-launch-app]').first()).toHaveCSS('pointer-events', 'auto')
        await page.locator('[data-dashboard-play-rail]').scrollIntoViewIfNeeded()
        await page.screenshot({ path: testInfo.outputPath(`shelf-${width}x${height}-${text}.png`), animations: 'disabled' })
        await touchTargets(page, `${label} idle`)
        await shot(page, testInfo, `idle-${width}x${height}-${text}.png`, { deck: true })

        await host.setStats(live)
        await stageMode(page, 'live')
        // The viewer's Diagnostics choice survives the change of stage.
        await expect(toggle).toHaveAttribute('aria-expanded', 'true')
        await noSidewaysPage(page, `${label} live`)
        await reachable(page.locator('[data-start-preview]'), `${label} live Show Display`)
        await touchTargets(page, `${label} live`)
        await shot(page, testInfo, `live-${width}x${height}-${text}.png`, { deck: true })

        // A dialog opened in one orientation stays usable in the other, and Escape returns focus.
        const fix = page.locator('[data-dashboard-doctor]').getByRole('button', { name: 'Fix and verify', exact: true })
        await fix.scrollIntoViewIfNeeded()
        await fix.tap()
        const dialog = page.getByRole('dialog')
        await expect(dialog).toBeVisible()
        await page.setViewportSize({ width: height, height: width })
        await page.waitForTimeout(250)
        await reachable(dialog.locator('[data-confirm-cancel]'), `${label} rotated dialog cancel`)
        await dialog.locator('[data-confirm-confirm]').scrollIntoViewIfNeeded()
        await withinViewport(dialog.locator('[data-confirm-confirm]'), `${label} rotated dialog confirm`)
        await noSidewaysPage(page, `${label} rotated live`)
        await page.keyboard.press('Escape')
        await expect(dialog).toHaveCount(0)
        await expect(fix).toBeFocused()
        await page.setViewportSize({ width, height })

        host.streamDown = true
        await page.evaluate(() => window.__dropStats())
        await stageMode(page, 'unavailable')
        await noSidewaysPage(page, `${label} unavailable`)
        await reachable(stage(page).getByRole('link', { name: 'Doctor & Support', exact: true }), `${label} unavailable troubleshooting`)
        await touchTargets(page, `${label} unavailable`)
        await shot(page, testInfo, `unavailable-${width}x${height}-${text}.png`, { deck: true })
        expect(host.writes).toEqual([])
        await testInfo.attach('mission-measurements', { body: JSON.stringify(measured), contentType: 'application/json' })
      })
    }
  }
})
