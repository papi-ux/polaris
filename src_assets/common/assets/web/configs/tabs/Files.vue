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
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Catalog & state</div>
        <h3 class="settings-section-title">App and runtime files</h3>
        <p class="settings-section-copy">Point Polaris at the files it uses for app discovery, log capture, and saved runtime state.</p>
      </div>

      <div class="mb-3">
        <label for="file_apps" class="block text-sm font-medium text-storm mb-1">{{ $t('config.file_apps') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="file_apps" placeholder="apps.json" v-model="config.file_apps" />
        <div class="text-sm text-storm mt-1">{{ $t('config.file_apps_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="log_path" class="block text-sm font-medium text-storm mb-1">{{ $t('config.log_path') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="log_path" placeholder="polaris.log" v-model="config.log_path" />
        <div class="text-sm text-storm mt-1">{{ $t('config.log_path_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="file_state" class="block text-sm font-medium text-storm mb-1">{{ $t('config.file_state') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="file_state" placeholder="polaris_state.json"
               v-model="config.file_state" />
        <div class="text-sm text-storm mt-1">{{ $t('config.file_state_desc') }}</div>
      </div>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Credentials</div>
        <h3 class="settings-section-title">Security material</h3>
        <p class="settings-section-copy">Keep login credentials and TLS material on known paths so pairing and the web UI remain stable across restarts.</p>
      </div>

      <div class="mb-3">
        <label for="credentials_file" class="block text-sm font-medium text-storm mb-1">{{ $t('config.credentials_file') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="credentials_file" placeholder="polaris_state.json" v-model="config.credentials_file" />
        <div class="text-sm text-storm mt-1">{{ $t('config.credentials_file_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="pkey" class="block text-sm font-medium text-storm mb-1">{{ $t('config.pkey') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="pkey" placeholder="/dir/pkey.pem" v-model="config.pkey" />
        <div class="text-sm text-storm mt-1">{{ $t('config.pkey_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="cert" class="block text-sm font-medium text-storm mb-1">{{ $t('config.cert') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="cert" placeholder="/dir/cert.pem" v-model="config.cert" />
        <div class="text-sm text-storm mt-1">{{ $t('config.cert_desc') }}</div>
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
