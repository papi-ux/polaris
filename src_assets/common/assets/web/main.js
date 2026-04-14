import './app.css'
import { createApp } from 'vue'
import App from './App.vue'
import DashboardView from './views/DashboardView.vue'
import { createRouter, createWebHashHistory } from 'vue-router'
import i18n from './locale'
import {
  isDynamicImportError,
  isPublicRoute,
  redirectToIpv4Loopback,
  wrapFetchWithCsrfToken,
} from './router-helpers.js'

// CSRF token from server-injected meta tag
const csrfToken = document.querySelector('meta[name="csrf-token"]')?.content || ''

// Wrap global fetch to include CSRF token on mutating requests
window.fetch = wrapFetchWithCsrfToken(window.fetch, csrfToken)

const routes = [
  { path: '/', component: DashboardView },
  { path: '/info', component: () => import(/* webpackChunkName: "home" */ './views/HomeView.vue') },
  { path: '/apps', component: () => import(/* webpackChunkName: "apps" */ './views/AppsView.vue') },
  { path: '/config', component: () => import(/* webpackChunkName: "config" */ './views/ConfigView.vue') },
  { path: '/pin', component: () => import(/* webpackChunkName: "pin" */ './views/PinView.vue') },
  { path: '/password', component: () => import(/* webpackChunkName: "password" */ './views/PasswordView.vue') },
  { path: '/troubleshooting', component: () => import(/* webpackChunkName: "troubleshooting" */ './views/TroubleshootingView.vue') },
  { path: '/welcome', component: () => import(/* webpackChunkName: "welcome" */ './views/WelcomeView.vue') },
  { path: '/login', component: () => import(/* webpackChunkName: "login" */ './views/LoginView.vue') },
  { path: '/recover', component: () => import(/* webpackChunkName: "recover" */ './views/RecoveryView.vue') },
]

const router = createRouter({
  history: createWebHashHistory(),
  routes,
})

// Auth guard - redirect to login if not authenticated
// Uses a cached auth flag to avoid making a fetch on every navigation,
// which exhausts server connections when response bodies aren't consumed.
let authed = false
router.beforeEach(async (to, _from) => {
  if (!isPublicRoute(to.path)) {
    if (authed) return
    try {
      const res = await fetch('./api/config')
      const finalPath = res.redirected ? new URL(res.url).pathname : ''
      await res.text()
      if (finalPath.endsWith('/welcome')) return '/welcome'
      if (res.status === 401) return '/login'
      if (!res.ok) return '/login'
      authed = true
    } catch (e) {
      return '/login'
    }
  }
})

router.onError((error, to) => {
  if (isDynamicImportError(error) && redirectToIpv4Loopback(window.location, window.location.replace.bind(window.location), `#${to.fullPath}`)) {
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
