<script setup>
import { ref } from 'vue'
import InfoHint from '../../components/InfoHint.vue'

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
        <div class="section-title-row">
          <h3 class="settings-section-title">Runtime paths</h3>
          <InfoHint size="sm" label="Catalog and state guidance">
            Point Polaris at the files it uses for app discovery, log capture, and saved runtime state.
          </InfoHint>
        </div>
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="file_apps" class="settings-field-label">{{ $t('config.file_apps') }}</label>
          <InfoHint size="sm" :label="$t('config.file_apps')">{{ $t('config.file_apps_desc') }}</InfoHint>
        </div>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="file_apps" placeholder="apps.json" v-model="config.file_apps" />
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="log_path" class="settings-field-label">{{ $t('config.log_path') }}</label>
          <InfoHint size="sm" :label="$t('config.log_path')">{{ $t('config.log_path_desc') }}</InfoHint>
        </div>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="log_path" placeholder="polaris.log" v-model="config.log_path" />
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="file_state" class="settings-field-label">{{ $t('config.file_state') }}</label>
          <InfoHint size="sm" :label="$t('config.file_state')">{{ $t('config.file_state_desc') }}</InfoHint>
        </div>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="file_state" placeholder="polaris_state.json"
               v-model="config.file_state" />
      </div>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Credentials</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Credentials and TLS</h3>
          <InfoHint size="sm" label="Security material guidance">
            Keep login credentials and TLS material on known paths so pairing and the web UI remain stable across restarts.
          </InfoHint>
        </div>
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="credentials_file" class="settings-field-label">{{ $t('config.credentials_file') }}</label>
          <InfoHint size="sm" :label="$t('config.credentials_file')">{{ $t('config.credentials_file_desc') }}</InfoHint>
        </div>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="credentials_file" placeholder="polaris_state.json" v-model="config.credentials_file" />
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="pkey" class="settings-field-label">{{ $t('config.pkey') }}</label>
          <InfoHint size="sm" :label="$t('config.pkey')">{{ $t('config.pkey_desc') }}</InfoHint>
        </div>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="pkey" placeholder="/dir/pkey.pem" v-model="config.pkey" />
      </div>

      <div class="mb-3">
        <div class="settings-field-head">
          <label for="cert" class="settings-field-label">{{ $t('config.cert') }}</label>
          <InfoHint size="sm" :label="$t('config.cert')">{{ $t('config.cert_desc') }}</InfoHint>
        </div>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="cert" placeholder="/dir/cert.pem" v-model="config.cert" />
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
