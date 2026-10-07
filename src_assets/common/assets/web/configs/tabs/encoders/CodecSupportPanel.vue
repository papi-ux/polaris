<script setup>
import { computed, inject } from 'vue'
import { describeYuv444 } from '../../../codec-support-readout.js'

const props = defineProps([
  'config',
])

const i18n = inject('i18n', null)
const t = (key, params) => (i18n ? i18n.t(key, params) : key)

// Read-only capability snapshot served by GET /api/config (response-only key).
// Absent on hosts that predate it, in which case the panel renders nothing.
const support = computed(() => props.config?.encoder_codec_support ?? null)
const ready = computed(() => !!support.value && support.value.ready === true)
const activeEncoder = computed(() => (ready.value ? String(support.value.encoder || '').trim() : ''))

// Why a codec is not advertised, as a locale key. The host reports
// "disabled_in_config" when the user switched it off and
// "not_available_on_encoder" once probing found the encoder cannot do it;
// anything else (including null while probing) renders no reason line.
const reasonKey = (reason, supported) => {
  if (!ready.value || supported || !reason) return ''
  return reason === 'disabled_in_config' ? 'config.codec_support_reason_disabled' : 'config.codec_support_reason_unavailable'
}
const hevcReasonKey = computed(() => reasonKey(support.value?.hevc_reason, support.value?.hevc_supported))
const av1ReasonKey = computed(() => reasonKey(support.value?.av1_reason, support.value?.av1_supported))

// 4:4:4 follows the probe, so it waits for it like the other codec rows. PyroWave has its own card
// above the encoder switcher (PyroWaveCard.vue): a client chooses it per stream, whichever profile
// is open here, so it is not repeated in each profile.
const yuv444 = computed(() => (ready.value ? describeYuv444(t, support.value) : null))

const YUV444_PILLS = {
  supported: { key: 'config.codec_support_supported', tone: 'border-success/30 bg-success/10 text-success' },
  pyrowave_only: { key: 'config.codec_support_yuv444_pyrowave_only', tone: 'border-info/30 bg-info/10 text-info-bright' },
  unsupported: { key: 'config.codec_support_unsupported', tone: '' },
}
const yuv444Pill = computed(() => (yuv444.value ? YUV444_PILLS[yuv444.value.status] : null))
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

        <p v-if="hevcReasonKey" class="text-xs leading-relaxed text-storm">{{ $t(hevcReasonKey) }}</p>

        <div class="flex items-center justify-between gap-3">
          <span class="text-sm font-medium text-silver">{{ $t('config.codec_support_av1') }}</span>
          <div class="flex flex-wrap items-center gap-1.5">
            <span v-if="support.av1_hdr" class="meta-pill border-info/30 bg-info/10 text-info-bright">{{ $t('config.codec_support_hdr') }}</span>
            <span class="meta-pill" :class="support.av1_supported ? 'border-success/30 bg-success/10 text-success' : ''">
              {{ support.av1_supported ? $t('config.codec_support_supported') : $t('config.codec_support_unsupported') }}
            </span>
          </div>
        </div>

        <p v-if="av1ReasonKey" class="text-xs leading-relaxed text-storm">{{ $t(av1ReasonKey) }}</p>

        <template v-if="yuv444">
          <div class="flex items-center justify-between gap-3" data-codec-row="yuv444">
            <span class="text-sm font-medium text-silver">{{ $t('config.codec_support_yuv444') }}</span>
            <span class="meta-pill" :class="yuv444Pill.tone" data-codec-status>{{ $t(yuv444Pill.key) }}</span>
          </div>
          <div class="flex flex-col gap-1" data-codec-detail="yuv444">
            <p v-for="line in yuv444.lines" :key="line" class="text-xs leading-relaxed text-storm">{{ line }}</p>
          </div>
        </template>
      </div>

      <p class="mt-3 text-sm leading-relaxed text-storm">{{ $t('config.codec_support_note') }}</p>
    </template>
  </div>
</template>

<style scoped>
</style>
