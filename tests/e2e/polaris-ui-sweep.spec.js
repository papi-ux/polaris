import fs from 'node:fs/promises'
import path from 'node:path'
import { test, expect } from './fixtures/auth.js'
import { POLARIS_VIEWPORTS } from './polaris-viewports.js'

const screenshotDir = process.env.POLARIS_UI_SWEEP_SCREENSHOT_DIR || path.resolve('.hermes/artifacts/polaris-ui-ux-sweep/screenshots')

const routes = [
  { name: 'mission-control', path: '/#/', heading: /Mission Control/i },
  { name: 'library', path: '/#/apps', heading: /Library/i },
  { name: 'settings', path: '/#/config', heading: /Settings/i },
  { name: 'pairing', path: '/#/pin', heading: /Pair/i },
  { name: 'browser-stream', path: '/#/browser-stream', heading: /Browser Stream/i },
  { name: 'troubleshooting', path: '/#/troubleshooting', heading: /Troubleshooting/i },
]

async function gotoRoute(page, route) {
  const targetHash = new URL(route.path, 'https://polaris.local').hash || '#/'

  for (let attempt = 0; attempt < 2; attempt++) {
    await page.evaluate((hash) => {
      window.location.hash = hash
    }, targetHash)
    await page.waitForFunction((hash) => window.location.hash === hash, targetHash, { timeout: 5000 })
    const heading = page.getByRole('heading', { name: route.heading }).first()

    try {
      await expect(heading).toBeVisible({ timeout: 15000 })
      return
    } catch (error) {
      if (attempt === 0) {
        await page.reload({ waitUntil: 'domcontentloaded' })
        continue
      }
      throw error
    }
  }
}

test.describe('Polaris UI screenshot and overflow sweep', () => {
  test.setTimeout(60000)

  test.beforeAll(async () => {
    await fs.mkdir(screenshotDir, { recursive: true })
  })

  for (const viewport of POLARIS_VIEWPORTS) {
    for (const route of routes) {
      test(`${viewport.name} ${route.name} captures screenshot without horizontal overflow`, async ({ loggedInPage }) => {
        await loggedInPage.setViewportSize({ width: viewport.width, height: viewport.height })
        await gotoRoute(loggedInPage, route)

        const dimensions = await loggedInPage.evaluate(() => {
          const documentWidth = Math.max(
            document.documentElement.scrollWidth,
            document.body?.scrollWidth || 0,
          )
          const viewportWidth = window.innerWidth
          return {
            documentWidth,
            viewportWidth,
            overflow: documentWidth - viewportWidth,
          }
        })

        expect(dimensions.overflow, `${route.name} overflows ${viewport.name}: ${dimensions.documentWidth}px document vs ${dimensions.viewportWidth}px viewport`).toBeLessThanOrEqual(1)

        await loggedInPage.screenshot({
          path: path.join(screenshotDir, `${viewport.name}-${route.name}.png`),
          fullPage: true,
        })
      })
    }
  }
})
