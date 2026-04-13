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
      <details class="mb-3 bg-deep rounded-xl border border-storm">
        <summary class="px-4 py-3 cursor-pointer text-silver font-medium hover:text-ice transition-colors">
              {{ $t('config.dd_options_header') }}
        </summary>
        <div class="px-4 pb-4">
              <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-3 rounded-lg" v-if="platform === 'windows'">
                <svg class="w-5 h-5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg> {{ $t('config.dd_resolution_option_vdisplay_desc') }} {{ $t('config.dd_resolution_option_multi_instance_desc') }}
              </div>

              <!-- Configuration option -->
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

              <!-- Resolution option -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <label for="dd_resolution_option" class="block text-sm font-medium text-storm mb-1">
                  {{ $t('config.dd_resolution_option') }}
                </label>
                <select id="dd_resolution_option" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.dd_resolution_option">
                  <option value="disabled">{{ $t('config.dd_resolution_option_disabled') }}</option>
                  <option value="auto">{{ $t('config.dd_resolution_option_auto') }}</option>
                  <option value="manual">{{ $t('config.dd_resolution_option_manual') }}</option>
                </select>
                <div class="text-sm text-storm mt-1"
                     v-if="config.dd_resolution_option === 'auto' || config.dd_resolution_option === 'manual'">
                  {{ $t('config.dd_resolution_option_ogs_desc') }}
                </div>

                <!-- Manual resolution -->
                <div class="mt-2 pl-4" v-if="config.dd_resolution_option === 'manual'">
                  <div class="text-sm text-storm mt-1">
                    {{ $t('config.dd_manual_resolution') }}
                  </div>
                  <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="dd_manual_resolution" placeholder="2560x1440"
                         v-model="config.dd_manual_resolution" />
                </div>
              </div>

              <!-- Refresh rate option -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <label for="dd_refresh_rate_option" class="block text-sm font-medium text-storm mb-1">
                  {{ $t('config.dd_refresh_rate_option') }}
                </label>
                <select id="dd_refresh_rate_option" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.dd_refresh_rate_option">
                  <option value="disabled">{{ $t('config.dd_refresh_rate_option_disabled') }}</option>
                  <option value="auto">{{ $t('config.dd_refresh_rate_option_auto') }}</option>
                  <option value="manual">{{ $t('config.dd_refresh_rate_option_manual') }}</option>
                </select>

                <!-- Manual refresh rate -->
                <div class="mt-2 pl-4" v-if="config.dd_refresh_rate_option === 'manual'">
                  <div class="text-sm text-storm mt-1">
                    {{ $t('config.dd_manual_refresh_rate') }}
                  </div>
                  <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="dd_manual_refresh_rate" placeholder="59.9558"
                         v-model="config.dd_manual_refresh_rate" />
                </div>
              </div>

              <!-- Config revert delay -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <label for="dd_config_revert_delay" class="block text-sm font-medium text-storm mb-1">
                  {{ $t('config.dd_config_revert_delay') }}
                </label>
                <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="dd_config_revert_delay" placeholder="3000" min="0"
                       v-model="config.dd_config_revert_delay" />
                <div class="text-sm text-storm mt-1">
                  {{ $t('config.dd_config_revert_delay_desc') }}
                </div>
              </div>

              <!-- Config revert on disconnect -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <Checkbox id="dd_config_revert_on_disconnect"
                  locale-prefix="config"
                  v-model="config.dd_config_revert_on_disconnect"
                  default="false"
                ></Checkbox>
              </div>

              <!-- Display mode remapping -->
              <div class="mb-3" v-if="canBeRemapped()">
                <label for="dd_mode_remapping" class="block text-sm font-medium text-storm mb-1">
                  {{ $t('config.dd_mode_remapping') }}
                </label>
                <div id="dd_mode_remapping" class="flex flex-col">
                  <div class="text-sm text-storm mt-1">
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
                </div>

                <table class="w-full text-left" v-if="config.dd_mode_remapping[getRemappingType()].length > 0">
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
                      <!-- Additional columns for buttons-->
                      <th scope="col"></th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr v-for="(value, idx) in config.dd_mode_remapping[getRemappingType()]">
                      <td v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="value.requested_resolution"
                               :placeholder="'1920x1080'" />
                      </td>
                      <td v-if="getRemappingType() !== RESOLUTION_ONLY">
                        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="value.requested_fps"
                               :placeholder="'60'" />
                      </td>
                      <td v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="value.final_resolution"
                               :placeholder="'2560x1440'" />
                      </td>
                      <td v-if="getRemappingType() !== RESOLUTION_ONLY">
                        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="value.final_refresh_rate"
                               :placeholder="'119.95'" />
                      </td>
                      <td>
                        <button class="bg-red-500 text-white px-3 py-1.5 rounded-lg hover:bg-red-600 transition" @click="config.dd_mode_remapping[getRemappingType()].splice(idx, 1)">
                          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
                        </button>
                      </td>
                    </tr>
                  </tbody>
                </table>
                <button class="mt-2 bg-ice/20 text-ice px-3 py-1.5 rounded-lg hover:bg-ice/30 transition" style="margin: 0 auto" @click="addRemappingEntry()">
                  &plus; {{ $t('config.dd_mode_remapping_add') }}
                </button>
              </div>
        </div>
      </details>

      <!-- HDR option -->
      <div class="mb-3">
        <label for="dd_hdr_option" class="block text-sm font-medium text-storm mb-1">
          {{ $t('config.dd_hdr_option') }}
        </label>
        <select id="dd_hdr_option" class="mb-3 w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.dd_hdr_option">
          <option value="disabled">{{ $t('config.dd_hdr_option_disabled') }}</option>
          <option value="auto">{{ $t('config.dd_hdr_option_auto') }}</option>
        </select>
        <!-- HDR toggle -->
        <!-- <label for="dd_wa_hdr_toggle_delay" class="block text-sm font-medium text-storm mb-1">
          {{ $t('config.dd_wa_hdr_toggle_delay') }}
        </label>
        <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="dd_wa_hdr_toggle_delay" placeholder="0" min="0" max="3000"
              v-model="config.dd_wa_hdr_toggle_delay" />
        <div class="text-sm text-storm mt-1">
          {{ $t('config.dd_wa_hdr_toggle_delay_desc_1') }}
          <br>
          {{ $t('config.dd_wa_hdr_toggle_delay_desc_2') }}
          <br>
          {{ $t('config.dd_wa_hdr_toggle_delay_desc_3') }}
        </div> -->
      </div>
    </template>
    <template #linux>
    </template>
    <template #macos>
    </template>
  </PlatformLayout>
</template>
