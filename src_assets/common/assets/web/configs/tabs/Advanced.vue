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
  <div class="config-page">
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Resilience</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Stream behavior under load</h3>
          <InfoHint size="sm" label="Resilience guidance">
            Tune FEC, quantization, and CPU thread floors when you need to balance bandwidth, encode cost, and stability.
          </InfoHint>
        </div>
        <p class="settings-section-copy">Recovery margin, quantization, and encoder thread floors.</p>
      </div>

      <div class="mb-3">
        <label for="fec_percentage" class="block text-sm font-medium text-storm mb-1">{{ $t('config.fec_percentage') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="fec_percentage" placeholder="20" v-model="config.fec_percentage" />
        <div class="text-sm text-storm mt-1">{{ $t('config.fec_percentage_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="qp" class="block text-sm font-medium text-storm mb-1">{{ $t('config.qp') }}</label>
        <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="qp" placeholder="28" v-model="config.qp" />
        <div class="text-sm text-storm mt-1">{{ $t('config.qp_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="min_threads" class="block text-sm font-medium text-storm mb-1">{{ $t('config.min_threads') }}</label>
        <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="min_threads" placeholder="2" min="1" v-model="config.min_threads" />
        <div class="text-sm text-storm mt-1">{{ $t('config.min_threads_desc') }}</div>
      </div>

      <Checkbox class="mb-3"
                id="limit_framerate"
                locale-prefix="config"
                v-model="config.limit_framerate"
                default="true"
      ></Checkbox>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Compatibility</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Client and environment behavior</h3>
          <InfoHint size="sm" label="Compatibility guidance">
            Use these flags when you need legacy compatibility, encoder probe leniency, or broader codec advertising.
          </InfoHint>
        </div>
        <p class="settings-section-copy">Legacy compatibility flags and codec advertising behavior.</p>
      </div>

      <Checkbox class="mb-3"
                id="envvar_compatibility_mode"
                locale-prefix="config"
                v-model="config.envvar_compatibility_mode"
                default="false"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="legacy_ordering"
                locale-prefix="config"
                v-model="config.legacy_ordering"
                default="false"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="ignore_encoder_probe_failure"
                locale-prefix="config"
                v-model="config.ignore_encoder_probe_failure"
                default="false"
      ></Checkbox>

      <div class="mb-3">
        <label for="hevc_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.hevc_mode') }}</label>
        <select id="hevc_mode" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.hevc_mode">
          <option value="0">{{ $t('config.hevc_mode_0') }}</option>
          <option value="1">{{ $t('config.hevc_mode_1') }}</option>
          <option value="2">{{ $t('config.hevc_mode_2') }}</option>
          <option value="3">{{ $t('config.hevc_mode_3') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.hevc_mode_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="av1_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.av1_mode') }}</label>
        <select id="av1_mode" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.av1_mode">
          <option value="0">{{ $t('config.av1_mode_0') }}</option>
          <option value="1">{{ $t('config.av1_mode_1') }}</option>
          <option value="2">{{ $t('config.av1_mode_2') }}</option>
          <option value="3">{{ $t('config.av1_mode_3') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.av1_mode_desc') }}</div>
      </div>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Selection</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Capture and encoder preference</h3>
          <InfoHint size="sm" label="Capture selection guidance">
            Override automatic capture or encoder selection only when you need to force a specific host path.
          </InfoHint>
        </div>
        <p class="settings-section-copy">Only override autodetect when the host needs a forced path.</p>
      </div>

      <div class="mb-3" v-if="platform !== 'macos'">
        <label for="capture" class="block text-sm font-medium text-storm mb-1">{{ $t('config.capture') }}</label>
        <select id="capture" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.capture">
          <option value="">{{ $t('_common.autodetect') }}</option>
          <PlatformLayout :platform="platform">
            <template #linux>
              <option value="nvfbc">NvFBC</option>
              <option value="wlr">wlroots</option>
              <option value="kms">KMS</option>
              <option value="x11">X11</option>
            </template>
            <template #windows>
              <option value="ddx">Desktop Duplication API</option>
              <option value="wgc">Windows.Graphics.Capture {{ $t('_common.beta') }}</option>
            </template>
          </PlatformLayout>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.capture_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="encoder" class="block text-sm font-medium text-storm mb-1">{{ $t('config.encoder') }}</label>
        <select id="encoder" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.encoder">
          <option value="">{{ $t('_common.autodetect') }}</option>
          <PlatformLayout :platform="platform">
            <template #windows>
              <option value="nvenc">NVIDIA NVENC</option>
              <option value="quicksync">Intel QuickSync</option>
              <option value="amdvce">AMD AMF/VCE</option>
            </template>
            <template #linux>
              <option value="nvenc">NVIDIA NVENC</option>
              <option value="vaapi">VA-API</option>
            </template>
            <template #macos>
              <option value="videotoolbox">VideoToolbox</option>
            </template>
          </PlatformLayout>
          <option value="software">{{ $t('config.encoder_software') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.encoder_desc') }}</div>
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
