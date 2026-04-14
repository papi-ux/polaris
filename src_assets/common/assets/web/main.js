import './app.css'
import { createApp } from 'vue'
import App from './App.vue'
import { createRouter, createWebHashHistory } from 'vue-router'
import i18n from './locale'

// CSRF token from server-injected meta tag
const csrfToken = document.querySelector('meta[name="csrf-token"]')?.content || ''

// Wrap global fetch to include CSRF token on mutating requests
const originalFetch = window.fetch
window.fetch = function (url, options = {}) {
  const method = (options.method || 'GET').toUpperCase()
  if (method !== 'GET' && method !== 'HEAD' && csrfToken) {
    options.headers = {
      ...(options.headers || {}),
      'X-CSRF-Token': csrfToken,
    }
  }
  return originalFetch.call(this, url, options)
}

const routes = [
  { path: '/', component: () => import(/* webpackChunkName: "dashboard" */ './views/DashboardView.vue') },
  { path: '/info', component: () => import(/* webpackChunkName: "home" */ './views/HomeView.vue') },
  { path: '/apps', component: () => import(/* webpackChunkName: "apps" */ './views/AppsView.vue') },
  { path: '/config', component: () => import(/* webpackChunkName: "config" */ './views/ConfigView.vue') },
  { path: '/pin', component: () => import(/* webpackChunkName: "pin" */ './views/PinView.vue') },
  { path: '/password', component: () => import(/* webpackChunkName: "password" */ './views/PasswordView.vue') },
  { path: '/troubleshooting', component: () => import(/* webpackChunkName: "troubleshooting" */ './views/TroubleshootingView.vue') },
  { path: '/welcome', component: () => import(/* webpackChunkName: "welcome" */ './views/WelcomeView.vue') },
  { path: '/login', component: () => import(/* webpackChunkName: "login" */ './views/LoginView.vue') },
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
  if (to.path !== '/login' && to.path !== '/welcome') {
    if (authed) return
    try {
      const res = await fetch('./api/config')
      await res.text()
      if (res.status === 401) return '/login'
      authed = true
    } catch (e) {
      return '/login'
    }
  }
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
