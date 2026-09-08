import { test, expect } from '@playwright/test'

const viewports = [
  { width: 320, height: 900 },
  { width: 768, height: 1024 },
  { width: 1207, height: 1443 },
  { width: 1280, height: 900 },
  { width: 1440, height: 900 },
  { width: 1920, height: 1080 },
]

test.beforeEach(async ({ page }) => {
  let credentialsSaved = false
  // Exercise the real wizard without reading or changing a host's credentials.
  await page.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/password') {
      credentialsSaved = true
      return route.fulfill({ json: { status: true } })
    }
    if (path === '/api/config') {
      if (!credentialsSaved) {
        return route.fulfill({ status: 302, headers: { location: '/welcome' } })
      }
      return route.fulfill({ json: { status: true, platform: 'linux', encoder: 'Auto-detected' } })
    }
    return route.fulfill({ status: 404 })
  })
})

async function expectReadableLayout(page, viewport) {
  const panel = page.locator('section.glass')
  const logo = await panel.getByRole('img', { name: 'Polaris', exact: true }).boundingBox()
  const heading = await panel.getByRole('heading', { level: 1 }).boundingBox()
  expect(logo.width, 'wordmark should leave room for the welcome heading').toBeLessThanOrEqual(240)
  expect(logo.y + logo.height, 'wordmark should sit above the heading').toBeLessThanOrEqual(heading.y)

  const panelBox = await panel.boundingBox()
  const forward = panel.getByRole('button', { name: /^(Next|Finish Setup)$/ })
  const forwardBox = await forward.boundingBox()
  expect(panelBox.y + panelBox.height - forwardBox.y - forwardBox.height,
    'main panel should end after navigation instead of stretching to the sidebar').toBeLessThanOrEqual(34)

  for (const title of ['Resources', 'Legal']) {
    const card = page.locator('section').filter({ has: page.getByRole('heading', { name: title, exact: true }) })
    const copy = await card.locator('p').boundingBox()
    const links = card.getByRole('link')
    for (const link of await links.all()) {
      const box = await link.boundingBox()
      expect(box.y, `${title} links should leave the description its own row`).toBeGreaterThanOrEqual(copy.y + copy.height + 12)
    }
  }

  // Check actual rendered boxes; overflow-hidden on the page can hide broken sizing.
  const clipped = await page.locator('section input, section a, section button, section img, section h1, section h2, section p').evaluateAll((elements, width) => {
    return elements.filter((element) => {
      const box = element.getBoundingClientRect()
      // A filled input scrolls its value horizontally by design; its box must still fit.
      const textClipped = element.tagName !== 'INPUT' && (
        element.scrollWidth > element.clientWidth + 1
        || element.scrollHeight > element.clientHeight + 1)
      return box.width > 0 && (box.left < -1 || box.right > width + 1
        || textClipped)
    }).map((element) => element.textContent.trim() || element.tagName)
  }, viewport.width)
  expect(clipped, 'text and controls should stay readable within the viewport').toEqual([])
}

for (const viewport of viewports) {
  test(`welcome stays readable at ${viewport.width}px`, async ({ page }, testInfo) => {
    await page.setViewportSize(viewport)
    await page.goto('/#/welcome')
    await page.getByLabel('Password', { exact: true }).fill('layout-test-password')
    await page.getByLabel('Confirm password', { exact: true }).fill('layout-test-password')
    await page.getByRole('button', { name: 'Save Credentials', exact: true }).click()
    const next = page.getByRole('button', { name: 'Next', exact: true })
    await expect(next).toBeEnabled()
    for (let step = 0; step < 3; step++) await next.click()
    await expect(page.getByRole('heading', { name: 'First App', exact: true })).toBeVisible()
    await page.evaluate(() => document.fonts.ready)
    await page.screenshot({ path: testInfo.outputPath('first-app.png'), fullPage: true })
    await expectReadableLayout(page, viewport)

    await next.click()
    await expect(page.getByRole('button', { name: 'Finish Setup', exact: true })).toBeVisible()
    await expectReadableLayout(page, viewport)
    // Cover every step, including the longer credentials and network panels.
    for (let step = 3; step >= 0; step--) {
      await page.getByRole('button', { name: 'Back', exact: true }).click()
      await expectReadableLayout(page, viewport)
    }
  })
}
