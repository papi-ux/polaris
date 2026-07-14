import {
  AUTH_PROBE_STATE,
  clearPolarisReloadParam,
  getSafeAuthReturnTarget,
  isPublicRoute,
  normalizeRoutePath,
  probeWebUiAuth,
} from './router-helpers.js'
import {
  isWebUiAuthenticated,
  markWebUiAuthenticated,
  markWebUiUnauthenticated,
} from './auth-state.js'

function redirectToLoginWithReturnTarget(to) {
  return {
    path: '/login',
    query: {
      redirect: to.fullPath || to.path || '/',
    },
  }
}

export function createWebUiAuthGuard(probeAuth = probeWebUiAuth) {
  return async function webUiAuthGuard(to) {
    const routePath = normalizeRoutePath(to.path)
    if (routePath === '/reconnecting') {
      return
    }

    if (isPublicRoute(routePath) && routePath !== '/welcome') {
      const result = await probeAuth()
      if (result.state === AUTH_PROBE_STATE.welcome) {
        markWebUiUnauthenticated()
        return '/welcome'
      }
      if (result.state === AUTH_PROBE_STATE.login) {
        markWebUiUnauthenticated()
      }
      if (result.state === AUTH_PROBE_STATE.authenticated) {
        markWebUiAuthenticated(result.config)
        clearPolarisReloadParam()
        if (routePath === '/login') {
          return getSafeAuthReturnTarget(to.query?.redirect)
        }
      }
    }

    if (!isPublicRoute(routePath)) {
      if (isWebUiAuthenticated()) return

      const result = await probeAuth()
      if (result.state === AUTH_PROBE_STATE.welcome) {
        markWebUiUnauthenticated()
        return '/welcome'
      }
      if (result.state === AUTH_PROBE_STATE.login) {
        markWebUiUnauthenticated()
        return redirectToLoginWithReturnTarget(to)
      }
      if (result.state !== AUTH_PROBE_STATE.authenticated) {
        return {
          path: '/reconnecting',
          query: {
            redirect: to.fullPath || to.path || '/',
          },
        }
      }

      markWebUiAuthenticated(result.config)
      clearPolarisReloadParam()
    }
  }
}
