<template>
  <!-- The host refused its settings file (#782). Settings explains it in place of
       its form; every other page shows this, so the refusal is never silent. -->
  <section v-if="refusal" class="section-card mb-5" role="alert" data-settings-unreadable-banner>
    <div class="section-kicker">{{ $t('config.unreadable_kicker') }}</div>
    <h2 class="section-title">{{ $t('config.unreadable_title') }}</h2>
    <p class="section-copy">{{ $t('config.unreadable_banner_desc') }}</p>
    <dl class="mt-4 grid gap-3">
      <div v-if="refusal.path">
        <dt class="eyebrow-label">{{ $t('config.unreadable_path') }}</dt>
        <dd class="mt-1 break-words font-mono text-xs text-silver" data-settings-unreadable-banner-path>{{ refusal.path }}</dd>
      </div>
      <div>
        <dt class="eyebrow-label">{{ $t('config.unreadable_reason') }}</dt>
        <dd class="mt-1 text-sm leading-relaxed text-storm" data-settings-unreadable-banner-reason>{{ refusal.reason || $t('config.unreadable_reason_unknown') }}</dd>
      </div>
      <div v-if="refusal.fix">
        <dt class="eyebrow-label">{{ $t('config.unreadable_fix') }}</dt>
        <dd class="mt-1 break-words rounded-lg border border-warning/20 bg-warning/10 px-3 py-2 font-mono text-xs text-warning-bright" data-settings-unreadable-banner-fix>{{ refusal.fix }}</dd>
      </div>
    </dl>
    <button
      type="button"
      class="focus-ring settings-action-button settings-action-button-secondary mt-4"
      :disabled="checking"
      data-settings-unreadable-banner-retry
      @click="recheck"
    >
      {{ $t('config.unreadable_retry') }}
    </button>
  </section>
</template>

<script setup>
import { ref } from 'vue'
import { clearCachedConfig, getCachedConfig } from '../config-cache.js'
import { settingsUnreadable as refusal } from '../settings-unreadable.js'

const checking = ref(false)

// Ask the host again. The config cache reports what it hears, so the banner
// goes away when the file reads and stays, with the new reason, when it does not.
async function recheck() {
  if (checking.value) return
  checking.value = true
  try {
    clearCachedConfig()
    await getCachedConfig()
  } catch {
    // The reply was reported; a host that is down leaves the banner as it was.
  } finally {
    checking.value = false
  }
}
</script>
