<script setup>
import { computed, inject } from 'vue'
import { describePyroWave } from '../../../codec-support-readout.js'
import PyroWaveStreamReadout from './PyroWaveStreamReadout.vue'

const props = defineProps([
  'config',
  'platform',
])

const i18n = inject('i18n', null)
const t = (key, params) => (i18n ? i18n.t(key, params) : key)

// PyroWave is chosen per stream by the client, whichever encoder profile is open here, so the card
// sits above the profile switcher. Off Linux it stays away: the host's not_built message would name
// a release that does not exist there. A host that predates the report renders nothing.
const pyrowave = computed(() => (props.platform === 'linux' ? describePyroWave(t, props.config?.encoder_codec_support) : null))
</script>

<template>
  <section v-if="pyrowave" id="pyrowave" class="section-card" data-setting-key="pyrowave" data-pyrowave-card>
    <div class="flex flex-col gap-2 sm:flex-row sm:items-start sm:justify-between">
      <div class="min-w-0">
        <div class="section-kicker">{{ $t('config.pyrowave_card_kicker') }}</div>
        <h3 class="section-title">{{ $t('config.codec_support_pyrowave') }}</h3>
      </div>
      <div class="flex flex-wrap items-center gap-1.5" data-codec-row="pyrowave">
        <span v-if="pyrowave.available" class="meta-pill border-info/30 bg-info/10 text-info-bright">{{ $t('config.codec_support_yuv444') }}</span>
        <span v-if="pyrowave.hdr" class="meta-pill border-info/30 bg-info/10 text-info-bright">{{ $t('config.codec_support_hdr') }}</span>
        <span class="meta-pill" :class="pyrowave.available ? 'border-success/30 bg-success/10 text-success' : ''" data-codec-status>
          {{ pyrowave.available ? $t('config.codec_support_available') : $t('config.codec_support_not_available') }}
        </span>
      </div>
    </div>

    <div class="mt-2 flex flex-col gap-1" data-codec-detail="pyrowave">
      <p v-for="line in pyrowave.lines" :key="line" class="text-sm leading-relaxed text-support">{{ line }}</p>
      <!-- Polaris only reports here; Nova picks the codec and the bitrate for each stream. -->
      <p class="text-sm leading-relaxed text-support" data-pyrowave-selection>{{ $t('config.pyrowave_card_selection') }}</p>
      <p v-if="pyrowave.message" class="text-sm leading-relaxed text-silver" data-pyrowave-message>{{ pyrowave.message }}</p>
      <p v-if="pyrowave.refusal" class="text-sm leading-relaxed text-warning-bright" data-pyrowave-refusal>
        {{ pyrowave.refusal.lead }} {{ pyrowave.refusal.message }} {{ pyrowave.refusal.action }}
      </p>
      <details v-if="pyrowave.reason" class="mt-1">
        <summary class="cursor-pointer text-xs text-ice">{{ $t('config.pyrowave_card_reason_details') }}</summary>
        <p class="mt-1 text-xs leading-relaxed text-storm" data-pyrowave-reason>
          {{ $t('config.codec_support_pyrowave_reason') }} <code>{{ pyrowave.reason }}</code>
        </p>
      </details>
    </div>

    <PyroWaveStreamReadout v-if="pyrowave.available" />
  </section>
</template>
