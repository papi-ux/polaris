<template>
  <div class="page-shell pb-2">
    <section class="page-hero">
      <div class="page-hero-content">
        <div class="page-hero-copy">
          <div class="page-hero-kicker">{{ $t('navbar.security') }}</div>
          <h1 class="page-hero-title">{{ $t('password.action_title') }}</h1>
          <div class="page-hero-copy-inline">
            <p class="page-hero-copy-text">Rotate web UI credentials without leaving the host console.</p>
            <InfoHint size="sm" label="Security overview">
              {{ $t('password.overview') }}
            </InfoHint>
          </div>
          <div class="page-hero-actions">
            <span class="meta-pill">{{ $t('password.web_ui_only') }}</span>
            <span class="meta-pill border-amber-300/30 bg-amber-300/10 text-amber-200">{{ $t('password.reload_note') }}</span>
          </div>
        </div>

        <div class="page-hero-aside">
          <article class="page-hero-note">
            <div class="page-hero-note-title-row">
              <div class="flex items-center gap-2">
                <div class="page-hero-note-title">{{ $t('password.commit_changes') }}</div>
                <span class="meta-pill border-amber-300/25 bg-amber-300/10 text-amber-200">
                  {{ $t('password.sensitive_action') }}
                </span>
              </div>
              <InfoHint size="sm" align="right" label="Reload guidance">
                {{ $t('password.action_desc') }}
              </InfoHint>
            </div>
            <div class="page-hero-note-copy">Verify the current credentials, set the replacement pair, then save once.</div>
            <div class="mt-2 text-sm text-storm">Polaris reloads the web credential gate after the save completes.</div>
          </article>
        </div>
      </div>
    </section>

    <form class="space-y-4" @submit.prevent="save">
      <div class="grid gap-4 xl:grid-cols-[minmax(320px,0.92fr)_minmax(0,1.08fr)]">
        <section class="section-card">
          <div>
            <div class="section-kicker">{{ $t('password.step_one') }}</div>
            <div class="section-title-row">
              <h2 class="section-title">{{ $t('password.verify_access') }}</h2>
              <InfoHint size="sm" label="Current credentials guidance">
                {{ $t('password.current_creds_desc') }}
              </InfoHint>
              <span class="meta-pill border-amber-300/25 bg-amber-300/10 text-amber-200">
                {{ $t('password.sensitive_action') }}
              </span>
            </div>
            <p class="section-copy">Use the current web credentials to authorize the rotation.</p>
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
              <div class="mb-1 flex items-center gap-2">
                <label for="currentPassword" class="block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
                <InfoHint size="sm" label="Current password guidance">
                  {{ $t('password.current_password_desc') }}
                </InfoHint>
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

        <section class="section-card">
          <div>
            <div class="section-kicker">{{ $t('password.step_two') }}</div>
            <div class="section-title-row">
              <h2 class="section-title">{{ $t('password.rotate_access') }}</h2>
              <InfoHint size="sm" label="New credentials guidance">
                {{ $t('password.new_creds_desc') }}
              </InfoHint>
            </div>
            <p class="section-copy">Set the replacement operator identity and confirm it before reload.</p>
          </div>

          <div class="mt-5 grid gap-4 lg:grid-cols-2">
            <div class="lg:col-span-2">
              <div class="mb-1 flex items-center gap-2">
                <label for="newUsername" class="block text-sm font-medium text-storm">{{ $t('_common.username') }}</label>
                <InfoHint size="sm" label="Username guidance">
                  {{ $t('password.new_username_desc') }}
                </InfoHint>
              </div>
              <input
                id="newUsername"
                v-model="passwordData.newUsername"
                type="text"
                class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
              />
            </div>

            <div>
              <div class="mb-1 flex items-center gap-2">
                <label for="newPassword" class="block text-sm font-medium text-storm">{{ $t('_common.password') }}</label>
                <InfoHint size="sm" label="New password guidance">
                  {{ $t('password.new_password_desc') }}
                </InfoHint>
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

      <section class="section-card border border-amber-300/20 bg-amber-300/5">
        <div class="flex flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
          <div class="min-w-0">
            <div class="section-kicker">{{ $t('password.commit_changes') }}</div>
            <div class="section-title-row">
              <div class="text-sm font-medium text-silver">{{ $t('password.save_and_reload') }}</div>
              <InfoHint size="sm" label="Commit changes guidance">
                {{ $t('password.action_desc') }}
              </InfoHint>
            </div>
            <div class="mt-1 text-sm text-storm">Apply the new credentials and let Polaris reload the web gate.</div>
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
import InfoHint from '../components/InfoHint.vue'

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
