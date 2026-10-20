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
// Every main-document HTML response must carry the served build's source identity,
// and a passing case must have loaded at least one such document.
// The fixture owns the context's close: guards stay installed until the context has
// actually closed, and only then are violations and pending checks reconciled in the
// case's own teardown, where any of them fails the case.
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
const CLOSE_DRAIN_TIMEOUT_MS = 5000

// scripts/e2e-frontend-identity.mjs encodes schema 1 exactly like this.
function schemaOneIdentity({ sourceSha, sourceFingerprint }) {
  return Buffer.from(JSON.stringify({ schema: 1, sourceSha, sourceFingerprint })).toString('base64url')
}

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
    if (typeof metadata.sourceSha !== 'string' || !/^([0-9a-f]{40}|[0-9a-f]{64})$/.test(metadata.sourceSha)) {
      problems.push('sourceSha is not a full Git object id')
    }
    if (typeof metadata.sourceFingerprint !== 'string' || !/^[0-9a-f]{64}$/.test(metadata.sourceFingerprint)) {
      problems.push('sourceFingerprint is not a sha256 hex digest')
    }
    if (!problems.length && metadata.identity !== schemaOneIdentity(metadata)) {
      problems.push('identity is not the schema 1 token for this sourceSha and sourceFingerprint')
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
    const observed = { verifiedDocuments: 0, releaseResponses: { releases: 0, latest: 0 } }
    const violation = (message) => violations.push(message)
    // Every asynchronous check and route handler is tracked and can only resolve;
    // teardown awaits them.
    const track = (work) => {
      const promise = Promise.resolve().then(work).catch((error) => violation(`internal check failed: ${error?.message || error}`))
      pending.add(promise)
      promise.finally(() => pending.delete(promise))
      return promise
    }
    // Wait (bounded) until nothing is pending; work tracked meanwhile is waited for too.
    const drain = async (timeoutMs, stage) => {
      const deadline = Date.now() + timeoutMs
      while (pending.size && Date.now() < deadline) {
        let timer
        await Promise.race([
          Promise.allSettled([...pending]),
          new Promise((resolve) => { timer = setTimeout(resolve, Math.min(100, deadline - Date.now())) }),
        ])
        clearTimeout(timer)
      }
      if (pending.size) violation(`${pending.size} check(s) did not settle within ${timeoutMs} ms ${stage}`)
    }

    if (serviceWorkers !== 'block') {
      violation(`serviceWorkers is ${JSON.stringify(serviceWorkers)}; the mocked lane requires 'block' so routing sees every request`)
    }
    testInfo.annotations.push({ type: 'mocked-frontend-source', description: `${metadata.sourceSha} ${metadata.sourceFingerprint}` })

    await context.route('**/*', (route) => track(async () => {
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
          const list = url.pathname === RELEASES_PATH
          await route.fulfill({ json: list ? [] : NO_UPDATE_RELEASE })
          observed.releaseResponses[list ? 'releases' : 'latest'] += 1
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
    }))
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
        } else {
          observed.verifiedDocuments += 1
        }
      })
    })

    await use(context)

    // The fixture, not the base fixture, closes the context, so guards stay active until
    // close completes. The base close that follows is then a no-op apart from its video
    // handling (BrowserContext.close returns at once on a closed context).
    await drain(SETTLE_TIMEOUT_MS, 'before the context closed')
    try {
      await context.close({ reason: testInfo.status === 'timedOut'
        ? `Test timeout of ${testInfo.timeout}ms exceeded.` : 'Test ended.' })
    } catch (error) {
      violation(`closing the context failed: ${error?.message || error}`)
    }
    await drain(CLOSE_DRAIN_TIMEOUT_MS, 'after the context closed')

    // A passing case is source-verified only if it loaded at least one checked document.
    if (testInfo.status === 'passed' && observed.verifiedDocuments === 0 && !violations.length) {
      violation('no main HTML document was loaded and verified, so the case is not source-verified')
    }
    testInfo.annotations.push({ type: 'mocked-frontend-observed', description: JSON.stringify({
      ...observed, violations: violations.length, unsettled: pending.size }) })
    if (violations.length) {
      throw new Error(`Mocked frontend lane violations:\n- ${violations.join('\n- ')}`)
    }
  },
})

export { expect }
