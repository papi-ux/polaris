import { chromium, expect, request as playwrightRequest } from '@playwright/test'
import fs from 'node:fs/promises'
import http from 'node:http'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const DEFAULT_LIVE_URL = 'https://127.0.0.1:47990'
const DEFAULT_STATIC_DIR = 'build/assets/web'
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

const mimeTypes = new Map([
  ['.css', 'text/css; charset=utf-8'],
  ['.html', 'text/html; charset=utf-8'],
  ['.ico', 'image/x-icon'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.json', 'application/json; charset=utf-8'],
  ['.png', 'image/png'],
  ['.svg', 'image/svg+xml; charset=utf-8'],
])

export function getSmokeHelp() {
  return `Usage: npm run smoke:web -- [options]

Modes:
  --target-url <url>        Smoke a live Polaris HTTPS server, e.g. --target-url https://127.0.0.1:47990
  --base-url <url>          Alias for --target-url (also POLARIS_URL). Defaults to ${DEFAULT_LIVE_URL}
  --static                  Smoke built static web assets from ${DEFAULT_STATIC_DIR}
  --static-dir <dir>        Smoke a specific built web asset directory (implies --static)

Auth and coverage:
  POLARIS_USER/POLARIS_PASS Login before authenticated route checks when both are set
  --no-login                Skip credential login and only verify unauthenticated login shell/assets

Diagnostics:
  --screenshots             Write route screenshots (or POLARIS_SMOKE_SCREENSHOTS=1)
  --screenshot-dir <dir>    Screenshot directory, default /tmp
  --help                    Show this help

Notes:
  Live mode keeps self-signed local Polaris certificate handling explicit by using Playwright's ignoreHTTPSErrors.
  If the live HTTPS server is missing, the preflight fails with a Polaris-specific message instead of raw ERR_CONNECTION_REFUSED.`
}

function parseRawArgs(argv) {
  const args = new Map()
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i]
    if (arg.startsWith('--') && argv[i + 1] && !argv[i + 1].startsWith('--')) {
      args.set(arg, argv[i + 1])
      i += 1
    } else {
      args.set(arg, true)
    }
  }
  return args
}

function normalizeBaseURL(url) {
  return String(url).replace(/\/$/, '')
}

export function parseSmokeOptions(argv = process.argv.slice(2), env = process.env) {
  const args = parseRawArgs(argv)
  const staticDirArg = args.get('--static-dir')
  const mode = args.has('--static') || staticDirArg ? 'static' : 'live'
  const targetURL = args.get('--target-url') || args.get('--base-url') || env.POLARIS_URL || DEFAULT_LIVE_URL

  return {
    help: args.has('--help') || args.has('-h'),
    mode,
    baseURL: mode === 'live' ? normalizeBaseURL(targetURL) : null,
    staticDir: mode === 'static' ? path.resolve(String(staticDirArg || DEFAULT_STATIC_DIR)) : null,
    username: env.POLARIS_USER || '',
    password: env.POLARIS_PASS || '',
    noLogin: args.has('--no-login'),
    writeScreenshots: args.has('--screenshots') || env.POLARIS_SMOKE_SCREENSHOTS === '1',
    screenshotDir: String(args.get('--screenshot-dir') || '/tmp'),
  }
}

function shouldIgnoreConsoleError(text, url) {
  if (ignoredConsoleErrors.some((pattern) => pattern.test(text))) {
    return true
  }
  if (text === 'Failed to load resource: net::ERR_SSL_PROTOCOL_ERROR' && ignoredAbortedResourcePaths.some((pattern) => pattern.test(url))) {
    return true
  }
  return false
}

function isHashedAsset(assetPath) {
  const basename = path.posix.basename(assetPath)
  return /-[A-Za-z0-9_-]{6,}\.(?:css|js)$/.test(basename)
}

function extractAssetPaths(indexHtml) {
  const assets = []
  const pattern = /(?:src|href)=["'](?:\.\/)?(assets\/[A-Za-z0-9._/-]+\.(?:css|js))["']/g
  for (const match of indexHtml.matchAll(pattern)) {
    const assetPath = `/${match[1]}`
    if (isHashedAsset(assetPath) && !assets.includes(assetPath)) {
      assets.push(assetPath)
    }
  }
  return assets
}

export async function collectCriticalHashedAssets(staticDir) {
  const root = path.resolve(staticDir)
  const indexPath = path.join(root, 'index.html')
  const indexHtml = await fs.readFile(indexPath, 'utf8')
  const assets = extractAssetPaths(indexHtml)

  if (assets.length === 0) {
    throw new Error(`${indexPath} does not reference hashed JS/CSS assets`)
  }

  for (const asset of assets) {
    await fs.access(path.join(root, asset))
  }

  return assets
}

async function fetchCriticalHashedAssets(baseURL, apiRequest) {
  const indexResponse = await apiRequest.get(`${baseURL}/`)
  if (!indexResponse.ok()) {
    throw new Error(`Index page failed with ${indexResponse.status()} at ${baseURL}/`)
  }

  const assets = extractAssetPaths(await indexResponse.text())
  if (assets.length === 0) {
    throw new Error(`Index page at ${baseURL}/ does not reference hashed JS/CSS assets`)
  }

  for (const asset of assets) {
    const assetResponse = await apiRequest.get(`${baseURL}${asset}`)
    if (!assetResponse.ok()) {
      throw new Error(`Critical hashed asset failed with ${assetResponse.status()}: ${baseURL}${asset}`)
    }
    console.log(`${scriptName}: asset ok ${asset}`)
  }

  return assets
}

export async function runLivePreflight({ baseURL, request = null } = {}) {
  const requester = request || (await playwrightRequest.newContext({ ignoreHTTPSErrors: true }))
  const ownsRequester = !request
  try {
    const response = await requester.get(`${baseURL}/`, { timeout: 5000 })
    if (!response.ok()) {
      throw new Error(`Polaris live HTTPS server responded with ${response.status()} at ${baseURL}/`)
    }
    return response
  } catch (error) {
    const cause = error?.code || error?.message || String(error)
    if (/ECONNREFUSED|ERR_CONNECTION_REFUSED|connect ECONNREFUSED/i.test(cause)) {
      throw new Error(`Polaris live HTTPS server is not reachable at ${baseURL}. Start Polaris or pass --static-dir build/assets/web for static asset smoke. (${cause})`)
    }
    throw error
  } finally {
    if (ownsRequester) {
      await requester.dispose()
    }
  }
}

function safeStaticPath(root, requestUrl) {
  const parsed = new URL(requestUrl, 'http://127.0.0.1')
  let pathname = decodeURIComponent(parsed.pathname)
  if (pathname === '/') {
    pathname = '/index.html'
  }
  const candidate = path.resolve(root, `.${pathname}`)
  if (!candidate.startsWith(`${root}${path.sep}`) && candidate !== root) {
    return null
  }
  return candidate
}

async function startStaticServer(staticDir) {
  const root = path.resolve(staticDir)
  await fs.access(path.join(root, 'index.html'))

  const server = http.createServer(async (req, res) => {
    const candidate = safeStaticPath(root, req.url || '/')
    if (!candidate) {
      res.writeHead(403)
      res.end('Forbidden')
      return
    }

    try {
      const body = await fs.readFile(candidate)
      const mimeType = mimeTypes.get(path.extname(candidate)) || 'application/octet-stream'
      res.writeHead(200, { 'content-type': mimeType })
      res.end(body)
    } catch (error) {
      res.writeHead(error?.code === 'ENOENT' ? 404 : 500)
      res.end(error?.code === 'ENOENT' ? 'Not found' : 'Server error')
    }
  })

  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', resolve)
  })

  const address = server.address()
  return {
    baseURL: `http://127.0.0.1:${address.port}`,
    close: () => new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve())),
  }
}

async function verifyUnauthenticatedLoginPage(page, baseURL) {
  await page.context().clearCookies()
  await page.goto(`${baseURL}/?smoke=${Date.now()}#/login`)
  await expect(page.getByRole('heading', { name: /Welcome to Polaris/i }).first()).toBeVisible({ timeout: 15000 })
  await expect(page.getByLabel(/username/i)).toBeVisible({ timeout: 10000 })
  await expect(page.getByLabel(/^password$/i)).toBeVisible({ timeout: 10000 })
  console.log(`${scriptName}: unauthenticated login page ok`)
}

async function verifyAuthenticatedRoutes(page, baseURL, options) {
  const loginResponse = await page.context().request.post(`${baseURL}/api/login`, {
    data: {
      username: options.username,
      password: options.password,
    },
  })
  if (!loginResponse.ok()) {
    throw new Error(`Login failed with ${loginResponse.status()}: ${await loginResponse.text().catch(() => '')}`)
  }

  for (const [index, route] of routes.entries()) {
    if (index === 0) {
      await page.goto(`${baseURL}/?smoke=${Date.now()}-${index}${route.hash}`)
      await expect(page.getByRole('navigation')).toBeVisible({ timeout: 15000 })
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
    if (options.writeScreenshots) {
      await page.screenshot({ path: path.join(options.screenshotDir, `polaris-smoke-${route.name}.png`), fullPage: false })
    }
    console.log(`${scriptName}: ${route.name} ok (${bodyText.length} chars)`)
  }
}

async function runBrowserSmoke(baseURL, options) {
  const browser = await chromium.launch()
  const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1280, height: 720 } })
  const page = await context.newPage()
  const consoleErrors = []
  const pageErrors = []

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
    await verifyUnauthenticatedLoginPage(page, baseURL)
    consoleErrors.length = 0
    pageErrors.length = 0

    if (options.username && options.password && !options.noLogin && options.mode === 'live') {
      await verifyAuthenticatedRoutes(page, baseURL, options)
    } else {
      console.log(`${scriptName}: authenticated route checks skipped (set POLARIS_USER/POLARIS_PASS for live route smoke)`)
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
}

export async function runSmoke(options = parseSmokeOptions()) {
  if (options.help) {
    console.log(getSmokeHelp())
    return
  }

  let staticServer = null
  let baseURL = options.baseURL
  let apiRequest = null
  try {
    if (options.mode === 'static') {
      const assets = await collectCriticalHashedAssets(options.staticDir)
      console.log(`${scriptName}: static assets found ${assets.join(', ')}`)
      staticServer = await startStaticServer(options.staticDir)
      baseURL = staticServer.baseURL
      apiRequest = await playwrightRequest.newContext()
      await fetchCriticalHashedAssets(baseURL, apiRequest)
    } else {
      console.log(`${scriptName}: live HTTPS preflight ${baseURL} (self-signed certs allowed for local Polaris)`)
      await runLivePreflight({ baseURL })
      apiRequest = await playwrightRequest.newContext({ ignoreHTTPSErrors: true })
      await fetchCriticalHashedAssets(baseURL, apiRequest)
    }

    await runBrowserSmoke(baseURL, options)
  } finally {
    if (apiRequest) {
      await apiRequest.dispose()
    }
    if (staticServer) {
      await staticServer.close()
    }
  }
}

function isMainModule() {
  return process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href
}

if (isMainModule()) {
  runSmoke().catch((error) => {
    console.error(`${scriptName}: ${error.message}`)
    process.exitCode = 1
  })
}
