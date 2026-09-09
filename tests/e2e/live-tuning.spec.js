import { test, expect } from '@playwright/test'
import { readFileSync } from 'node:fs'

const cases = JSON.parse(readFileSync(new URL('../fixtures/live-tuning-v1.json', import.meta.url), 'utf8'))
for (const width of [390, 1280]) {
  test(`Live Tuning saves confirmed state and remains readable at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    let live = { ...cases[0].live_tuning, host_instance: 'e2e-host' }
    await page.addInitScript((initial) => {
      window.__liveSources = []
      window.EventSource = class {
        constructor() {
          window.__liveSources.push(this)
          setTimeout(() => { this.onopen?.({}); this.onmessage?.({ data: JSON.stringify({ streaming: false, live_tuning: initial }) }) }, 25)
        }
        close() { window.__liveSources = window.__liveSources.filter(source => source !== this) }
      }
    }, live)
    await page.route('**/api/**', async route => {
      const path = new URL(route.request().url()).pathname
      if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
      if (path === '/api/config') return route.fulfill({ json: { status: true, platform: 'linux', configuration_revision: live.configuration_revision, live_tuning: live } })
      if (path === '/api/live-tuning' && route.request().method() === 'POST') {
        expect(route.request().postDataJSON()).toEqual({ enabled: true })
        expect(route.request().headers()['if-match']).toBe(`"${live.configuration_revision}"`)
        live = { ...live, enabled: true, state: 'waiting', supported: false, applied_bitrate_kbps: 0, quality_limit_kbps: 0, sequence: 2 }
        return route.fulfill({ json: { status: true, live_tuning: live } })
      }
      if (path === '/api/stats') return route.fulfill({ json: { streaming: false, live_tuning: live } })
      if (path === '/api/apps') return route.fulfill({ json: { status: true, apps: [] } })
      if (path === '/api/clients/list') return route.fulfill({ json: { status: true, named_certs: [] } })
      return route.fulfill({ json: { status: true } })
    })
    await page.goto('/#/')
    const control = page.getByRole('switch', { name: 'Live Tuning', exact: true }).first()
    await expect(control).toBeEnabled()
    await expect(control).toHaveAttribute('aria-checked', 'false')
    await control.click()
    await expect(control).toHaveAttribute('aria-checked', 'true')
    const panel = control.locator('..')
    await expect(panel).toContainText('waiting for a stream')
    const box = await panel.boundingBox()
    expect(box.x).toBeGreaterThanOrEqual(0)
    expect(box.x + box.width).toBeLessThanOrEqual(width)
    expect(await panel.evaluate(node => node.scrollWidth <= node.clientWidth + 1)).toBe(true)
    await panel.screenshot({ path: `test-results/live-tuning-${width}.png` })
  })
}
