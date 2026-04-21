<template>
  <div class="relative mx-auto flex min-h-screen max-w-6xl items-center justify-center px-4 py-8 sm:px-6">
    <div class="w-full max-w-5xl">
      <section class="page-hero auth-stage auth-stage--login overflow-visible">
        <div class="page-hero-content auth-login-header items-start">
          <div class="page-hero-copy auth-login-copy">
            <div class="flex items-center gap-3">
              <img src="/images/logo-polaris.svg" class="h-12" alt="Polaris">
              <div>
                <div class="page-hero-kicker">Host Console</div>
                <h1 class="page-hero-title">{{ $t('welcome.greeting') }}</h1>
              </div>
            </div>

            <p class="page-hero-copy-text auth-login-copy-text">
              Sign in to manage the host from one local control surface.
            </p>
          </div>
        </div>

        <div class="relative z-[1] auth-login-body px-6 pb-6 pt-4 md:px-7 md:pb-7 md:pt-3">
          <form @submit.prevent="login" class="auth-login-form">
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
          </form>

          <aside class="auth-login-aside hidden lg:block" aria-label="After sign-in overview">
            <div class="auth-login-summary">
              <div class="auth-panel-kicker">After Sign-In</div>
              <div class="auth-panel-copy auth-login-summary-copy">
                Mission Control, pairing, Library, and Settings stay one click away.
              </div>

              <div class="auth-orbit-field auth-orbit-field--compact auth-login-summary-visual" aria-hidden="true">
                <div class="auth-orbit-ring auth-orbit-ring--outer"></div>
                <div class="auth-orbit-ring auth-orbit-ring--mid"></div>
                <div class="auth-orbit-core"></div>
                <span class="auth-orbit-node auth-orbit-node--primary">Mission Control</span>
                <span class="auth-orbit-node auth-orbit-node--right">Library</span>
                <span class="auth-orbit-node auth-orbit-node--lower-left">Pairing</span>
                <span class="auth-orbit-node auth-orbit-node--lower-right">Settings</span>
              </div>

              <div class="auth-login-summary-note">
                <span>Secure session on trusted devices only.</span>
                <InfoHint align="right" label="Session security">
                  Treat Polaris like a local system console. Sign out or close the browser on shared devices when you are done.
                </InfoHint>
              </div>
            </div>
          </aside>
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

<style scoped>
.auth-login-form {
  display: block;
  max-width: 44rem;
}

.auth-login-header {
  display: block;
  padding: 1.2rem 1.45rem 0.25rem;
  align-items: start;
}

.auth-login-copy {
  max-width: 36rem;
}

.auth-login-copy-text {
  max-width: 23rem;
}

.auth-login-aside {
  width: min(100%, 18rem);
}

.auth-login-summary-copy {
  margin-top: 0.35rem;
  max-width: 18rem;
}

.auth-login-body {
  display: grid;
  gap: 1.6rem;
  align-items: start;
}

.auth-login-summary {
  display: grid;
  gap: 0.65rem;
  min-width: 0;
}

.auth-login-summary-visual {
  min-height: 116px;
  margin-top: 0;
}

.auth-login-summary-note {
  display: flex;
  align-items: center;
  gap: 0.45rem;
  color: var(--theme-text-soft);
  font-size: 0.78rem;
  line-height: 1.45;
}

.auth-orbit-field--compact {
  min-height: 124px;
  margin-top: 0.8rem;
}

.auth-stage--login :deep(.auth-orbit-field--compact .auth-orbit-ring--outer) {
  width: 8.6rem;
  height: 8.6rem;
}

.auth-stage--login :deep(.auth-orbit-field--compact .auth-orbit-ring--mid) {
  width: 5.9rem;
  height: 5.9rem;
}

.auth-stage--login :deep(.auth-orbit-field--compact .auth-orbit-core) {
  width: 3.35rem;
  height: 3.35rem;
}

.auth-stage--login :deep(.auth-orbit-field--compact .auth-orbit-node) {
  padding: 0.28rem 0.56rem;
  font-size: 0.64rem;
}

.auth-stage--login :deep(.auth-orbit-field--compact .auth-orbit-node--primary) {
  top: 0.55rem;
}

.auth-stage--login :deep(.auth-orbit-field--compact .auth-orbit-node--right) {
  right: 0;
  top: 2.45rem;
}

.auth-stage--login :deep(.auth-orbit-field--compact .auth-orbit-node--lower-left) {
  left: 0.55rem;
  bottom: 0.55rem;
}

.auth-stage--login :deep(.auth-orbit-field--compact .auth-orbit-node--lower-right) {
  right: 0.2rem;
  bottom: 0;
}

.auth-login-summary-visual.auth-orbit-field--compact {
  min-height: 116px;
  margin-top: 0;
}

@media (max-width: 1279px) {
  .auth-login-aside {
    width: 100%;
  }
}

@media (min-width: 1024px) {
  .auth-login-body {
    grid-template-columns: minmax(0, 1fr) 18rem;
  }
}
</style>
