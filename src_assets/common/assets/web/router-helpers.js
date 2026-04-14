const PUBLIC_ROUTES = new Set(['/login', '/welcome', '/recover'])

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

export function wrapFetchWithCsrfToken(originalFetch, csrfToken) {
  return function fetchWithCsrf(url, options = {}) {
    const method = (options.method || 'GET').toUpperCase()
    if (method !== 'GET' && method !== 'HEAD' && csrfToken) {
      options.headers = {
        ...(options.headers || {}),
        'X-CSRF-Token': csrfToken,
      }
    }
    return originalFetch.call(this, url, options)
  }
}
