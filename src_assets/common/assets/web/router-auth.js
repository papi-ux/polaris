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
import {
  beginWebUiAuthProbeGeneration,
  isCurrentWebUiAuthProbeGeneration,
  runWebUiAuthProbeForGeneration,
} from './auth-probe-coordinator.js'

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
    const generation = beginWebUiAuthProbeGeneration()
    const routePath = normalizeRoutePath(to.path)
    if (routePath === '/reconnecting') {
      return
    }

    const runCurrentProbe = () => runWebUiAuthProbeForGeneration(
      generation,
      (signal) => probeAuth(window.fetch, window.location.href, signal)
    )

    if (isPublicRoute(routePath) && routePath !== '/welcome') {
      const outcome = await runCurrentProbe()
      if (!outcome.current || !isCurrentWebUiAuthProbeGeneration(generation)) return false
      const result = outcome.result

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

      const outcome = await runCurrentProbe()
      if (!outcome.current || !isCurrentWebUiAuthProbeGeneration(generation)) return false
      const result = outcome.result

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
