<template>
  <div class="surface-subtle p-4 mt-4" :aria-busy="String(saving)">
    <label class="flex items-center gap-3 font-medium text-silver">
      <input
        type="checkbox"
        class="focus-ring"
        :checked="enabled"
        :disabled="disabled || saving || !revision"
        aria-describedby="update-channel-help"
        @change="saveChannel"
      >
      {{ $t('index.include_beta_releases') }}
    </label>
    <p id="update-channel-help" class="mt-2 text-sm text-storm">{{ $t('index.beta_updates_help') }}</p>
    <p class="mt-2 text-sm" role="status">{{ $t(saving ? 'index.saving_update_channel' : enabled ? 'index.stable_and_beta' : 'index.stable_only') }}</p>
    <p v-if="error" class="mt-2 text-sm text-warning" role="alert">{{ $t(error) }}<template v-if="errorDetail"> {{ errorDetail }}</template></p>
  </div>
</template>

<script setup>
import { onUnmounted, ref } from 'vue'
import { clearCachedConfig, settingsRefusalFromBody } from '../config-cache.js'
import { reportSettingsReadable, reportSettingsUnreadable, settingsRefusalSentence } from '../settings-unreadable.js'

const props = defineProps({
  enabled: { type: Boolean, default: false },
  revision: { type: String, default: '' },
  disabled: { type: Boolean, default: false },
})
const emit = defineEmits(['saved', 'refresh', 'busy'])
const saving = ref(false)
const error = ref('')
// The host's reason and fix, after the lead sentence, when it refused its settings file.
const errorDetail = ref('')
let request = null

async function saveChannel(event) {
  const enabled = event.target.checked
  event.target.checked = props.enabled
  if (saving.value || props.disabled || !props.revision) return
  saving.value = true
  error.value = ''
  errorDetail.value = ''
  emit('busy', true)
  const controller = new AbortController()
  request = controller
  try {
    const response = await fetch('./api/config', {
      method: 'PATCH',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json', 'If-Match': `"${props.revision}"` },
      body: JSON.stringify({ notify_pre_releases: enabled ? 'enabled' : 'disabled' }),
      signal: controller.signal,
    })
    const result = await response.json()
    if (controller.signal.aborted) return
    if (!response.ok || result.status !== true) {
      // A refused settings file answers 503 with the reason and the fix (#782), which a retry
      // cannot get past, so the control says them and the banner hears of it.
      const refusal = settingsRefusalFromBody(response.status, result)
      if (refusal) {
        reportSettingsUnreadable(refusal)
        error.value = 'index.update_channel_unreadable'
        errorDetail.value = settingsRefusalSentence('', refusal)
      } else {
        error.value = response.status === 412 ? 'index.update_channel_changed' : 'index.update_channel_failed'
      }
      emit('refresh')
      return
    }
    reportSettingsReadable()
    clearCachedConfig()
    emit('saved', enabled)
    window.dispatchEvent(new Event('polaris-update-channel-changed'))
  } catch {
    if (!controller.signal.aborted) {
      error.value = 'index.update_channel_failed'
      emit('refresh')
    }
  } finally {
    if (request === controller) {
      request = null
      saving.value = false
      emit('busy', false)
    }
  }
}

onUnmounted(() => request?.abort())
</script>
