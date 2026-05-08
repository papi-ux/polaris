import { chromium, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const args = new Map()
for (let i = 2; i < process.argv.length; i += 1) {
  const arg = process.argv[i]
  if (arg.startsWith('--') && process.argv[i + 1] && !process.argv[i + 1].startsWith('--')) {
    args.set(arg, process.argv[i + 1])
    i += 1
  } else {
    args.set(arg, true)
  }
}

const baseURL = String(args.get('--base-url') || process.env.POLARIS_URL || 'https://127.0.0.1:49001').replace(/\/$/, '')
const username = process.env.POLARIS_USER || ''
const password = process.env.POLARIS_PASS || ''
const writeScreenshots = args.has('--screenshots') || process.env.POLARIS_SMOKE_SCREENSHOTS === '1'
const screenshotDir = args.get('--screenshot-dir') || '/tmp'
const scriptName = path.basename(fileURLToPath(import.meta.url))

const routes = [
  { name: 'dashboard', hash: '#/', heading: /^Mission Control$/i },
  { name: 'apps-new', hash: '#/apps?new=1', text: /Application Editor|Add New/i },
  { name: 'apps-import', hash: '#/apps?import=1&scan=1', text: /Stage imports|Library/i },
  { name: 'pairing', hash: '#/pin', heading: /pair/i },
  { name: 'browser-stream', hash: '#/browser-stream', heading: /^Browser Stream$/i },
  { name: 'webrtc-alias', hash: '#/webrtc', heading: /^Browser Stream$/i },
  { name: 'troubleshooting', hash: '#/troubleshooting', heading: /^Troubleshooting$/i },
  { name: 'welcome', hash: '#/welcome', heading: /Welcome to Polaris/i },
]

const browser = await chromium.launch()
const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1280, height: 720 } })
const page = await context.newPage()
const consoleErrors = []
const pageErrors = []
let loadedAppShell = false
const ignoredConsoleErrors = [
  /^AI status fetch failed:/,
  /^Device DB fetch failed:/,
  /^Device suggestion fetch failed:/,
  /^Error fetching logs: TypeError: Failed to fetch$/,
]
const ignoredAbortedResourcePaths = [
  /\/api\/clients\/list$/,
  /\/api\/config$/,
  /\/api\/covers\/image\?/,
  /\/api\/devices$/,
  /\/api\/stats\/stream-sse$/,
]

const shouldIgnoreConsoleError = (text, url) => {
  if (ignoredConsoleErrors.some((pattern) => pattern.test(text))) {
    return true
  }
  if (text === 'Failed to load resource: net::ERR_SSL_PROTOCOL_ERROR' && ignoredAbortedResourcePaths.some((pattern) => pattern.test(url))) {
    return true
  }
  return false
}

page.on('console', (message) => {
  if (message.type() === 'error') {
    const text = message.text()
    const location = message.location()
    if (!shouldIgnoreConsoleError(text, location.url || '')) {
      consoleErrors.push(location.url ? `${text} (${location.url})` : text)
    }
  }
})
page.on('pageerror', (error) => {
  pageErrors.push(error.message)
})

try {
  if (username && password && !args.has('--no-login')) {
    await context.clearCookies()
    const loginResponse = await context.request.post(`${baseURL}/api/login`, {
      data: {
        username,
        password,
      },
    })
    if (!loginResponse.ok()) {
      throw new Error(`Login failed with ${loginResponse.status()}: ${await loginResponse.text().catch(() => '')}`)
    }
    await page.goto(`${baseURL}/#/`)
    await expect(page.getByRole('navigation')).toBeVisible({ timeout: 15000 })
    loadedAppShell = true
    consoleErrors.length = 0
    pageErrors.length = 0
  }

  for (const [index, route] of routes.entries()) {
    if (index === 0 && !loadedAppShell) {
      await page.goto(`${baseURL}/?smoke=${Date.now()}-${index}${route.hash}`)
    } else {
      await page.evaluate((hash) => {
        window.location.hash = hash
      }, route.hash)
    }

    if (route.heading) {
      await expect(page.getByRole('heading', { name: route.heading }).first()).toBeVisible({ timeout: 10000 })
    }
    if (route.text) {
      await expect(page.getByText(route.text).first()).toBeVisible({ timeout: 10000 })
    }
    await page.waitForTimeout(500)

    const overlayCount = await page.locator('[data-nextjs-dialog], .vite-error-overlay, #webpack-dev-server-client-overlay').count()
    const bodyText = (await page.locator('body').innerText()).trim()
    if (overlayCount > 0) {
      throw new Error(`${route.name} rendered a framework error overlay`)
    }
    if (bodyText.length < 80) {
      throw new Error(`${route.name} rendered too little content (${bodyText.length} chars)`)
    }
    if (writeScreenshots) {
      await page.screenshot({ path: path.join(screenshotDir, `polaris-smoke-${route.name}.png`), fullPage: false })
    }
    console.log(`${scriptName}: ${route.name} ok (${bodyText.length} chars)`)
  }

  if (pageErrors.length > 0) {
    throw new Error(`Page errors:\n${pageErrors.join('\n')}`)
  }
  if (consoleErrors.length > 0) {
    throw new Error(`Console errors:\n${consoleErrors.join('\n')}`)
  }
} finally {
  await browser.close()
}
