<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../../PlatformLayout.vue'
import Checkbox from "../../../Checkbox.vue";

const props = defineProps({
  platform: String,
  config: Object
})
const config = ref(props.config)

const REFRESH_RATE_ONLY = "refresh_rate_only"
const RESOLUTION_ONLY = "resolution_only"
const MIXED = "mixed"

function canBeRemapped() {
  return (config.value.dd_resolution_option === "auto" || config.value.dd_refresh_rate_option === "auto")
    && config.value.dd_configuration_option !== "disabled";
}

function getRemappingType() {
  // Assuming here that at least one setting is set to "auto" if other is not
  if (config.value.dd_resolution_option !== "auto") {
    return REFRESH_RATE_ONLY;
  }
  if (config.value.dd_refresh_rate_option !== "auto") {
    return RESOLUTION_ONLY;
  }
  return MIXED;
}

function addRemappingEntry() {
  const type = getRemappingType();
  let template = {};

  if (type !== RESOLUTION_ONLY) {
    template["requested_fps"] = "";
    template["final_refresh_rate"] = "";
  }

  if (type !== REFRESH_RATE_ONLY) {
    template["requested_resolution"] = "";
    template["final_resolution"] = "";
  }

  config.value.dd_mode_remapping[type].push(template);
}
</script>

<template>
  <PlatformLayout :platform="platform">
    <template #windows>
      <details class="mb-3 settings-subtle-surface">
        <summary class="cursor-pointer list-none">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0">
              <div class="section-kicker">Windows virtual display</div>
              <div class="mt-2 text-sm font-medium text-silver">{{ $t('config.dd_options_header') }}</div>
              <div class="mt-1 text-sm text-storm">Use this when Polaris should verify, activate, or fully manage a Windows display target instead of only trusting what the client requested.</div>
            </div>
            <span class="meta-pill shrink-0">Advanced</span>
          </div>
        </summary>

        <div class="mt-4 space-y-3">
          <div class="settings-subtle-surface">
            <svg class="w-5 h-5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
            {{ $t('config.dd_resolution_option_vdisplay_desc') }} {{ $t('config.dd_resolution_option_multi_instance_desc') }}
          </div>

          <div class="mb-3">
            <label for="dd_configuration_option" class="block text-sm font-medium text-storm mb-1">
              {{ $t('config.dd_configuration_option') }}
            </label>
            <select id="dd_configuration_option" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.dd_configuration_option">
              <option value="disabled">{{ $t('_common.disabled_def') }}</option>
              <option value="verify_only">{{ $t('config.dd_config_verify_only') }}</option>
              <option value="ensure_active">{{ $t('config.dd_config_ensure_active') }}</option>
              <option value="ensure_primary">{{ $t('config.dd_config_ensure_primary') }}</option>
              <option value="ensure_only_display">{{ $t('config.dd_config_ensure_only_display') }}</option>
            </select>
          </div>

          <div v-if="config.dd_configuration_option !== 'disabled'" class="settings-form-grid">
            <div>
              <label for="dd_resolution_option" class="block text-sm font-medium text-storm mb-1">
                {{ $t('config.dd_resolution_option') }}
              </label>
              <select id="dd_resolution_option" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.dd_resolution_option">
                <option value="disabled">{{ $t('config.dd_resolution_option_disabled') }}</option>
                <option value="auto">{{ $t('config.dd_resolution_option_auto') }}</option>
                <option value="manual">{{ $t('config.dd_resolution_option_manual') }}</option>
              </select>
              <div class="text-sm text-storm mt-1" v-if="config.dd_resolution_option === 'auto' || config.dd_resolution_option === 'manual'">
                {{ $t('config.dd_resolution_option_ogs_desc') }}
              </div>

              <div class="mt-2" v-if="config.dd_resolution_option === 'manual'">
                <div class="text-sm text-storm mb-1">{{ $t('config.dd_manual_resolution') }}</div>
                <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="dd_manual_resolution" placeholder="2560x1440" v-model="config.dd_manual_resolution" />
              </div>
            </div>

            <div>
              <label for="dd_refresh_rate_option" class="block text-sm font-medium text-storm mb-1">
                {{ $t('config.dd_refresh_rate_option') }}
              </label>
              <select id="dd_refresh_rate_option" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.dd_refresh_rate_option">
                <option value="disabled">{{ $t('config.dd_refresh_rate_option_disabled') }}</option>
                <option value="auto">{{ $t('config.dd_refresh_rate_option_auto') }}</option>
                <option value="manual">{{ $t('config.dd_refresh_rate_option_manual') }}</option>
              </select>

              <div class="mt-2" v-if="config.dd_refresh_rate_option === 'manual'">
                <div class="text-sm text-storm mb-1">{{ $t('config.dd_manual_refresh_rate') }}</div>
                <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="dd_manual_refresh_rate" placeholder="59.9558" v-model="config.dd_manual_refresh_rate" />
              </div>
            </div>
          </div>

          <div class="settings-form-grid" v-if="config.dd_configuration_option !== 'disabled'">
            <div>
              <label for="dd_config_revert_delay" class="block text-sm font-medium text-storm mb-1">
                {{ $t('config.dd_config_revert_delay') }}
              </label>
              <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="dd_config_revert_delay" placeholder="3000" min="0" v-model="config.dd_config_revert_delay" />
              <div class="text-sm text-storm mt-1">
                {{ $t('config.dd_config_revert_delay_desc') }}
              </div>
            </div>

            <div class="flex items-end">
              <Checkbox
                id="dd_config_revert_on_disconnect"
                locale-prefix="config"
                v-model="config.dd_config_revert_on_disconnect"
                default="false"
              ></Checkbox>
            </div>
          </div>

          <div class="mb-3" v-if="canBeRemapped()">
            <label for="dd_mode_remapping" class="block text-sm font-medium text-storm mb-1">
              {{ $t('config.dd_mode_remapping') }}
            </label>
            <div id="dd_mode_remapping" class="text-sm text-storm mt-1">
              {{ $t('config.dd_mode_remapping_desc_1') }}<br>
              {{ $t('config.dd_mode_remapping_desc_2') }}<br>
              {{ $t('config.dd_mode_remapping_desc_3') }}<br>
              {{ $t(getRemappingType() === MIXED ? 'config.dd_mode_remapping_desc_4_final_values_mixed' : 'config.dd_mode_remapping_desc_4_final_values_non_mixed') }}<br>
              <template v-if="getRemappingType() === MIXED">
                {{ $t('config.dd_mode_remapping_desc_5_sops_mixed_only') }}<br>
              </template>
              <template v-if="getRemappingType() === RESOLUTION_ONLY">
                {{ $t('config.dd_mode_remapping_desc_5_sops_resolution_only') }}<br>
              </template>
            </div>

            <div class="overflow-x-auto" v-if="config.dd_mode_remapping[getRemappingType()].length > 0">
              <table class="w-full text-left">
                <thead>
                  <tr>
                    <th scope="col" v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                      {{ $t('config.dd_mode_remapping_requested_resolution') }}
                    </th>
                    <th scope="col" v-if="getRemappingType() !== RESOLUTION_ONLY">
                      {{ $t('config.dd_mode_remapping_requested_fps') }}
                    </th>
                    <th scope="col" v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                      {{ $t('config.dd_mode_remapping_final_resolution') }}
                    </th>
                    <th scope="col" v-if="getRemappingType() !== RESOLUTION_ONLY">
                      {{ $t('config.dd_mode_remapping_final_refresh_rate') }}
                    </th>
                    <th scope="col"></th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="(value, idx) in config.dd_mode_remapping[getRemappingType()]">
                    <td v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="value.requested_resolution" :placeholder="'1920x1080'" />
                    </td>
                    <td v-if="getRemappingType() !== RESOLUTION_ONLY">
                      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="value.requested_fps" :placeholder="'60'" />
                    </td>
                    <td v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="value.final_resolution" :placeholder="'2560x1440'" />
                    </td>
                    <td v-if="getRemappingType() !== RESOLUTION_ONLY">
                      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="value.final_refresh_rate" :placeholder="'119.95'" />
                    </td>
                    <td>
                      <button class="focus-ring rounded-lg border border-red-400/30 bg-red-500/10 px-3 py-1.5 text-red-200 transition hover:bg-red-500/20" @click="config.dd_mode_remapping[getRemappingType()].splice(idx, 1)">
                        <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
                      </button>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
            <button class="focus-ring mt-2 rounded-lg border border-ice/20 bg-ice/10 px-3 py-1.5 text-ice transition hover:bg-ice/20" style="margin: 0 auto" @click="addRemappingEntry()">
              &plus; {{ $t('config.dd_mode_remapping_add') }}
            </button>
          </div>
        </div>
      </details>

      <div class="mb-3">
        <label for="dd_hdr_option" class="block text-sm font-medium text-storm mb-1">
          {{ $t('config.dd_hdr_option') }}
        </label>
        <select id="dd_hdr_option" class="mb-3 w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.dd_hdr_option">
          <option value="disabled">{{ $t('config.dd_hdr_option_disabled') }}</option>
          <option value="auto">{{ $t('config.dd_hdr_option_auto') }}</option>
        </select>
      </div>
    </template>
    <template #linux>
    </template>
    <template #macos>
    </template>
  </PlatformLayout>
</template>
