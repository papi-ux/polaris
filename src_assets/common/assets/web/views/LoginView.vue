<template>
  <div class="relative mx-auto flex min-h-screen max-w-6xl items-center justify-center px-4 py-8 sm:px-6">
    <div class="w-full max-w-5xl">
      <section class="page-hero auth-stage auth-stage--login overflow-visible">
        <div class="page-hero-content items-start">
          <div class="page-hero-copy">
            <div class="flex items-center gap-3">
              <img src="/images/logo-polaris.svg" class="h-12" alt="Polaris">
              <div>
                <div class="page-hero-kicker">Host Console</div>
                <h1 class="page-hero-title">{{ $t('welcome.greeting') }}</h1>
              </div>
            </div>

            <p class="page-hero-copy-text max-w-xl">
              Sign in to manage the host, pairing, and published apps from one control surface.
            </p>

            <div class="page-hero-actions">
              <span class="auth-chip">Mission Control</span>
              <span class="auth-chip">Pairing</span>
              <span class="auth-chip">Library</span>
            </div>
          </div>

          <div class="page-hero-aside hidden lg:block">
            <div class="auth-orbit-panel">
              <div class="auth-panel-kicker">After Sign-In</div>
              <div class="auth-panel-copy">
                Mission Control, pairing, library, and settings stay within one surface.
              </div>
              <div class="auth-orbit-field" aria-hidden="true">
                <div class="auth-orbit-ring auth-orbit-ring--outer"></div>
                <div class="auth-orbit-ring auth-orbit-ring--mid"></div>
                <div class="auth-orbit-core"></div>
                <span class="auth-orbit-node auth-orbit-node--primary">Mission Control</span>
                <span class="auth-orbit-node auth-orbit-node--right">Library</span>
                <span class="auth-orbit-node auth-orbit-node--lower-left">Pairing</span>
                <span class="auth-orbit-node auth-orbit-node--lower-right">Settings</span>
              </div>
            </div>
          </div>
        </div>

        <div class="relative z-[1] border-t border-storm/15 px-6 py-6 md:px-7 md:py-7">
          <form @submit.prevent="login" class="grid gap-5 lg:grid-cols-[minmax(0,1fr)_260px]">
            <div class="space-y-4">
              <div>
                <label for="usernameInput" class="mb-1 block text-sm font-medium text-storm">{{ $t('_common.username') }}</label>
              <input
                id="usernameInput"
                v-model="passwordData.username"
                type="text"
                autocomplete="username"
                required
                autofocus
                class="w-full rounded-xl border border-storm/30 bg-void/55 px-3 py-3 text-silver transition-[border-color,background-color,box-shadow] duration-200 focus:border-ice/40 focus:bg-void/70 focus:outline-none focus:shadow-[0_0_20px_rgba(200,214,229,0.08)]"
              />
              </div>
              <div>
                <div class="mb-1 flex items-center gap-2">
                  <label for="passwordInput" class="block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
                  <InfoHint label="Password guidance">
                    Use the local Polaris web credentials for this host. Keep them separate from game launcher or platform passwords.
                  </InfoHint>
                </div>
                <input
                  id="passwordInput"
                  v-model="passwordData.password"
                  type="password"
                  autocomplete="current-password"
                  required
                  class="w-full rounded-xl border border-storm/30 bg-void/55 px-3 py-3 text-silver transition-[border-color,background-color,box-shadow] duration-200 focus:border-ice/40 focus:bg-void/70 focus:outline-none focus:shadow-[0_0_20px_rgba(200,214,229,0.08)]"
                />
              </div>

              <div class="flex flex-col gap-3 rounded-2xl border border-storm/20 bg-deep/35 p-4 sm:flex-row sm:items-center sm:justify-between">
                <label for="savePassword" class="flex items-center gap-2 text-sm text-storm">
                  <input id="savePassword" v-model="savePassword" type="checkbox" class="h-4 w-4 rounded border-storm bg-void text-ice accent-ice focus:ring-ice" />
                  {{ $t('login.save_password') }}
                </label>
                <div class="flex items-center gap-2 text-xs text-storm">
                  <span>Trusted device only</span>
                  <InfoHint align="right" label="Remember password details">
                    This stores your Polaris web credentials in local browser storage. Use it only on a machine you control.
                  </InfoHint>
                </div>
              </div>

              <button
                type="submit"
                class="inline-flex h-11 w-full items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)] disabled:opacity-50"
                :disabled="loading"
              >
                {{ loading ? 'Signing In…' : $t('welcome.login') }}
              </button>

              <div class="flex flex-wrap items-center gap-2 text-sm text-storm">
                <span>{{ $t('login.recovery_hint') }}</span>
                <router-link to="/recover" class="text-ice transition-colors no-underline hover:text-ice/80">
                  {{ $t('login.forgot_password') }}
                </router-link>
                <InfoHint label="Recovery flow">
                  Recovery is handled on the host itself. Polaris will show the exact terminal command to reset the local web credentials safely.
                </InfoHint>
              </div>

              <div v-if="error" class="rounded-2xl border border-red-500/25 bg-red-500/10 px-4 py-3 text-sm text-red-100">
                <b>{{ $t('_common.error') }}</b> {{ error }}
              </div>
              <div v-if="success" class="rounded-2xl border border-green-500/25 bg-green-500/10 px-4 py-3 text-sm text-green-100">
                <b>{{ $t('_common.success') }}</b> {{ $t('welcome.login_success') }}
              </div>
            </div>

            <aside class="space-y-3">
              <div class="auth-note">
                <div class="auth-note-title">Access Scope</div>
                <div class="auth-note-copy">Pair clients, tune the host, and publish only the apps you want exposed.</div>
              </div>
              <div class="auth-note">
                <div class="flex items-center gap-2 text-sm font-medium text-silver">
                  Secure session
                  <InfoHint align="right" label="Session security">
                    Treat Polaris like a local system console. Sign out or close the browser on shared devices when you are done.
                  </InfoHint>
                </div>
                <div class="mt-2 flex flex-wrap gap-2">
                  <span class="auth-data-pill">Host health</span>
                  <span class="auth-data-pill">Client pairing</span>
                  <span class="auth-data-pill">Runtime config</span>
                </div>
              </div>
            </aside>
          </form>
        </div>
      </section>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import InfoHint from '../components/InfoHint.vue'
import { isDynamicImportError } from '../router-helpers.js'

const router = useRouter()

const error = ref('')
const success = ref(false)
const loading = ref(false)
const savePassword = ref(false)

const passwordData = ref({
  username: '',
  password: ''
})

const savedPasswordStr = localStorage.getItem('login')
if (savedPasswordStr) {
  try {
    const { username, password } = JSON.parse(savedPasswordStr)
    savePassword.value = true
    passwordData.value = { username, password }
  } catch (e) {
    console.error('Reading saved password failed!', e)
  }
}

async function login() {
  error.value = null
  success.value = false
  loading.value = true

  if (!savePassword.value) {
    localStorage.removeItem('login')
  }

  try {
    const res = await fetch("./api/login", {
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify(passwordData.value),
    })

    if (res.status === 200) {
      success.value = true
      if (savePassword.value) {
        localStorage.setItem('login', JSON.stringify(passwordData.value))
      }
      if (window.location.hostname === 'localhost') {
        const ipv4Url = `${window.location.protocol}//127.0.0.1:${window.location.port}${window.location.pathname}#/`
        window.location.replace(ipv4Url)
        return
      }
      await router.push('/')
      return
    }

    let serverError = ''
    const contentType = res.headers.get('content-type') || ''
    if (contentType.includes('application/json')) {
      try {
        const payload = await res.json()
        serverError = payload?.error || ''
      } catch (e) {
        // Ignore malformed error bodies and fall back to status-based messages.
      }
    } else {
      try {
        serverError = await res.text()
      } catch (e) {
        // Ignore body read failures and fall back to status-based messages.
      }
    }

    if (res.status === 409) {
      await router.push('/welcome')
      return
    }

    if (res.status === 401 && !serverError) {
      throw new Error('Please check your username and password')
    }

    throw new Error(serverError || `Server returned ${res.status}`)
  } catch (e) {
    if (success.value) {
      if (isDynamicImportError(e)) {
        error.value = 'Polaris signed you in, but the dashboard did not finish loading. Reload once and try again.'
        return
      }

      error.value = `Signed in, but navigation failed: ${e.message}`
      return
    }

    if (e instanceof TypeError && e.message === 'Failed to fetch') {
      if (window.location.hostname === 'localhost') {
        const fallbackUrl = `${window.location.protocol}//127.0.0.1:${window.location.port}${window.location.pathname}#/login`
        error.value = `Login failed: Polaris could not be reached on localhost. This instance may be listening on IPv4 only. Open ${fallbackUrl} instead.`
      } else {
        error.value = 'Login failed: The browser could not reach Polaris over HTTPS. Reload the page and accept the certificate warning if your browser prompts for it.'
      }
      return
    }

    error.value = `Login failed: ${e.message}`
  } finally {
    loading.value = false
  }
}
</script>
