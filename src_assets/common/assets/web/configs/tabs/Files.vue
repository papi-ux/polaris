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
    <p class="-mt-3 text-right text-xs text-storm" data-tab-docs-link>
      <a href="https://papi-ux.com/docs/configuration/#files" target="_blank" rel="noopener" class="focus-ring text-ice hover:underline">{{ $t('config.files_docs_link') }}</a>
    </p>
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.files_kicker_catalog_state') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.files_section_runtime_paths') }}</h3>
        </div>
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="file_apps" class="settings-field-label">{{ $t('config.file_apps') }}</label>
        </div>
        <input type="text" class="settings-input" id="file_apps" placeholder="apps.json" v-model="config.file_apps" />
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="log_path" class="settings-field-label">{{ $t('config.log_path') }}</label>
        </div>
        <input type="text" class="settings-input" id="log_path" placeholder="polaris.log" v-model="config.log_path" />
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="file_state" class="settings-field-label">{{ $t('config.file_state') }}</label>
        </div>
        <input type="text" class="settings-input" id="file_state" placeholder="polaris_state.json"
               v-model="config.file_state" />
      </div>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.files_kicker_credentials') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.files_section_credentials_and_tls') }}</h3>
        </div>
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="credentials_file" class="settings-field-label">{{ $t('config.credentials_file') }}</label>
        </div>
        <input type="text" class="settings-input" id="credentials_file" placeholder="polaris_state.json" v-model="config.credentials_file" />
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="pkey" class="settings-field-label">{{ $t('config.pkey') }}</label>
        </div>
        <input type="text" class="settings-input" id="pkey" placeholder="/dir/pkey.pem" v-model="config.pkey" />
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="cert" class="settings-field-label">{{ $t('config.cert') }}</label>
        </div>
        <input type="text" class="settings-input" id="cert" placeholder="/dir/cert.pem" v-model="config.cert" />
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
