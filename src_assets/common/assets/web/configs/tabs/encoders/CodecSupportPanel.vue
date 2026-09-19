<script setup>
import { computed } from 'vue'

const props = defineProps([
  'config',
])

// Read-only capability snapshot served by GET /api/config (response-only key).
// Absent on hosts that predate it, in which case the panel renders nothing.
const support = computed(() => props.config?.encoder_codec_support ?? null)
const ready = computed(() => !!support.value && support.value.ready === true)
const activeEncoder = computed(() => (ready.value ? String(support.value.encoder || '').trim() : ''))
</script>

<template>
  <div v-if="support" class="surface-subtle mb-4 p-4">
    <div class="eyebrow-label mb-2">{{ $t('config.codec_support_title') }}</div>

    <p v-if="!ready" class="text-sm leading-relaxed text-storm">
      {{ $t('config.codec_support_probing') }}
    </p>

    <template v-else>
      <div class="flex flex-col gap-2">
        <div v-if="activeEncoder" class="flex items-center justify-between gap-3">
          <span class="text-sm font-medium text-silver">{{ $t('config.codec_support_encoder') }}</span>
          <span class="meta-pill border-info/30 bg-info/10 text-info-bright">{{ activeEncoder }}</span>
        </div>

        <div class="flex items-center justify-between gap-3">
          <span class="text-sm font-medium text-silver">{{ $t('config.codec_support_h264') }}</span>
          <span class="meta-pill border-success/30 bg-success/10 text-success">{{ $t('config.codec_support_supported') }}</span>
        </div>

        <div class="flex items-center justify-between gap-3">
          <span class="text-sm font-medium text-silver">{{ $t('config.codec_support_hevc') }}</span>
          <div class="flex flex-wrap items-center gap-1.5">
            <span v-if="support.hevc_hdr" class="meta-pill border-info/30 bg-info/10 text-info-bright">{{ $t('config.codec_support_hdr') }}</span>
            <span class="meta-pill" :class="support.hevc_supported ? 'border-success/30 bg-success/10 text-success' : ''">
              {{ support.hevc_supported ? $t('config.codec_support_supported') : $t('config.codec_support_unsupported') }}
            </span>
          </div>
        </div>

        <div class="flex items-center justify-between gap-3">
          <span class="text-sm font-medium text-silver">{{ $t('config.codec_support_av1') }}</span>
          <div class="flex flex-wrap items-center gap-1.5">
            <span v-if="support.av1_hdr" class="meta-pill border-info/30 bg-info/10 text-info-bright">{{ $t('config.codec_support_hdr') }}</span>
            <span class="meta-pill" :class="support.av1_supported ? 'border-success/30 bg-success/10 text-success' : ''">
              {{ support.av1_supported ? $t('config.codec_support_supported') : $t('config.codec_support_unsupported') }}
            </span>
          </div>
        </div>
      </div>

      <p class="mt-3 text-sm leading-relaxed text-storm">{{ $t('config.codec_support_note') }}</p>
    </template>
  </div>
</template>

<style scoped>
</style>
