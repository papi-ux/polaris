<template>
  <div class="page-shell pb-2">
    <section class="page-header security-page-header">
      <div class="page-heading">
        <div class="section-kicker">{{ $t('navbar.security') }}</div>
        <h1 class="page-title">{{ $t('password.page_title') }}</h1>
        <p class="page-subtitle">
          {{ $t('password.page_subtitle') }}
          <a href="https://papi-ux.com/docs/configuration/#rotate-the-web-credentials" target="_blank" rel="noopener" class="focus-ring text-ice hover:underline">{{ $t('password.docs_link') }}</a>
        </p>
        <div class="page-meta">
          <span class="meta-pill">{{ $t('password.web_ui_only') }}</span>
          <span class="meta-pill border-warning/30 bg-warning/10 text-warning-bright">{{ $t('password.reloads_on_save') }}</span>
        </div>
      </div>

      <article class="header-support-card security-header-card">
        <div class="header-support-title-row">
          <div class="flex items-center gap-2">
            <div class="section-kicker !mb-0">{{ $t('password.commit') }}</div>
            <span class="meta-pill border-warning/25 bg-warning/10 text-warning-bright">
              {{ $t('password.sensitive_action') }}
            </span>
          </div>
        </div>
        <div class="header-support-copy">{{ $t('password.commit_copy') }}</div>
      </article>
    </section>

    <form class="space-y-4" @submit.prevent="save">
      <div class="grid gap-4 xl:grid-cols-[minmax(320px,0.92fr)_minmax(0,1.08fr)]">
        <section class="section-card">
          <div>
            <div class="section-kicker">{{ $t('password.step_one') }}</div>
            <div class="section-title-row">
              <h2 class="section-title">{{ $t('password.verify_access') }}</h2>
              <span class="meta-pill border-warning/25 bg-warning/10 text-warning-bright">
                {{ $t('password.sensitive_action') }}
              </span>
            </div>
            <p class="section-copy">{{ $t('password.verify_access_copy') }}</p>
          </div>

          <div class="mt-5 space-y-4">
            <div>
              <label for="currentUsername" class="mb-1 block text-sm font-medium text-storm">{{ $t('_common.username') }}</label>
              <input
                id="currentUsername"
                v-model="passwordData.currentUsername"
                required
                type="text"
                class="settings-input"
              />
            </div>

            <div>
              <div class="mb-1 flex items-center gap-2">
                <label for="currentPassword" class="block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
              </div>
              <div class="relative">
                <input
                  id="currentPassword"
                  v-model="passwordData.currentPassword"
                  autocomplete="current-password"
                  required
                  :type="showCurrentPassword ? 'text' : 'password'"
                  class="settings-input pr-20"
                />
                <button
                  type="button"
                  class="absolute inset-y-0 right-2 my-auto h-7 rounded-md px-2 text-xs font-medium text-storm transition-colors hover:text-ice"
                  @click="showCurrentPassword = !showCurrentPassword"
                >
                  {{ showCurrentPassword ? $t('password.hide') : $t('password.show') }}
                </button>
              </div>
            </div>
          </div>
        </section>

        <section class="section-card">
          <div>
            <div class="section-kicker">{{ $t('password.step_two') }}</div>
            <div class="section-title-row">
              <h2 class="section-title">{{ $t('password.rotate_access') }}</h2>
            </div>
            <p class="section-copy">{{ $t('password.rotate_access_copy') }}</p>
          </div>

          <div class="mt-5 grid gap-4 lg:grid-cols-2">
            <div class="lg:col-span-2">
              <div class="mb-1 flex items-center gap-2">
                <label for="newUsername" class="block text-sm font-medium text-storm">{{ $t('_common.username') }}</label>
              </div>
              <input
                id="newUsername"
                v-model="passwordData.newUsername"
                type="text"
                class="settings-input"
              />
            </div>

            <div>
              <div class="mb-1 flex items-center gap-2">
                <label for="newPassword" class="block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
              </div>
              <div class="relative">
                <input
                  id="newPassword"
                  v-model="passwordData.newPassword"
                  autocomplete="new-password"
                  required
                  :type="showNewPassword ? 'text' : 'password'"
                  class="settings-input pr-20"
                />
                <button
                  type="button"
                  class="absolute inset-y-0 right-2 my-auto h-7 rounded-md px-2 text-xs font-medium text-storm transition-colors hover:text-ice"
                  @click="showNewPassword = !showNewPassword"
                >
                  {{ showNewPassword ? $t('password.hide') : $t('password.show') }}
                </button>
              </div>
            </div>

            <div>
              <label for="confirmNewPassword" class="mb-1 block text-sm font-medium text-storm">{{ $t('password.confirm_password') }}</label>
              <div class="relative">
                <input
                  id="confirmNewPassword"
                  v-model="passwordData.confirmNewPassword"
                  autocomplete="new-password"
                  required
                  :type="showConfirmPassword ? 'text' : 'password'"
                  class="settings-input pr-20"
                  :class="passwordMismatch ? '!border-danger/70' : ''"
                />
                <button
                  type="button"
                  class="absolute inset-y-0 right-2 my-auto h-7 rounded-md px-2 text-xs font-medium text-storm transition-colors hover:text-ice"
                  @click="showConfirmPassword = !showConfirmPassword"
                >
                  {{ showConfirmPassword ? $t('password.hide') : $t('password.show') }}
                </button>
              </div>
              <div v-if="passwordMismatch" class="mt-1 text-sm text-danger">{{ $t('password.password_mismatch') }}</div>
            </div>
          </div>
        </section>
      </div>

      <section class="section-card border border-warning/20 bg-warning/5">
        <div class="flex flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
          <div class="min-w-0">
            <div class="section-kicker">{{ $t('password.commit_changes') }}</div>
            <div class="section-title-row">
              <div class="text-sm font-medium text-silver">{{ $t('password.save_and_reload') }}</div>
            </div>
            <div class="mt-1 text-sm text-storm">{{ $t('password.save_and_reload_copy') }}</div>
          </div>
          <button
            class="focus-ring dashboard-action-button dashboard-action-button-primary"
            :disabled="!canSave"
          >
            {{ saving ? $t('password.saving') : $t('password.save_and_reload') }}
          </button>
        </div>

        <div v-if="error" class="mt-4 rounded-lg border-l-4 border-danger bg-twilight/50 p-3 text-silver">
          <b>{{ $t('_common.error') }}</b> {{ error }}
        </div>
        <div v-if="success" class="mt-4 rounded-lg border-l-4 border-success bg-twilight/50 p-3 text-silver">
          <b>{{ $t('_common.success') }}</b> {{ $t('password.success_msg') }}
        </div>
      </section>
    </form>
  </div>
</template>

<script setup>
import { computed, reactive, ref, watch } from 'vue'

const error = ref(null)
const success = ref(false)
const saving = ref(false)
const showCurrentPassword = ref(false)
const showNewPassword = ref(false)
const showConfirmPassword = ref(false)

const passwordData = reactive({
  currentUsername: "",
  currentPassword: "",
  newUsername: "",
  newPassword: "",
  confirmNewPassword: "",
})

const passwordMismatch = computed(() => {
  return passwordData.newPassword.length > 0
    && passwordData.confirmNewPassword.length > 0
    && passwordData.newPassword !== passwordData.confirmNewPassword
})

const canSave = computed(() => {
  return !saving.value
    && passwordData.currentUsername.trim().length > 0
    && passwordData.currentPassword.length > 0
    && passwordData.newPassword.length > 0
    && passwordData.confirmNewPassword.length > 0
    && !passwordMismatch.value
})

watch(
  () => [
    passwordData.currentUsername,
    passwordData.currentPassword,
    passwordData.newUsername,
    passwordData.newPassword,
    passwordData.confirmNewPassword,
  ],
  () => {
    error.value = null
    if (!saving.value) {
      success.value = false
    }
  }
)

async function save() {
  if (!canSave.value) {
    if (passwordMismatch.value) {
      error.value = "Password mismatch"
    }
    return
  }

  error.value = null
  saving.value = true

  try {
    const response = await fetch("./api/password", {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify(passwordData),
    })

    if (response.status !== 200) {
      error.value = "Internal Server Error"
      return
    }

    const payload = await response.json()
    success.value = payload.status

    if (success.value === true) {
      setTimeout(() => {
        document.location.reload()
      }, 5000)
    } else {
      error.value = payload.error
    }
  } catch (e) {
    error.value = "Internal Server Error"
  } finally {
    saving.value = false
  }
}
</script>
