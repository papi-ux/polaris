import { test, expect } from './fixtures/mocked.js'

async function mockHost(page, { activeSpace = false } = {}) {
  await page.addInitScript(() => {
    window.__pads = []
    window.__pulses = 0
    Object.defineProperty(navigator, 'getGamepads', { configurable: true, value: () => window.__pads })
    window.EventSource = class {
      constructor() {
        this.timer = setInterval(() => {
          this.onopen?.({})
          this.onmessage?.({ data: JSON.stringify({ streaming: false }) })
        }, 250)
      }
      close() { clearInterval(this.timer) }
    }
  })
  await page.route('**/api/**', async route => {
    const request = route.request(), path = new URL(request.url()).pathname
    // A UI-only smoke test has no reason to mutate even its mock host.
    expect(request.method()).toBe('GET')
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/config') return route.fulfill({ json: { status: true, platform: 'linux',
      version: '1.4.14-beta.2', configuration_revision: 'a'.repeat(64), sunshine_name: 'Host',
      linux_stream_mode: 'desktop_display', headless_mode: 'disabled',
      encoder_codec_support: { ready: true, encoder: 'nvenc', hevc_supported: true } } })
    if (path === '/api/settings/metadata') return route.fulfill({ status: 404, json: { status: false } })
    if (path === '/api/multiseat/profiles') return route.fulfill({ json: { enabled: activeSpace,
      available: true, changing: false, failed: false,
      profiles: activeSpace ? [{ id: 'a', name: 'Gaming Space', clients: ['device'], family: 'steam' }] : [],
      activity: activeSpace ? [{ profile_id: 'a', client_id: 'device', state: 'running' }] : [] } })
    if (path === '/api/clients/list') return route.fulfill({ json: { status: true, named_certs: [{ uuid: 'device', name: 'Handheld' }] } })
    if (path === '/api/apps') return route.fulfill({ json: { status: true, apps: [], current_app: '' } })
    if (path === '/api/logs') return route.fulfill({ body: '' })
    if (path === '/api/ai/history' || path === '/api/ai/cache') return route.fulfill({ json: [] })
    return route.fulfill({ json: { status: true, streaming: false } })
  })
}

async function fits(locator, width) {
  const box = await locator.boundingBox()
  expect(box).not.toBeNull()
  expect(box.x).toBeGreaterThanOrEqual(0)
  expect(box.x + box.width).toBeLessThanOrEqual(width + 1)
  expect(await locator.evaluate(node => node.scrollWidth <= node.clientWidth + 1)).toBe(true)
}

for (const width of [390, 1280]) {
  test(`settings search keyboard and accessible name at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    await mockHost(page)
    await page.goto('/#/config')
    const search = page.getByRole('searchbox', { name: 'Search settings', exact: true })
    await expect(search).toHaveAttribute('enterkeyhint', 'search')
    await search.fill('Server Name')
    await search.press('Enter')
    await expect(page.locator('#sunshine_name')).toBeFocused()
    await search.fill('bitrate')
    await search.press('Escape')
    await expect(search).toHaveValue('')
    await expect(search).toBeFocused()
    await search.fill('zzzz-no-such-setting')
    await search.press('Enter')
    await expect(search).toBeFocused()
    await search.fill('')
    await fits(page.locator('.settings-command-bar'), width)
    await page.locator('.settings-command-bar').screenshot({ path: `test-results/polaris-search-keyboard-${width}.png` })
  })

  test(`late controller detection and disabled rumble reasons at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    await mockHost(page)
    await page.goto('/#/troubleshooting')
    const rumble = page.getByRole('button', { name: 'Test rumble', exact: true })
    const hint = page.locator('[data-controller-rumble-hint]')
    await expect(rumble).toBeDisabled()
    await expect(rumble).toHaveAccessibleDescription(/Connect a controller to this browser/)
    await page.evaluate(() => {
      // No event: Detect itself must refresh the browser list after page load.
      window.__pads = [{ index: 0, id: 'Browser Test Pad', buttons: [{}, {}], axes: [0, 0],
        vibrationActuator: { playEffect: async () => { window.__pulses += 1; return 'complete' } } }]
    })
    await page.getByRole('button', { name: 'Detect controller', exact: true }).click()
    await expect(rumble).toBeEnabled()
    await expect(hint).toHaveCount(0)
    await rumble.click()
    await expect.poll(() => page.evaluate(() => window.__pulses)).toBe(1)
    await page.evaluate(() => {
      window.__pads = [{ index: 0, id: 'Unsupported Browser Pad', buttons: [{}], axes: [] }]
      window.dispatchEvent(new Event('gamepaddisconnected'))
    })
    await expect(rumble).toBeDisabled()
    await page.getByRole('button', { name: 'Detect controller', exact: true }).click()
    await expect(rumble).toHaveAccessibleDescription(/This browser cannot vibrate/)
    await fits(hint, width)
    await hint.locator('..').screenshot({ path: `test-results/polaris-controller-reason-${width}.png` })
    await page.evaluate(() => { window.__pads = []; window.dispatchEvent(new Event('gamepaddisconnected')) })
    await expect(rumble).toHaveAccessibleDescription(/Connect a controller to this browser/)
  })

  test(`blocked display preview exposes its existing reason at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    await mockHost(page, { activeSpace: true })
    await page.goto('/#/')
    const preview = page.getByRole('button', { name: 'Show display', exact: true })
    await expect(preview).toBeDisabled()
    await expect(preview).toHaveAccessibleDescription(/Desktop preview is paused while a Space is active/)
    const reason = page.locator('[data-preview-hidden-reason]')
    await fits(reason, width)
    await reason.locator('..').screenshot({ path: `test-results/polaris-preview-reason-${width}.png` })
    await expect(page.locator('.dashboard-preview-image')).toHaveCount(0)
  })
}
