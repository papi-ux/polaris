<template>
  <div class="min-h-screen bg-void flex items-center justify-center p-4">
    <div class="glass rounded-2xl p-8 w-full max-w-md shadow-2xl animate-[scale-in_0.3s_ease-out]">
      <img src="/images/logo-polaris.svg" class="mx-auto mb-6 h-12" alt="Polaris">
      <h1 class="text-2xl font-bold text-silver text-center mb-6">{{ $t('welcome.greeting') }}</h1>
      <form @submit.prevent="login" class="space-y-4">
        <div>
          <label for="usernameInput" class="block text-sm font-medium text-storm mb-1">{{ $t('_common.username') }}</label>
          <input type="text" class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2.5 text-silver focus:border-ice focus:outline-none focus:shadow-[0_0_12px_rgba(200,214,229,0.1)] transition-all" id="usernameInput" autocomplete="username"
            v-model="passwordData.username" required autofocus/>
        </div>
        <div>
          <label for="passwordInput" class="block text-sm font-medium text-storm mb-1">{{ $t('_common.password') }}</label>
          <input type="password" class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2.5 text-silver focus:border-ice focus:outline-none focus:shadow-[0_0_12px_rgba(200,214,229,0.1)] transition-all" id="passwordInput" autocomplete="password"
            v-model="passwordData.password" required />
        </div>
        <div class="flex items-center gap-2">
          <input type="checkbox" class="w-4 h-4 rounded border-storm bg-void text-ice focus:ring-ice accent-ice" id="savePassword" v-model="savePassword"/>
          <label for="savePassword" class="text-sm text-storm">{{ $t('login.save_password') }}</label>
        </div>
        <button type="submit" class="w-full h-10 bg-ice text-void rounded-lg font-semibold hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.25)] transition-all duration-200 disabled:opacity-50" v-bind:disabled="loading">
          {{ $t('welcome.login') }}
        </button>
        <div class="text-center text-sm text-storm">
          {{ $t('login.recovery_hint') }}
          <router-link to="/recover" class="text-ice hover:text-ice/80 transition-colors no-underline">
            {{ $t('login.forgot_password') }}
          </router-link>
        </div>
        <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg" v-if="error">
          <b>{{ $t('_common.error') }}</b> {{error}}
        </div>
        <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg" v-if="success">
          <b>{{ $t('_common.success') }}</b> {{ $t('welcome.login_success') }}
        </div>
      </form>
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
