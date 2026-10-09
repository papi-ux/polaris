<script setup>
import { computed, toRef } from 'vue'

const props = defineProps([
  'platform',
  'config'
])

const config = toRef(props, 'config')

const POLICIES = ['off', 'manual', 'replay']

function isEnabled(value) {
  if (value === true) return true
  return ['enabled', 'true', '1', 'yes', 'on'].includes(String(value ?? '').toLowerCase().trim())
}

// One host policy over the two existing keys. Choosing a policy only edits settings: it never starts,
// stops or saves a recording, and nothing on this tab calls the recorder.
const policy = computed({
  get() {
    if (!isEnabled(config.value.recording_enabled)) return 'off'
    return isEnabled(config.value.recording_replay_buffer) ? 'replay' : 'manual'
  },
  set(next) {
    if (!POLICIES.includes(next)) return
    config.value.recording_enabled = next === 'off' ? 'disabled' : 'enabled'
    config.value.recording_replay_buffer = next === 'replay' ? 'enabled' : 'disabled'
  },
})
</script>

<template>
  <div id="recording" class="config-page">
    <section class="settings-section" data-recording-experimental>
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.recording_kicker') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.recording_section_policy') }}</h3>
        </div>
      </div>

      <p class="mb-3 text-sm leading-relaxed text-storm" data-recording-format-notice>
        {{ $t('config.recording_format_notice') }}
      </p>

      <div data-setting-key="recording_replay_buffer">
        <fieldset class="mb-3" data-setting-key="recording_enabled" data-recording-policy>
          <legend class="settings-field-label">{{ $t('config.recording_enabled') }}</legend>
          <p class="mb-2 text-sm text-storm">{{ $t('config.recording_enabled_desc') }}</p>
          <label
            v-for="option in POLICIES"
            :key="option"
            :for="`recording_policy_${option}`"
            class="mb-2 flex items-start gap-2"
            :data-recording-policy-option="option"
          >
            <input
              :id="`recording_policy_${option}`"
              type="radio"
              name="recording_policy"
              class="mt-1 shrink-0"
              :value="option"
              v-model="policy"
            />
            <span class="flex min-w-0 flex-col gap-1">
              <span class="text-silver">{{ $t(`config.recording_policy_${option}`) }}</span>
              <span class="text-sm leading-relaxed text-storm">{{ $t(`config.recording_policy_${option}_desc`) }}</span>
            </span>
          </label>
        </fieldset>
      </div>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.recording_kicker_storage') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.recording_section_storage') }}</h3>
        </div>
      </div>

      <div class="mb-3" data-setting-key="recording_replay_buffer_minutes">
        <div class="settings-field-head">
          <label for="recording_replay_buffer_minutes" class="settings-field-label">{{ $t('config.recording_replay_buffer_minutes') }}</label>
        </div>
        <input
          type="number"
          class="settings-input"
          id="recording_replay_buffer_minutes"
          min="1"
          max="60"
          step="1"
          v-model.number="config.recording_replay_buffer_minutes"
        />
        <p class="mt-1 text-sm text-storm">{{ $t('config.recording_replay_buffer_minutes_desc') }}</p>
      </div>

      <div class="mb-3" data-setting-key="recording_output_dir">
        <div class="settings-field-head">
          <label for="recording_output_dir" class="settings-field-label">{{ $t('config.recording_output_dir') }}</label>
        </div>
        <input
          type="text"
          class="settings-input"
          id="recording_output_dir"
          :placeholder="$t('config.recording_output_dir_placeholder')"
          v-model="config.recording_output_dir"
        />
        <p class="mt-1 text-sm text-storm">{{ $t('config.recording_output_dir_desc') }}</p>
      </div>

      <p class="text-sm leading-relaxed text-storm" data-recording-restart-note>
        {{ $t('config.recording_restart_note') }}
      </p>
    </section>
  </div>
</template>
