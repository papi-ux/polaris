<template>
  <div class="my-6 space-y-6">
    <div class="flex flex-col gap-3 lg:flex-row lg:items-end lg:justify-between">
      <div>
        <h1 class="text-2xl font-bold text-silver">{{ $t('navbar.security') }}</h1>
        <p class="mt-2 max-w-3xl text-sm text-storm">{{ $t('password.overview') }}</p>
      </div>
      <div class="flex flex-wrap items-center gap-2 text-xs">
        <span class="rounded-full border border-storm/30 bg-deep/60 px-2.5 py-1 text-storm">
          {{ $t('password.web_ui_only') }}
        </span>
        <span class="rounded-full border border-amber-300/30 bg-amber-300/10 px-2.5 py-1 text-amber-200">
          {{ $t('password.reload_note') }}
        </span>
      </div>
    </div>

    <form class="space-y-4" @submit.prevent="save">
      <div class="grid gap-4 xl:grid-cols-[minmax(320px,0.9fr)_minmax(0,1.35fr)]">
        <section class="card border border-storm/15 p-5">
          <div class="flex items-start justify-between gap-4">
            <div>
              <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm">{{ $t('password.step_one') }}</div>
              <h2 class="mt-2 text-lg font-semibold text-silver">{{ $t('password.verify_access') }}</h2>
              <p class="mt-2 text-sm text-storm">{{ $t('password.current_creds_desc') }}</p>
            </div>
            <span class="rounded-full border border-amber-300/25 bg-amber-300/10 px-2 py-1 text-[10px] font-medium text-amber-200">
              {{ $t('password.sensitive_action') }}
            </span>
          </div>

          <div class="mt-5 space-y-4">
            <div>
              <label for="currentUsername" class="mb-1 block text-sm font-medium text-storm">{{ $t('_common.username') }}</label>
              <input
                id="currentUsername"
                v-model="passwordData.currentUsername"
                required
                type="text"
                class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
              />
            </div>

            <div>
              <div class="mb-1 flex items-center justify-between gap-3">
                <label for="currentPassword" class="block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
                <span class="text-[11px] text-storm">{{ $t('password.current_password_desc') }}</span>
              </div>
              <div class="relative">
                <input
                  id="currentPassword"
                  v-model="passwordData.currentPassword"
                  autocomplete="current-password"
                  required
                  :type="showCurrentPassword ? 'text' : 'password'"
                  class="w-full rounded-lg border border-storm bg-deep px-3 py-2 pr-20 text-silver focus:border-ice focus:outline-none"
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

        <section class="card border border-storm/15 p-5">
          <div>
            <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm">{{ $t('password.step_two') }}</div>
            <h2 class="mt-2 text-lg font-semibold text-silver">{{ $t('password.rotate_access') }}</h2>
            <p class="mt-2 text-sm text-storm">{{ $t('password.new_creds_desc') }}</p>
          </div>

          <div class="mt-5 grid gap-4 lg:grid-cols-2">
            <div class="lg:col-span-2">
              <label for="newUsername" class="mb-1 block text-sm font-medium text-storm">{{ $t('_common.username') }}</label>
              <input
                id="newUsername"
                v-model="passwordData.newUsername"
                type="text"
                class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
              />
              <div class="mt-1 text-sm text-storm">{{ $t('password.new_username_desc') }}</div>
            </div>

            <div>
              <div class="mb-1 flex items-center justify-between gap-3">
                <label for="newPassword" class="block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
                <span class="text-[11px] text-storm">{{ $t('password.new_password_desc') }}</span>
              </div>
              <div class="relative">
                <input
                  id="newPassword"
                  v-model="passwordData.newPassword"
                  autocomplete="new-password"
                  required
                  :type="showNewPassword ? 'text' : 'password'"
                  class="w-full rounded-lg border border-storm bg-deep px-3 py-2 pr-20 text-silver focus:border-ice focus:outline-none"
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
                  class="w-full rounded-lg border bg-deep px-3 py-2 pr-20 text-silver focus:border-ice focus:outline-none"
                  :class="passwordMismatch ? 'border-red-500/70' : 'border-storm'"
                />
                <button
                  type="button"
                  class="absolute inset-y-0 right-2 my-auto h-7 rounded-md px-2 text-xs font-medium text-storm transition-colors hover:text-ice"
                  @click="showConfirmPassword = !showConfirmPassword"
                >
                  {{ showConfirmPassword ? $t('password.hide') : $t('password.show') }}
                </button>
              </div>
              <div v-if="passwordMismatch" class="mt-1 text-sm text-red-400">{{ $t('password.password_mismatch') }}</div>
            </div>
          </div>
        </section>
      </div>

      <section class="card border border-amber-300/20 bg-amber-300/5 p-4">
        <div class="flex flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
          <div class="min-w-0">
            <div class="flex flex-wrap items-center gap-2">
              <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm">{{ $t('password.commit_changes') }}</div>
              <span class="rounded-full border border-amber-300/25 bg-amber-300/10 px-2 py-1 text-[10px] font-medium text-amber-200">
                {{ $t('password.sensitive_action') }}
              </span>
            </div>
            <div class="mt-1 text-sm font-medium text-silver">{{ $t('password.action_title') }}</div>
            <div class="mt-1 text-sm text-storm">{{ $t('password.action_desc') }}</div>
          </div>
          <button
            class="inline-flex h-9 items-center justify-center gap-2 rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="!canSave"
          >
            {{ saving ? $t('password.saving') : $t('password.save_and_reload') }}
          </button>
        </div>

        <div v-if="error" class="mt-4 rounded-lg border-l-4 border-red-500 bg-twilight/50 p-3 text-silver">
          <b>{{ $t('_common.error') }}</b> {{ error }}
        </div>
        <div v-if="success" class="mt-4 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver">
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
