import { readonly, ref } from 'vue'
import { clearCachedConfig, primeCachedConfig } from './config-cache.js'
import { forgetSettingsRefusal } from './settings-unreadable.js'

const authenticated = ref(false)

export const webUiAuthenticated = readonly(authenticated)

function publishAuthenticationState(value) {
  if (typeof window !== 'undefined') {
    window.__POLARIS_AUTHENTICATED__ = value
  }
}

export function initializeWebUiAuthState() {
  authenticated.value = false
  publishAuthenticationState(false)
  clearCachedConfig()
  forgetSettingsRefusal()
}

export function isWebUiAuthenticated() {
  return authenticated.value
}

export function markWebUiAuthenticated(config) {
  primeCachedConfig(config)
  authenticated.value = true
  publishAuthenticationState(true)
}

export function markWebUiUnauthenticated() {
  authenticated.value = false
  publishAuthenticationState(false)
  clearCachedConfig()
  forgetSettingsRefusal()
}
