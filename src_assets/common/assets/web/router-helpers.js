const PUBLIC_ROUTES = new Set(['/login', '/welcome', '/recover'])

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

export function isPublicRoute(path) {
  return PUBLIC_ROUTES.has(path)
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
