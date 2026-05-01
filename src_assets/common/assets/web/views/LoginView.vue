<template>
  <div class="relative min-h-screen overflow-hidden bg-background">
    <div class="pointer-events-none absolute inset-0 bg-[radial-gradient(circle_at_top_left,rgba(124,115,255,0.18),transparent_35%),radial-gradient(circle_at_bottom_right,rgba(200,214,229,0.08),transparent_28%)]"></div>
    <div class="relative mx-auto flex min-h-screen max-w-5xl items-center justify-center p-4 sm:p-6">
      <div class="grid w-full max-w-4xl gap-6 lg:grid-cols-[minmax(0,1fr)_320px]">
        <section class="glass rounded-[28px] border border-storm/30 p-6 shadow-2xl sm:p-8">
          <div class="flex items-center gap-3">
            <img src="/images/logo-polaris.svg" class="h-12" alt="Polaris">
            <div>
              <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm/85">Host Console</div>
              <h1 class="mt-1 text-3xl font-bold text-silver">{{ $t('welcome.greeting') }}</h1>
            </div>
          </div>

          <div class="mt-6 max-w-2xl">
            <p class="text-sm leading-relaxed text-storm">
              Sign in to manage pairing, library entries, host health, and stream configuration. This is the control surface for the machine, not just a passive dashboard.
            </p>
          </div>

          <form @submit.prevent="login" class="mt-8 space-y-4">
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
              <label for="passwordInput" class="mb-1 block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
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
              <div class="text-xs text-storm">Use this only on a device you control.</div>
            </div>

            <button
              type="submit"
              class="inline-flex h-11 w-full items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)] disabled:opacity-50"
              :disabled="loading"
            >
              {{ loading ? 'Signing In…' : $t('welcome.login') }}
            </button>

            <div class="text-sm text-storm">
              {{ $t('login.recovery_hint') }}
              <router-link to="/recover" class="ml-1 text-ice transition-colors no-underline hover:text-ice/80">
                {{ $t('login.forgot_password') }}
              </router-link>
            </div>

            <div v-if="error" class="rounded-2xl border border-red-500/25 bg-red-500/10 px-4 py-3 text-sm text-red-100">
              <b>{{ $t('_common.error') }}</b> {{ error }}
            </div>
            <div v-if="success" class="rounded-2xl border border-green-500/25 bg-green-500/10 px-4 py-3 text-sm text-green-100">
              <b>{{ $t('_common.success') }}</b> {{ $t('welcome.login_success') }}
            </div>
          </form>
        </section>

        <aside class="section-card self-center">
          <div class="section-kicker">Before You Continue</div>
          <h2 class="section-title">What this login gives you</h2>
          <p class="section-copy">
            Once signed in, you can publish launch entries, review runtime telemetry, change host security settings, and restart Polaris from the web UI.
          </p>

          <div class="mt-5 space-y-3">
            <div class="surface-subtle p-4">
              <div class="text-sm font-semibold text-silver">Security</div>
              <p class="mt-2 text-sm leading-relaxed text-storm">
                Pairing and password changes affect who can control the host. Treat this interface like a system console, not a public status page.
              </p>
            </div>
            <div class="surface-subtle p-4">
              <div class="text-sm font-semibold text-silver">Recovery</div>
              <p class="mt-2 text-sm leading-relaxed text-storm">
                If credentials stop working, use the recovery flow or reset them locally on the host before troubleshooting anything else.
              </p>
            </div>
          </div>
        </aside>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter } from 'vue-router'
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
