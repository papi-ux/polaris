<script setup>
import { ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
</script>

<template>
  <div class="mb-3" v-if="platform !== 'macos'">
    <label for="adapter_name" class="block text-sm font-medium text-storm mb-1">{{ $t('config.adapter_name') }}</label>
    <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="adapter_name"
           :placeholder="$tp('config.adapter_name_placeholder', '/dev/dri/renderD128')"
           v-model="config.adapter_name" />
    <div class="text-sm text-storm mt-1">
      <PlatformLayout :platform="platform">
        <template #windows>
          {{ $t('config.adapter_name_desc_windows') }}
          <div class="settings-subtle-surface mt-3">
            <div class="section-kicker">Discovery helper</div>
            <pre class="mt-2 overflow-x-auto whitespace-pre-wrap font-mono text-xs text-silver">tools\dxgi-info.exe</pre>
          </div>
        </template>
        <template #linux>
          {{ $t('config.adapter_name_desc_linux_1') }}
          <div class="settings-subtle-surface mt-3 space-y-2">
            <div class="section-kicker">Discovery helpers</div>
            <pre class="overflow-x-auto whitespace-pre-wrap font-mono text-xs text-silver">ls /dev/dri/renderD*  # {{ $t('config.adapter_name_desc_linux_2') }}</pre>
            <pre class="overflow-x-auto whitespace-pre-wrap font-mono text-xs text-silver">
              vainfo --display drm --device /dev/dri/renderD129 | \
                grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
            </pre>
            <div>{{ $t('config.adapter_name_desc_linux_3') }}</div>
            <i class="text-silver">VAProfileH264High   : VAEntrypointEncSlice</i>
          </div>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
