<template>
  <h1 class="text-2xl font-bold text-silver my-6">{{ $t('password.password_change') }}</h1>
  <form @submit.prevent="save">
    <div class="card p-6 flex flex-col md:flex-row gap-6">
      <div class="flex-1">
        <h4 class="text-lg font-semibold text-silver mb-4">{{ $t('password.current_creds') }}</h4>
        <div class="mb-4">
          <label for="currentUsername" class="block text-sm font-medium text-storm mb-1">{{ $t('_common.username') }}</label>
          <input required type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="currentUsername"
            v-model="passwordData.currentUsername" />
        </div>
        <div class="mb-4">
          <label for="currentPassword" class="block text-sm font-medium text-storm mb-1">{{ $t('_common.password') }}</label>
          <input autocomplete="current-password" type="password" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="currentPassword"
            v-model="passwordData.currentPassword" />
        </div>
      </div>
      <div class="flex-1">
        <h4 class="text-lg font-semibold text-silver mb-4">{{ $t('password.new_creds') }}</h4>
        <div class="mb-4">
          <label for="newUsername" class="block text-sm font-medium text-storm mb-1">{{ $t('_common.username') }}</label>
          <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="newUsername" v-model="passwordData.newUsername" />
          <div class="text-sm text-storm mt-1">{{ $t('password.new_username_desc') }}</div>
        </div>
        <div class="mb-4">
          <label for="newPassword" class="block text-sm font-medium text-storm mb-1">{{ $t('_common.password') }}</label>
          <input autocomplete="new-password" required type="password" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="newPassword"
            v-model="passwordData.newPassword" />
        </div>
        <div class="mb-4">
          <label for="confirmNewPassword" class="block text-sm font-medium text-storm mb-1">{{ $t('password.confirm_password') }}</label>
          <input autocomplete="new-password" required type="password" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="confirmNewPassword"
            v-model="passwordData.confirmNewPassword" />
        </div>
      </div>
    </div>
    <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg my-4" v-if="error"><b>Error: </b>{{error}}</div>
    <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg my-4" v-if="success">
      <b>{{ $t('_common.success') }}</b> {{ $t('password.success_msg') }}
    </div>
    <div class="py-4">
      <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200">{{ $t('_common.save') }}</button>
    </div>
  </form>
</template>

<script setup>
import { ref, reactive } from 'vue'

const error = ref(null)
const success = ref(false)

const passwordData = reactive({
  currentUsername: "",
  currentPassword: "",
  newUsername: "",
  newPassword: "",
  confirmNewPassword: "",
})

function save() {
  if (passwordData.newPassword !== passwordData.confirmNewPassword) {
    error.value = "Password mismatch"
    return
  }
  error.value = null
  fetch("./api/password", {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify(passwordData),
  }).then((r) => {
    if (r.status === 200) {
      r.json().then((rj) => {
        success.value = rj.status
        if (success.value === true) {
          setTimeout(() => {
            document.location.reload()
          }, 5000)
        } else {
          error.value = rj.error
        }
      })
    } else {
      error.value = "Internal Server Error"
    }
  })
}
</script>
