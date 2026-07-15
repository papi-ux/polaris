const PUBLIC_ROUTES = new Set(['/login', '/welcome', '/recover', '/reconnecting'])

function isMutatingMethod(method) {
  return method !== 'GET' && method !== 'HEAD'
}

function updateDocumentCsrfToken(nextToken) {
  if (!nextToken || typeof document === 'undefined') {
    return
  }

  const meta = document.querySelector('meta[name="csrf-token"]')
  if (meta) {
    meta.setAttribute('content', nextToken)
  }
}

function extractCsrfTokenFromHtml(html) {
  if (!html) {
    return ''
  }

  if (typeof DOMParser !== 'undefined') {
    const parsed = new DOMParser().parseFromString(html, 'text/html')
    return parsed.querySelector('meta[name="csrf-token"]')?.content || ''
  }

  const match = html.match(/<meta\s+name=["']csrf-token["']\s+content=["']([^"']+)["']/i)
  return match ? match[1] : ''
}

export function getIpv4LoopbackUrl(locationLike = window.location, hash = locationLike.hash || '#/') {
  return `${locationLike.protocol}//127.0.0.1:${locationLike.port}${locationLike.pathname}${hash}`
}

export function redirectToIpv4Loopback(
  locationLike = window.location,
  replace = (url) => locationLike.replace(url),
  hash = locationLike.hash || '#/'
) {
  if (locationLike.hostname !== 'localhost') {
    return false
  }

  replace(getIpv4LoopbackUrl(locationLike, hash))
  return true
}

export function isDynamicImportError(error) {
  const message = String(error?.message || error || '')
  return message.includes('Failed to fetch dynamically imported module')
    || message.includes('Importing a module script failed')
}

export function normalizeRoutePath(path) {
  if (typeof path !== 'string') return ''
  const withoutQueryOrHash = path.split(/[?#]/, 1)[0]
  const withoutTrailingSlash = withoutQueryOrHash.replace(/\/+$/, '')
  return (withoutTrailingSlash || '/').toLowerCase()
}

export function isPublicRoute(path) {
  return PUBLIC_ROUTES.has(normalizeRoutePath(path))
}

export function getAuthRedirectPath(response, baseUrl = window.location.href) {
  if (!response?.redirected || !response?.url) {
    return ''
  }

  try {
    const finalPath = normalizeRoutePath(new URL(response.url, baseUrl).pathname)
    if (finalPath.endsWith('/welcome')) {
      return '/welcome'
    }
    if (finalPath.endsWith('/login')) {
      return '/login'
    }
  } catch {
    return ''
  }

  return ''
}

export const AUTH_PROBE_STATE = Object.freeze({
  authenticated: 'authenticated',
  login: 'login',
  unavailable: 'unavailable',
  welcome: 'welcome',
})

const AUTH_PROBE_TIMEOUT_MS = 5000

async function discardResponseBody(response) {
  try {
    await response?.body?.cancel()
  } catch {
    // The classification result matters more than cleanup of an already-failed response.
  }
}

export async function probeWebUiAuth(fetchImpl = window.fetch, baseUrl = window.location.href, externalSignal) {
  const controller = new AbortController()
  const abortProbe = () => controller.abort()
  const timeout = globalThis.setTimeout(abortProbe, AUTH_PROBE_TIMEOUT_MS)

  if (externalSignal?.aborted) {
    abortProbe()
  } else {
    externalSignal?.addEventListener('abort', abortProbe, { once: true })
  }

  try {
    const response = await fetchImpl('./api/config', {
      credentials: 'include',
      cache: 'no-store',
      signal: controller.signal,
    })
    const redirectPath = getAuthRedirectPath(response, baseUrl)
    if (redirectPath === '/welcome') {
      await discardResponseBody(response)
      return { state: AUTH_PROBE_STATE.welcome }
    }
    if (redirectPath === '/login' || response.status === 401) {
      await discardResponseBody(response)
      return { state: AUTH_PROBE_STATE.login }
    }
    if (!response.ok) {
      await discardResponseBody(response)
      return { state: AUTH_PROBE_STATE.unavailable }
    }

    const contentType = response.headers.get('content-type') || ''
    const mediaType = contentType.split(';', 1)[0].trim().toLowerCase()
    if (mediaType !== 'application/json') {
      await discardResponseBody(response)
      return { state: AUTH_PROBE_STATE.unavailable }
    }

    const config = await response.json()
    if (!config || Array.isArray(config) || typeof config !== 'object' || config.status !== true) {
      return { state: AUTH_PROBE_STATE.unavailable }
    }

    return {
      state: AUTH_PROBE_STATE.authenticated,
      config,
    }
  } catch {
    return { state: AUTH_PROBE_STATE.unavailable }
  } finally {
    globalThis.clearTimeout(timeout)
    externalSignal?.removeEventListener('abort', abortProbe)
  }
}

export function clearPolarisReloadParam(
  locationLike = window.location,
  replaceState = window.history.replaceState.bind(window.history),
  historyState = window.history.state
) {
  const url = new URL(locationLike.href)
  if (!url.searchParams.has('polarisReload')) {
    return false
  }

  url.searchParams.delete('polarisReload')
  replaceState(historyState, '', `${url.pathname}${url.search}${url.hash}`)
  return true
}

export function getSafeAuthReturnTarget(value, fallback = '/') {
  const candidate = Array.isArray(value) ? value[0] : value
  if (typeof candidate !== 'string' || !candidate.startsWith('/') || candidate.startsWith('//')) {
    return fallback
  }

  if (isPublicRoute(candidate)) {
    return fallback
  }

  return candidate
}

export function wrapFetchWithCsrfToken(originalFetch, initialCsrfToken, getRefreshUrl = () => window.location.pathname || '/') {
  let csrfToken = initialCsrfToken

  async function refreshCsrfToken(context) {
    const refreshUrl = getRefreshUrl()
    if (!refreshUrl) {
      return ''
    }

    const response = await originalFetch.call(context, refreshUrl, {
      credentials: 'include',
      cache: 'no-store',
      headers: {
        Accept: 'text/html',
      },
    })

    if (!response.ok) {
      return ''
    }

    const html = await response.text()
    const nextToken = extractCsrfTokenFromHtml(html)
    if (!nextToken) {
      return ''
    }

    csrfToken = nextToken
    updateDocumentCsrfToken(nextToken)
    return nextToken
  }

  return async function fetchWithCsrf(url, options = {}) {
    const method = (options.method || 'GET').toUpperCase()

    const buildOptions = () => {
      if (!isMutatingMethod(method) || !csrfToken) {
        return options
      }

      return {
        ...options,
        headers: {
          ...(options.headers || {}),
          'X-CSRF-Token': csrfToken,
        },
      }
    }

    let response = await originalFetch.call(this, url, buildOptions())
    if (isMutatingMethod(method) && (response.status === 401 || response.status === 403)) {
      const refreshedToken = await refreshCsrfToken(this)
      if (refreshedToken) {
        response = await originalFetch.call(this, url, buildOptions())
      }
    }

    return response
  }
}
