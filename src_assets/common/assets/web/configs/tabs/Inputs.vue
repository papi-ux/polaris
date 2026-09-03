<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from "../../Checkbox.vue";
import InfoHint from '../../components/InfoHint.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
</script>

<template>
  <div id="input" class="config-page">
    <p class="text-xs text-storm" data-tab-docs-link>
      <a href="https://papi-ux.com/docs/troubleshooting/#input-does-not-work" target="_blank" rel="noopener" class="focus-ring text-ice hover:underline">{{ $t('config.inputs_docs_link') }}</a>
    </p>
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.inputs_kicker_controllers') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.inputs_section_gamepads') }}</h3>
          <InfoHint size="sm" :label="$t('config.inputs_hint_gamepad_handling_guidance')">
            {{ $t('config.inputs_hint_gamepad_handling_guidance_body') }}
          </InfoHint>
        </div>
      </div>

      <Checkbox class="mb-3"
                id="controller"
                locale-prefix="config"
                v-model="config.controller"
                default="true"
      ></Checkbox>

      <div class="mb-3" v-if="config.controller === 'enabled' && platform !== 'macos'">
      <div class="settings-field-head">
        <label for="gamepad" class="settings-field-label">{{ $t('config.gamepad') }}</label>
        <InfoHint size="sm" :label="$t('config.gamepad')">{{ $t('config.gamepad_desc') }}</InfoHint>
      </div>
      <select id="gamepad" class="settings-input" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

        <PlatformLayout :platform="platform">
          <template #linux>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
          </template>

          <template #windows>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
          </template>
        </PlatformLayout>
      </select>
      </div>

      <template v-if="config.controller === 'enabled'">
      <template v-if="config.gamepad === 'ds4' || config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform !== 'macos')">
        <details class="mb-3 bg-deep rounded-xl border border-storm" open>
          <summary class="px-4 py-3 cursor-pointer text-silver font-medium hover:text-ice transition-colors">
                {{ $t(config.gamepad === 'ds4' ? 'config.gamepad_ds4_manual' : (config.gamepad === 'ds5' ? 'config.gamepad_ds5_manual' : 'config.gamepad_auto')) }}
          </summary>
          <div class="px-4 pb-4">
                <!-- Automatic detection options (for Windows and Linux) -->
                <template v-if="config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')">
                  <!-- Gamepad with motion-capability as DS4(Windows)/DS5(Linux) -->
                  <Checkbox class="mb-3"
                            id="motion_as_ds4"
                            locale-prefix="config"
                            v-model="config.motion_as_ds4"
                            default="true"
                  ></Checkbox>
                  <!-- Gamepad with touch-capability as DS4(Windows)/DS5(Linux) -->
                  <Checkbox class="mb-3"
                            id="touchpad_as_ds4"
                            locale-prefix="config"
                            v-model="config.touchpad_as_ds4"
                            default="true"
                  ></Checkbox>
                </template>
                <!-- DS4 option: DS4 back button as touchpad click (on Automatic: Windows only) -->
                <template v-if="config.gamepad === 'ds4' || (config.gamepad === 'auto' && platform === 'windows')">
                  <Checkbox class="mb-3"
                            id="ds4_back_as_touchpad_click"
                            locale-prefix="config"
                            v-model="config.ds4_back_as_touchpad_click"
                            default="true"
                  ></Checkbox>
                </template>
                <!-- DS5 Option: Controller MAC randomization (on Automatic: Linux only) -->
                <template v-if="config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform === 'linux')">
                  <Checkbox class="mb-3"
                            id="ds5_inputtino_randomize_mac"
                            locale-prefix="config"
                            v-model="config.ds5_inputtino_randomize_mac"
                            default="true"
                  ></Checkbox>
                </template>
          </div>
        </details>
      </template>
        <Checkbox class="mb-3"
                  v-if="platform === 'linux'"
                  id="headless_gamepad_isolation"
                  locale-prefix="config"
                  v-model="config.headless_gamepad_isolation"
                  default="true"
        ></Checkbox>
        <Checkbox class="mb-3"
                  v-if="platform === 'linux'"
                  id="client_gamepad_seat_isolation"
                  locale-prefix="config"
                  v-model="config.client_gamepad_seat_isolation"
                  default="false"
        ></Checkbox>
      </template>

      <div class="mb-3" v-if="config.controller === 'enabled'">
      <div class="settings-field-head">
        <label for="back_button_timeout" class="settings-field-label">{{ $t('config.back_button_timeout') }}</label>
        <InfoHint size="sm" :label="$t('config.back_button_timeout')">{{ $t('config.back_button_timeout_desc') }}</InfoHint>
      </div>
      <input type="text" class="settings-input" id="back_button_timeout" placeholder="-1"
             v-model="config.back_button_timeout" />
      </div>
    </section>

    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.inputs_kicker_keyboard') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.inputs_section_keyboard') }}</h3>
          <InfoHint size="sm" :label="$t('config.inputs_hint_keyboard_passthrough_guidance')">
            {{ $t('config.inputs_hint_keyboard_passthrough_guidance_body') }}
          </InfoHint>
        </div>
      </div>

      <Checkbox class="mb-3"
                id="keyboard"
                locale-prefix="config"
                v-model="config.keyboard"
                default="true"
      ></Checkbox>

      <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <div class="settings-field-head">
        <label for="key_repeat_delay" class="settings-field-label">{{ $t('config.key_repeat_delay') }}</label>
        <InfoHint size="sm" :label="$t('config.key_repeat_delay')">{{ $t('config.key_repeat_delay_desc') }}</InfoHint>
      </div>
      <input type="text" class="settings-input" id="key_repeat_delay" placeholder="500"
             v-model="config.key_repeat_delay" />
      </div>

      <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <div class="settings-field-head">
        <label for="key_repeat_frequency" class="settings-field-label">{{ $t('config.key_repeat_frequency') }}</label>
        <InfoHint size="sm" :label="$t('config.key_repeat_frequency')">{{ $t('config.key_repeat_frequency_desc') }}</InfoHint>
      </div>
      <input type="text" class="settings-input" id="key_repeat_frequency" placeholder="24.9"
             v-model="config.key_repeat_frequency" />
      </div>

      <Checkbox v-if="config.keyboard === 'enabled' && platform === 'windows'"
                class="mb-3"
                id="always_send_scancodes"
                locale-prefix="config"
                v-model="config.always_send_scancodes"
                default="true"
      ></Checkbox>

      <Checkbox v-if="config.keyboard === 'enabled'"
                class="mb-3"
                id="key_rightalt_to_key_win"
                locale-prefix="config"
                v-model="config.key_rightalt_to_key_win"
                default="false"
      ></Checkbox>
    </section>

    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.inputs_kicker_mouse_touch') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.inputs_section_pointer_and_touch') }}</h3>
          <InfoHint size="sm" :label="$t('config.inputs_hint_pointer_and_touch_guidance')">
            {{ $t('config.inputs_hint_pointer_and_touch_guidance_body') }}
          </InfoHint>
        </div>
      </div>

      <Checkbox class="mb-3"
                id="mouse"
                locale-prefix="config"
                v-model="config.mouse"
                default="true"
      ></Checkbox>

      <div v-if="config.mouse === 'enabled'" class="settings-toggle-row mb-3">
      <div class="min-w-0">
        <div>
          <div class="settings-field-head !mb-0">
            <div class="text-sm font-medium text-silver">{{ $t('config.inputs_show_host_cursor') }}</div>
            <InfoHint size="sm" :label="$t('config.inputs_hint_show_host_cursor')">
              {{ $t('config.inputs_hint_show_host_cursor_body') }}
            </InfoHint>
          </div>
        </div>
        <label class="relative inline-flex items-center cursor-pointer shrink-0">
          <input
            type="checkbox"
            class="sr-only peer"
            :checked="config.mouse_cursor_visible === 'enabled'"
            @change="config.mouse_cursor_visible = $event.target.checked ? 'enabled' : 'disabled'"
          />
          <div class="w-9 h-5 bg-storm/40 peer-focus:outline-none rounded-full peer peer-checked:bg-accent transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full"></div>
        </label>
      </div>
      </div>

      <Checkbox v-if="config.mouse === 'enabled'"
                class="mb-3"
                id="high_resolution_scrolling"
                locale-prefix="config"
                v-model="config.high_resolution_scrolling"
                default="true"
      ></Checkbox>

      <Checkbox v-if="config.mouse === 'enabled'"
                class="mb-3"
                id="native_pen_touch"
                locale-prefix="config"
                v-model="config.native_pen_touch"
                default="true"
      ></Checkbox>

      <Checkbox v-if="platform === 'linux'"
                class="mb-3"
                id="client_keyboard_mouse_seat_isolation"
                locale-prefix="config"
                v-model="config.client_keyboard_mouse_seat_isolation"
                default="false"
      ></Checkbox>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.inputs_kicker_accessories') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.inputs_section_extras') }}</h3>
          <InfoHint size="sm" :label="$t('config.inputs_hint_extended_input_guidance')">
            {{ $t('config.inputs_hint_extended_input_guidance_body') }}
          </InfoHint>
        </div>
      </div>

      <Checkbox class="mb-3"
                id="enable_input_only_mode"
                locale-prefix="config"
                v-model="config.enable_input_only_mode"
                default="false"
      ></Checkbox>

      <Checkbox v-if="platform === 'windows'"
                class="mb-3"
                id="forward_rumble"
                locale-prefix="config"
                v-model="config.forward_rumble"
                default="true"
      ></Checkbox>
    </section>

  </div>
</template>

<style scoped>

</style>
