<script setup>
import { ref } from 'vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
</script>

<template>
  <div id="files" class="config-page">
    <!-- Apps File -->
    <div class="mb-3">
      <label for="file_apps" class="block text-sm font-medium text-storm mb-1">{{ $t('config.file_apps') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="file_apps" placeholder="apps.json" v-model="config.file_apps" />
      <div class="text-sm text-storm mt-1">{{ $t('config.file_apps_desc') }}</div>
    </div>

    <!-- Credentials File -->
    <div class="mb-3">
      <label for="credentials_file" class="block text-sm font-medium text-storm mb-1">{{ $t('config.credentials_file') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="credentials_file" placeholder="polaris_state.json" v-model="config.credentials_file" />
      <div class="text-sm text-storm mt-1">{{ $t('config.credentials_file_desc') }}</div>
    </div>

    <!-- Log Path -->
    <div class="mb-3">
      <label for="log_path" class="block text-sm font-medium text-storm mb-1">{{ $t('config.log_path') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="log_path" placeholder="polaris.log" v-model="config.log_path" />
      <div class="text-sm text-storm mt-1">{{ $t('config.log_path_desc') }}</div>
    </div>

    <!-- Private Key -->
    <div class="mb-3">
      <label for="pkey" class="block text-sm font-medium text-storm mb-1">{{ $t('config.pkey') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="pkey" placeholder="/dir/pkey.pem" v-model="config.pkey" />
      <div class="text-sm text-storm mt-1">{{ $t('config.pkey_desc') }}</div>
    </div>

    <!-- Certificate -->
    <div class="mb-3">
      <label for="cert" class="block text-sm font-medium text-storm mb-1">{{ $t('config.cert') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="cert" placeholder="/dir/cert.pem" v-model="config.cert" />
      <div class="text-sm text-storm mt-1">{{ $t('config.cert_desc') }}</div>
    </div>

    <!-- State File -->
    <div class="mb-3">
      <label for="file_state" class="block text-sm font-medium text-storm mb-1">{{ $t('config.file_state') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="file_state" placeholder="polaris_state.json"
             v-model="config.file_state" />
      <div class="text-sm text-storm mt-1">{{ $t('config.file_state_desc') }}</div>
    </div>

  </div>
</template>

<style scoped>

</style>
