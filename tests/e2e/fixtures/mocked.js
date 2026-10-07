// Shared fixture for the mocked frontend lane (see playwright.mocked-specs.js).
//
// Each case's own page.route mocks stay authoritative: Playwright gives page routes
// precedence over context routes. The context route below only sees what those mocks
// left alone, and it keeps the case on this checkout's assets and off any host or
// network:
// - same-origin frontend asset GETs continue to the owned preview server;
// - an unmocked same-origin /api request is refused and recorded;
// - the exact GitHub Polaris release endpoints get a deterministic "no update";
// - every other request is refused before it leaves the browser, and recorded.
// Every main-document HTML response must carry the served build's source identity.
// Violations and pending checks are reconciled in the case's own teardown, where any
// of them fails the case.
import { test as base, expect } from '@playwright/test'

const SOURCE_META = 'polaris-e2e-source'
const RELEASES_ORIGIN = 'https://api.github.com'
const RELEASES_PATH = '/repos/papi-ux/polaris/releases'
// Older than any installable build, so the console never offers an update.
const NO_UPDATE_RELEASE = {
  tag_name: 'v0.0.0', name: 'v0.0.0', prerelease: false, draft: false,
  html_url: 'https://example.invalid/polaris-e2e-no-update', assets: [],
}
const SETTLE_TIMEOUT_MS = 10000

function mockedFrontendMetadata(testInfo) {
  const metadata = testInfo.config.metadata?.mockedFrontend
  const problems = []
  if (!metadata || typeof metadata !== 'object') {
    problems.push('config.metadata.mockedFrontend is missing')
  } else {
    let origin = null
    try {
      origin = new URL(metadata.origin)
    } catch {
      problems.push(`origin ${JSON.stringify(metadata.origin)} is not a URL`)
    }
    if (origin && (origin.origin !== metadata.origin || !['http:', 'https:'].includes(origin.protocol)
        || !['127.0.0.1', 'localhost', '[::1]'].includes(origin.hostname))) {
      problems.push(`origin ${metadata.origin} is not an exact loopback http(s) origin`)
    }
    if (typeof metadata.identity !== 'string' || !/^[A-Za-z0-9_-]+$/.test(metadata.identity)) {
      problems.push('identity is not a base64url token')
    }
    for (const key of ['sourceSha', 'sourceFingerprint']) {
      if (typeof metadata[key] !== 'string' || !metadata[key]) problems.push(`${key} is missing`)
    }
  }
  if (problems.length) {
    throw new Error(`Mocked frontend lane is not configured: ${problems.join('; ')}`)
  }
  return metadata
}

function attributes(tag) {
  const values = {}
  const pattern = /([^\s"'<>/=]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))?/g
  for (const match of tag.replace(/^<meta\b/i, '').replace(/\/?>$/, '').matchAll(pattern)) {
    values[match[1].toLowerCase()] = match[2] ?? match[3] ?? match[4] ?? ''
  }
  return values
}

// The identity values of every <meta name="polaris-e2e-source"> in the document.
export function sourceIdentities(html) {
  return [...html.matchAll(/<meta\b[^>]*>/gi)]
    .map(([tag]) => attributes(tag))
    .filter((meta) => (meta.name || '').toLowerCase() === SOURCE_META)
    .map((meta) => meta.content ?? '')
}

function isMainFrameDocument(response) {
  if (response.request().resourceType() !== 'document') return false
  try {
    const frame = response.frame()
    return frame === frame.page().mainFrame()
  } catch {
    return false // a service-worker or detached response has no main frame
  }
}

export const test = base.extend({
  context: async ({ context, serviceWorkers }, use, testInfo) => {
    const metadata = mockedFrontendMetadata(testInfo)
    const violations = []
    const pending = new Set()
    let finalized = false
    const violation = (message) => {
      if (!finalized) violations.push(message)
    }
    // Every asynchronous check is tracked and can only resolve; teardown awaits them.
    const track = (work) => {
      const promise = Promise.resolve().then(work).catch((error) => violation(`internal check failed: ${error?.message || error}`))
      pending.add(promise)
      promise.finally(() => pending.delete(promise))
    }

    if (serviceWorkers !== 'block') {
      violation(`serviceWorkers is ${JSON.stringify(serviceWorkers)}; the mocked lane requires 'block' so routing sees every request`)
    }
    testInfo.annotations.push({ type: 'mocked-frontend-source', description: `${metadata.sourceSha} ${metadata.sourceFingerprint}` })

    await context.route('**/*', async (route) => {
      const request = route.request()
      const method = request.method()
      let url
      try {
        url = new URL(request.url())
      } catch {
        violation(`unparseable request URL ${request.url()}`)
        await route.abort('blockedbyclient').catch(() => {})
        return
      }
      try {
        if (url.origin === metadata.origin) {
          if (url.pathname === '/api' || url.pathname.startsWith('/api/')) {
            violation(`unmocked API request ${method} ${url.pathname}`)
            await route.abort('blockedbyclient')
          } else if (method === 'GET' || method === 'HEAD') {
            await route.continue()
          } else {
            violation(`unexpected same-origin ${method} ${url.pathname}`)
            await route.abort('blockedbyclient')
          }
        } else if (url.origin === RELEASES_ORIGIN && method === 'GET' && !url.search
            && (url.pathname === RELEASES_PATH || url.pathname === `${RELEASES_PATH}/latest`)) {
          await route.fulfill({ json: url.pathname === RELEASES_PATH ? [] : NO_UPDATE_RELEASE })
        } else {
          violation(`external request refused ${method} ${url.origin}${url.pathname}`)
          await route.abort('blockedbyclient')
        }
      } catch (error) {
        // The page may close while a request is in flight; that is not a lane failure.
        if (!/closed|Target page|detached/i.test(String(error?.message))) {
          violation(`routing ${method} ${url.origin}${url.pathname} failed: ${error?.message || error}`)
        }
      }
    })
    // WebSocket traffic is not HTTP routing; refuse it the same way, without connecting.
    await context.routeWebSocket(/.*/, (socket) => {
      violation(`WebSocket refused ${socket.url()}`)
      socket.close().catch(() => {})
    })

    context.on('response', (response) => {
      if (!isMainFrameDocument(response)) return
      const status = response.status()
      if (status >= 300 && status < 400) return // the redirected-to document is checked itself
      let origin = ''
      try {
        origin = new URL(response.url()).origin
      } catch {}
      if (origin !== metadata.origin) {
        violation(`main document from ${response.url()} is not the owned origin ${metadata.origin}`)
        return
      }
      const contentType = (response.headers()['content-type'] || '').toLowerCase()
      if (!contentType.includes('text/html')) return
      track(async () => {
        let html
        try {
          html = await response.text()
        } catch (error) {
          violation(`main document ${response.url()} could not be read for its source identity: ${error?.message || error}`)
          return
        }
        const identities = sourceIdentities(html)
        if (identities.length !== 1) {
          violation(`main document ${response.url()} has ${identities.length} ${SOURCE_META} meta tags (expected 1)`)
        } else if (identities[0] !== metadata.identity) {
          violation(`main document ${response.url()} was built from ${identities[0]}, not this checkout's ${metadata.identity}`)
        }
      })
    })

    await use(context)

    // Reconcile in the case itself: settle every pending check (bounded), then fail on
    // anything recorded. Requests after this point are still refused, but not reported.
    const deadline = Date.now() + SETTLE_TIMEOUT_MS
    while (pending.size && Date.now() < deadline) {
      await Promise.race([Promise.allSettled([...pending]), new Promise((resolve) => setTimeout(resolve, 100))])
    }
    if (pending.size) violation(`${pending.size} source identity check(s) did not settle within ${SETTLE_TIMEOUT_MS} ms`)
    finalized = true
    if (violations.length) {
      throw new Error(`Mocked frontend lane violations:\n- ${violations.join('\n- ')}`)
    }
  },
})

export { expect }
