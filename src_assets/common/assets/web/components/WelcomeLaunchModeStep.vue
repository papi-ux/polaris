<template>
  <div>
    <p class="mb-4 text-sm leading-relaxed text-storm">{{ $t('welcome.launch_mode_intro') }}</p>

    <div v-if="!modes.length" class="rounded-2xl border border-warning/25 bg-warning/10 px-4 py-3 text-sm text-warning-bright" role="status">
      {{ $t('welcome.launch_mode_no_options') }}
    </div>

    <div v-else class="space-y-3">
      <div class="grid gap-2" role="radiogroup" :aria-label="$t('welcome.step_launch_mode')">
        <button
          v-for="mode in modes"
          :key="mode.value"
          type="button"
          role="radio"
          class="w-full min-w-0 rounded-2xl border px-4 py-3 text-left transition-colors disabled:cursor-not-allowed disabled:opacity-60"
          :class="selected === mode.value ? 'border-ice/40 bg-ice/10' : 'border-storm/20 bg-deep/45 hover:border-storm/40'"
          :aria-checked="String(selected === mode.value)"
          :disabled="!mode.selectable"
          :data-mode="mode.value"
          @click="selected = mode.value"
        >
          <span class="flex flex-wrap items-center gap-2">
            <span class="min-w-0 break-words text-sm font-semibold" :class="selected === mode.value ? 'text-ice' : 'text-silver'">{{ $t(mode.titleKey) }}</span>
            <span v-if="mode.value === recommendedMode" class="rounded-full border border-success/30 bg-success/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-success">
              {{ $t('welcome.launch_mode_recommended') }}
            </span>
            <span v-if="mode.value === currentMode" class="rounded-full border border-storm/40 bg-storm/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-storm">
              {{ $t('welcome.launch_mode_current') }}
            </span>
          </span>
          <span class="mt-1 block break-words text-xs leading-relaxed text-storm">{{ $t(mode.copyKey) }}</span>
          <span v-if="!mode.available" class="mt-1 block break-words text-xs leading-relaxed text-warning-bright">
            {{ $t('welcome.launch_mode_unavailable', { reason: mode.unavailableReason }) }}
          </span>
          <span v-else-if="mode.needsSettings" class="mt-1 block text-xs leading-relaxed text-warning-bright">
            {{ $t('welcome.launch_mode_needs_settings') }}
          </span>
        </button>
      </div>

      <p v-if="encodesOnNvidia" class="text-xs text-storm">{{ $t('welcome.launch_mode_nvidia_hint') }}</p>

      <div class="flex flex-wrap items-center gap-3">
        <button
          type="button"
          class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 disabled:cursor-not-allowed disabled:opacity-60"
          :disabled="busy || !selected || selected === currentMode"
          @click="saveMode"
        >
          {{ busy ? $t('welcome.launch_mode_saving') : $t('welcome.launch_mode_save') }}
        </button>
      </div>

      <div
        v-if="outcome"
        class="rounded-2xl border px-4 py-3 text-sm"
        :class="outcome.ok ? 'border-success/25 bg-success/10 text-success-bright' : 'border-danger/25 bg-danger/10 text-danger-bright'"
        role="status"
      >
        {{ $t(outcome.textKey, outcome.params || {}) }}
      </div>

      <p class="text-xs text-storm">{{ $t('welcome.launch_mode_per_game') }}</p>
    </div>
  </div>
</template>

<script setup>
import { computed, getCurrentInstance, ref, watch } from 'vue'
import { applyStreamDisplayModeToConfig, resolveStreamDisplayMode } from '../client-settings-sync.js'

// The wizard's own $t, for the template. Script code keeps message keys only: the mode list is
// first computed during setup, before the app's $t can answer, and a computed would cache
// the untranslated keys.
const instance = getCurrentInstance()
const $t = (key, params) => instance?.proxy?.$t?.(key, params) ?? key

const props = defineProps({
  configData: { type: Object, default: null },
  hardware: { type: Object, default: null },
  patchConfig: { type: Function, required: true },
})
const emit = defineEmits(['saved'])

const recommendedMode = 'headless_stream'

// The keys a mode choice writes, the same set Settings, Audio/Video writes for it, so the
// wizard and Settings never leave different files behind for the same choice.
const modeKeys = [
  'linux_stream_mode',
  'linux_private_runtime',
  'capture',
  'linux_auto_manage_displays',
  'headless_swap_mode',
  'headless_mode',
  'linux_use_cage_compositor',
  'linux_prefer_gpu_native_capture',
]

const currentMode = ref(resolveStreamDisplayMode(props.configData || {}))
const busy = ref(false)
const outcome = ref(null)

const modes = computed(() => {
  const options = Array.isArray(props.configData?.stream_display_mode_options) ? props.configData.stream_display_mode_options : []
  return options
    .filter((option) => option && typeof option.value === 'string' && option.value)
    .map((option) => {
      const available = option.available === true
      // A dummy plug needs its outputs chosen, which only Settings can do.
      const needsSettings = option.value === 'headless_dongle'
      return {
        value: option.value,
        titleKey: `config.av_mode_${option.value}_title`,
        copyKey: `config.av_mode_${option.value}_copy`,
        available,
        needsSettings,
        selectable: available && !needsSettings,
        unavailableReason: String(option.unavailable_reason || ''),
      }
    })
})

function initialSelection() {
  const selectable = (value) => modes.value.some((mode) => mode.value === value && mode.selectable)
  if (selectable(currentMode.value)) return currentMode.value
  if (selectable(recommendedMode)) return recommendedMode
  return ''
}

const selected = ref(initialSelection())

// The host's list can arrive after the step opens; preselect once it does.
watch(modes, () => {
  if (!selected.value && !busy.value) {
    currentMode.value = resolveStreamDisplayMode(props.configData || {})
    selected.value = initialSelection()
  }
})

const encodesOnNvidia = computed(() => (
  Array.isArray(props.hardware?.gpus) && props.hardware.gpus.some((gpu) => gpu?.selected && gpu.vendor === 'nvidia')
))

async function saveMode() {
  const mode = selected.value
  if (!mode) return
  busy.value = true
  outcome.value = null
  try {
    const next = applyStreamDisplayModeToConfig(props.configData || {}, mode)
    const body = Object.fromEntries(modeKeys.map((key) => [key, next[key] ?? '']))
    const result = await props.patchConfig(body)
    if (!result.ok) {
      outcome.value = { ok: false, textKey: 'welcome.launch_mode_save_failed', params: { error: result.error || '' } }
      return
    }
    currentMode.value = mode
    outcome.value = {
      ok: true,
      textKey: result.restartRequired === false ? 'welcome.launch_mode_saved' : 'welcome.launch_mode_saved_restart',
    }
    emit('saved', result)
  } finally {
    busy.value = false
  }
}
</script>
