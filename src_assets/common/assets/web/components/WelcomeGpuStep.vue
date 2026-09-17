<template>
  <div>
    <p class="mb-4 text-sm leading-relaxed text-storm">{{ $t('welcome.gpu_intro') }}</p>

    <div v-if="loading" class="rounded-2xl border border-storm/20 bg-void/45 px-4 py-8 text-center text-sm text-storm">
      {{ $t('welcome.gpu_loading') }}
    </div>
    <div v-else-if="!hardware" class="rounded-2xl border border-warning/25 bg-warning/10 px-4 py-3 text-sm text-warning-bright" role="status">
      {{ $t('welcome.gpu_load_failed') }}
    </div>

    <div v-else class="space-y-3">
      <div v-if="!gpus.length" class="surface-subtle p-4 text-sm text-storm">{{ $t('welcome.gpu_none') }}</div>
      <div v-for="gpu in gpus" :key="gpu.render_node" class="surface-subtle min-w-0 p-4" :data-gpu="gpu.render_node">
        <div class="flex flex-wrap items-start justify-between gap-2">
          <div class="min-w-0">
            <div class="break-words text-base font-medium text-silver">{{ gpu.model || $t('welcome.gpu_model_unknown', { vendor: vendorLabel(gpu.vendor) }) }}</div>
            <div class="mt-1 break-words text-xs text-storm">{{ gpuDetail(gpu) }}</div>
          </div>
          <span v-if="gpu.selected" class="shrink-0 rounded-full border border-ice/30 bg-ice/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-ice">
            {{ $t('welcome.gpu_encodes_here') }}
          </span>
        </div>

        <template v-if="gpu.vaapi">
          <div v-if="gpu.vaapi.driver_loaded" class="mt-3 flex flex-wrap gap-2">
            <span
              v-for="codec in codecs"
              :key="codec.key"
              class="rounded-full border px-2.5 py-1 text-[11px] font-medium"
              :class="gpu.vaapi[codec.key] ? 'border-success/30 bg-success/10 text-success-bright' : 'border-danger/30 bg-danger/10 text-danger-bright'"
              :data-codec="codec.key"
            >
              {{ gpu.vaapi[codec.key] ? $t('welcome.gpu_codec_yes', { codec: codec.name }) : $t('welcome.gpu_codec_no', { codec: codec.name }) }}
            </span>
          </div>
          <p v-if="gpu.vaapi.driver_vendor" class="mt-2 break-words text-xs text-storm">{{ $t('welcome.gpu_vaapi_driver', { driver: gpu.vaapi.driver_vendor }) }}</p>
          <p v-if="!gpu.vaapi.driver_loaded" class="mt-2 text-xs text-warning-bright">{{ $t('welcome.gpu_vaapi_not_loaded') }}</p>
        </template>

        <template v-if="gpu.driver === 'nvidia'">
          <p class="mt-2 text-xs text-storm" data-nvenc-floor>
            {{ gpu.driver_version
              ? $t('welcome.gpu_nvidia_floor', { floor: hardware.nvenc_min_driver, version: gpu.driver_version })
              : $t('welcome.gpu_nvidia_floor_unknown', { floor: hardware.nvenc_min_driver }) }}
          </p>
          <p class="mt-1 text-xs text-storm" data-cuda>{{ hardware.build?.cuda ? $t('welcome.gpu_cuda_yes') : $t('welcome.gpu_cuda_no') }}</p>
        </template>
      </div>

      <div class="surface-subtle p-4" data-encoder-summary>
        <div class="text-sm text-storm">{{ $t('welcome.gpu_encoder_label') }}</div>
        <div class="mt-1 text-base font-medium text-silver">{{ encoderLabel(encoderInUse) }}</div>
        <p class="mt-1 text-xs text-storm">{{ hardware.encoder?.active ? $t('welcome.gpu_encoder_state_active') : $t('welcome.gpu_encoder_state_expected') }}</p>
        <p class="mt-2 text-sm leading-relaxed text-storm">{{ $t(reasonKey) }}</p>
        <p v-if="encoderInUse === 'software'" class="mt-2 text-sm leading-relaxed text-warning-bright">{{ $t('welcome.gpu_reason_software') }}</p>
      </div>

      <div
        v-for="item in visibleAdvice"
        :key="`${item.code}:${item.render_node}`"
        class="rounded-2xl border px-4 py-3 text-sm"
        :class="toneFor(item.severity)"
        :data-advice="item.code"
      >
        <div>{{ $t(adviceKey(item.code), adviceParams(item)) }}</div>
        <pre
          v-if="item.commands?.length"
          class="mt-2 overflow-x-auto rounded-xl border border-storm/20 bg-void/70 p-3 font-mono text-xs leading-relaxed text-silver"
        ><code>{{ item.commands.join('\n') }}</code></pre>
      </div>

      <div class="surface-subtle p-4">
        <label for="welcomeEncoder" class="mb-1 block text-sm font-medium text-storm">{{ $t('welcome.gpu_choice_label') }}</label>
        <select id="welcomeEncoder" v-model="choice" class="settings-input text-sm">
          <option value="">{{ $t('welcome.gpu_choice_auto') }}</option>
          <option v-for="encoder in choiceOptions" :key="encoder" :value="encoder">{{ encoderLabel(encoder) }}</option>
        </select>
        <div class="mt-3 flex flex-wrap items-center gap-3">
          <button
            type="button"
            class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 disabled:cursor-not-allowed disabled:opacity-60"
            :disabled="busy || choice === savedEncoder"
            @click="saveEncoder"
          >
            {{ busy ? $t('welcome.gpu_choice_saving') : $t('welcome.gpu_choice_save') }}
          </button>
        </div>
        <p class="mt-2 text-xs text-storm">{{ $t('welcome.gpu_choice_hint') }}</p>
      </div>

      <div
        v-if="outcome"
        class="rounded-2xl border px-4 py-3 text-sm"
        :class="outcome.ok ? 'border-success/25 bg-success/10 text-success-bright' : 'border-danger/25 bg-danger/10 text-danger-bright'"
        role="status"
      >
        {{ $t(outcome.textKey, outcome.params || {}) }}
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, getCurrentInstance, ref, watch } from 'vue'

// Labels are built in script code, so the component keeps its own $t, as the wizard does.
const instance = getCurrentInstance()
const $t = (key, params) => instance?.proxy?.$t?.(key, params) ?? key

const props = defineProps({
  configData: { type: Object, default: null },
  hardware: { type: Object, default: null },
  loading: { type: Boolean, default: false },
  patchConfig: { type: Function, required: true },
})
const emit = defineEmits(['saved'])

const codecs = [
  { key: 'h264', name: 'H.264' },
  { key: 'hevc', name: 'HEVC' },
  { key: 'av1', name: 'AV1' },
]

// Advice codes this page has copy for; a newer host's finding falls back to a generic line.
const knownAdvice = new Set([
  'no_gpu',
  'encoder_gpu_not_found',
  'nouveau_cannot_encode',
  'nvidia_driver_below_floor',
  'nvidia_build_without_cuda',
  'nvenc_not_active',
  'vaapi_not_built',
  'amd_vaapi_driver_missing_fedora',
  'amd_vaapi_driver_missing_fedora_atomic',
  'vaapi_driver_missing',
  'amd_vaapi_encode_missing_fedora',
  'amd_vaapi_encode_missing_fedora_atomic',
  'amd_vaapi_encode_missing',
  'intel_vaapi_encode_missing',
  'vaapi_av1_only',
  'vaapi_hevc_missing',
  'configured_encoder_cannot_run',
])

const reasonKeys = {
  nvidia_nvenc: 'welcome.gpu_reason_nvidia',
  amd_private_vulkan_live_probe: 'welcome.gpu_reason_amd_vulkan',
  amd_private_vulkan_not_built: 'welcome.gpu_reason_amd',
  amd_established_desktop: 'welcome.gpu_reason_amd',
  intel_vaapi: 'welcome.gpu_reason_intel',
  nouveau_availability_probe: 'welcome.gpu_reason_nouveau',
}

const savedEncoder = ref(String(props.configData?.encoder || ''))
const choice = ref(savedEncoder.value)
const busy = ref(false)
const outcome = ref(null)

watch(() => props.configData?.encoder, (encoder) => {
  if (busy.value) return
  savedEncoder.value = String(encoder || '')
})

const gpus = computed(() => (Array.isArray(props.hardware?.gpus) ? props.hardware.gpus : []))
const encoderInUse = computed(() => props.hardware?.encoder?.expected || 'software')

const reasonKey = computed(() => {
  if (savedEncoder.value) return 'welcome.gpu_reason_configured'
  return reasonKeys[props.hardware?.encoder?.policy] || 'welcome.gpu_reason_probe'
})

// The first-stream note is the encoder summary's second line, so it is not repeated as a card.
const visibleAdvice = computed(() => (
  Array.isArray(props.hardware?.advice)
    ? props.hardware.advice.filter((item) => item?.code && item.code !== 'encoder_confirmed_at_first_stream')
    : []
))

const choiceOptions = computed(() => {
  const working = Array.isArray(props.hardware?.encoder_choices) ? props.hardware.encoder_choices : []
  const options = [...working]
  if (savedEncoder.value && !options.includes(savedEncoder.value)) options.push(savedEncoder.value)
  return options
})

function encoderLabel(encoder) {
  const keys = {
    nvenc: 'welcome.encoder_nvenc',
    vaapi: 'welcome.encoder_vaapi',
    vulkan: 'welcome.encoder_vulkan',
    software: 'welcome.encoder_software',
  }
  return keys[encoder] ? $t(keys[encoder]) : String(encoder || '')
}

function vendorLabel(vendor) {
  const keys = {
    amd: 'welcome.vendor_amd',
    intel: 'welcome.vendor_intel',
    nvidia: 'welcome.vendor_nvidia',
  }
  return $t(keys[vendor] || 'welcome.vendor_unknown')
}

function gpuDetail(gpu) {
  const params = {
    vendor: vendorLabel(gpu.vendor),
    driver: gpu.driver || '?',
    version: gpu.driver_version,
    node: gpu.render_node,
  }
  return gpu.driver_version ? $t('welcome.gpu_detail', params) : $t('welcome.gpu_detail_no_version', params)
}

function adviceKey(code) {
  return knownAdvice.has(code) ? `welcome.gpu_advice_${code}` : 'welcome.gpu_advice_unknown'
}

function adviceParams(item) {
  const params = item.params || {}
  return {
    ...params,
    node: item.render_node,
    encoder: encoderLabel(params.encoder),
    vendor: vendorLabel(params.vendor),
  }
}

function toneFor(severity) {
  if (severity === 'fail') return 'border-danger/25 bg-danger/10 text-danger-bright'
  if (severity === 'warning') return 'border-warning/25 bg-warning/10 text-warning-bright'
  return 'border-info/20 bg-info/10 text-info-bright'
}

async function saveEncoder() {
  busy.value = true
  outcome.value = null
  try {
    // An empty value removes the key, which is Automatic.
    const result = await props.patchConfig({ encoder: choice.value })
    if (!result.ok) {
      outcome.value = { ok: false, textKey: 'welcome.gpu_choice_save_failed', params: { error: result.error || '' } }
      return
    }
    savedEncoder.value = choice.value
    outcome.value = {
      ok: true,
      textKey: result.restartRequired === false ? 'welcome.gpu_choice_saved' : 'welcome.gpu_choice_saved_restart',
    }
    emit('saved', result)
  } finally {
    busy.value = false
  }
}
</script>
