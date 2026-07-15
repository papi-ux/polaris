import './app.css'
import { createApp } from 'vue'
import App from './App.vue'
import DashboardView from './views/DashboardView.vue'
import ReconnectingView from './views/ReconnectingView.vue'
import { createRouter, createWebHashHistory } from 'vue-router'
import i18n from './locale'
import {
  isDynamicImportError,
  redirectToIpv4Loopback,
  wrapFetchWithCsrfToken,
} from './router-helpers.js'
import { initializeWebUiAuthState } from './auth-state.js'
import { createWebUiAuthGuard } from './router-auth.js'

// CSRF token from server-injected meta tag
const csrfToken = document.querySelector('meta[name="csrf-token"]')?.content || ''

// Wrap global fetch to include CSRF token on mutating requests
window.fetch = wrapFetchWithCsrfToken(window.fetch, csrfToken)

function normalizeLegacyPairingRoute() {
  const legacyPairing = window.location.hash.match(/^#\/pin#(PIN|OTP|TOFU)$/i)
  if (!legacyPairing) {
    return
  }

  const method = legacyPairing[1].toUpperCase()
  const nextUrl = `${window.location.pathname}${window.location.search}#/pin?method=${method}`
  window.history.replaceState(null, '', nextUrl)
}

normalizeLegacyPairingRoute()

const routes = [
  { path: '/', component: DashboardView },
  { path: '/info', component: () => import(/* webpackChunkName: "home" */ './views/HomeView.vue') },
  { path: '/apps', component: () => import(/* webpackChunkName: "apps" */ './views/AppsView.vue') },
  { path: '/browser-stream', component: () => import(/* webpackChunkName: "browser-stream" */ './views/BrowserStreamView.vue') },
  { path: '/webrtc', redirect: '/browser-stream' },
  { path: '/config', component: () => import(/* webpackChunkName: "config" */ './views/ConfigView.vue') },
  { path: '/pin', component: () => import(/* webpackChunkName: "pin" */ './views/PinView.vue') },
  { path: '/password', component: () => import(/* webpackChunkName: "password" */ './views/PasswordView.vue') },
  { path: '/troubleshooting', component: () => import(/* webpackChunkName: "troubleshooting" */ './views/TroubleshootingView.vue') },
  { path: '/welcome', component: () => import(/* webpackChunkName: "welcome" */ './views/WelcomeView.vue') },
  { path: '/login', component: () => import(/* webpackChunkName: "login" */ './views/LoginView.vue') },
  { path: '/recover', component: () => import(/* webpackChunkName: "recover" */ './views/RecoveryView.vue') },
  { path: '/reconnecting', component: ReconnectingView },
]

const router = createRouter({
  history: createWebHashHistory(),
  routes,
  scrollBehavior(to) {
    if (to.hash) {
      return { el: to.hash, behavior: 'smooth' }
    }
    return { top: 0 }
  },
})

// Auth guard - distinguish confirmed logout from temporary host unavailability.
// Shared state lets the reconnect view hand a successful probe back to the guard
// without issuing a second request during route recovery.
initializeWebUiAuthState()

function dynamicImportReloadKey(to) {
  return `polaris:dynamic-import-reload:${to.fullPath || to.path || '/'}`
}

function reloadDynamicImportTarget(to) {
  const target = to.fullPath || to.path || '/'
  const key = dynamicImportReloadKey(to)
  if (sessionStorage.getItem(key)) {
    return false
  }

  sessionStorage.setItem(key, '1')
  const url = new URL(window.location.href)
  url.hash = target
  url.searchParams.set('polarisReload', String(Date.now()))
  window.location.replace(url.toString())
  return true
}

router.beforeEach(createWebUiAuthGuard())

router.afterEach((to) => {
  sessionStorage.removeItem(dynamicImportReloadKey(to))
})

router.onError((error, to) => {
  if (isDynamicImportError(error) && redirectToIpv4Loopback(window.location, window.location.replace.bind(window.location), `#${to.fullPath}`)) {
    return
  }

  if (isDynamicImportError(error) && reloadDynamicImportTarget(to)) {
    return
  }

  console.error('[Polaris] router', error)
})

const app = createApp(App)

app.config.errorHandler = (err, instance, info) => {
  console.error('[Polaris]', info, err)
}

i18n().then(i18nInstance => {
  app.use(i18nInstance)
  app.provide('i18n', i18nInstance.global)
  app.use(router)
  app.mount('#app')
})
