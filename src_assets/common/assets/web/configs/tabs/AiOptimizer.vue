<script setup>
import { ref, onMounted } from 'vue'
import { useAiOptimizer } from '../../composables/useAiOptimizer'
import { useToast } from '../../composables/useToast'

const props = defineProps(['config'])
const config = ref(props.config)

const { toast } = useToast()
const {
  status: aiStatus, cache: aiCache, devices: aiDevices, loading: aiLoading,
  fetchStatus, fetchCache, fetchDevices, clearCache, optimize
} = useAiOptimizer()

const showApiKey = ref(false)
const testResult = ref(null)
const testLoading = ref(false)
const deviceSearch = ref('')
const cacheExpanded = ref(false)

async function testApiKey() {
  testLoading.value = true
  testResult.value = null
  const isSub = config.value.ai_use_subscription === 'enabled'
  const result = await optimize('Test Device', 'Test App')
  testLoading.value = false
  if (result.status) {
    testResult.value = { success: true, message: isSub ? 'Claude CLI working — got optimization response' : 'API key is valid — got response from Claude' }
    toast(isSub ? 'Claude subscription connection verified' : 'API key validated successfully', 'success')
  } else {
    testResult.value = { success: false, message: result.error || 'Failed to connect' }
    toast((isSub ? 'Subscription test failed: ' : 'API key validation failed: ') + (result.error || 'Unknown error'), 'error')
  }
}

async function handleClearCache() {
  const ok = await clearCache()
  if (ok) {
    toast('AI optimization cache cleared', 'success')
    fetchCache()
  } else {
    toast('Failed to clear cache', 'error')
  }
}

const filteredDevices = ref([])
function filterDevices() {
  const q = deviceSearch.value.toLowerCase()
  if (!Array.isArray(aiDevices.value)) {
    filteredDevices.value = []
    return
  }
  if (!q) {
    filteredDevices.value = aiDevices.value
  } else {
    filteredDevices.value = aiDevices.value.filter(d =>
      (d.name || '').toLowerCase().includes(q) ||
      (d.type || '').toLowerCase().includes(q)
    )
  }
}

onMounted(async () => {
  await Promise.all([fetchStatus(), fetchCache(), fetchDevices()])
  filterDevices()
})
</script>

<template>
  <div class="config-page space-y-6">
    <!-- AI Mode Toggle -->
    <div class="mb-3">
      <div class="flex items-center justify-between">
        <div>
          <label class="block text-sm font-medium text-silver mb-1">AI Optimization</label>
          <div class="text-sm text-storm">Enable AI-powered streaming optimization using Claude. Automatically selects optimal display mode, bitrate, codec settings, and color range for each connected device.</div>
        </div>
        <button @click="config.ai_enabled = config.ai_enabled === 'enabled' ? 'disabled' : 'enabled'"
                class="relative inline-flex h-6 w-11 shrink-0 cursor-pointer rounded-full transition-colors duration-200"
                :class="config.ai_enabled === 'enabled' ? 'bg-purple-500' : 'bg-storm/50'">
          <span class="inline-block h-5 w-5 transform rounded-full bg-white shadow-lg transition-transform duration-200 mt-0.5"
                :class="config.ai_enabled === 'enabled' ? 'translate-x-[22px]' : 'translate-x-0.5'"></span>
        </button>
      </div>
    </div>

    <!-- Authentication Method -->
    <div class="mb-3">
      <label class="block text-sm font-medium text-silver mb-2">Authentication Method</label>
      <div class="flex gap-2 mb-3">
        <button @click="config.ai_use_subscription = 'enabled'; config.ai_api_key = ''"
                class="flex-1 px-4 py-2.5 rounded-lg text-sm font-medium transition-all duration-200 border"
                :class="config.ai_use_subscription === 'enabled' ? 'bg-purple-500/20 border-purple-500/50 text-purple-300' : 'border-storm/50 text-storm hover:border-ice hover:text-silver'">
          <div class="flex items-center justify-center gap-2">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9.663 17h4.673M12 3v1m6.364 1.636l-.707.707M21 12h-1M4 12H3m3.343-5.657l-.707-.707m2.828 9.9a5 5 0 117.072 0l-.548.547A3.374 3.374 0 0014 18.469V19a2 2 0 11-4 0v-.531c0-.895-.356-1.754-.988-2.386l-.548-.547z"/></svg>
            Claude Subscription
          </div>
          <div class="text-xs mt-1 opacity-70">Uses your logged-in Claude session</div>
        </button>
        <button @click="config.ai_use_subscription = 'disabled'"
                class="flex-1 px-4 py-2.5 rounded-lg text-sm font-medium transition-all duration-200 border"
                :class="config.ai_use_subscription !== 'enabled' ? 'bg-ice/10 border-ice/30 text-ice' : 'border-storm/50 text-storm hover:border-ice hover:text-silver'">
          <div class="flex items-center justify-center gap-2">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z"/></svg>
            API Key
          </div>
          <div class="text-xs mt-1 opacity-70">Direct Anthropic API access</div>
        </button>
      </div>
    </div>

    <!-- Subscription mode info -->
    <div v-if="config.ai_use_subscription === 'enabled'" class="mb-3 p-3 rounded-lg border border-purple-500/20 bg-purple-500/5">
      <div class="flex items-start gap-3">
        <svg class="w-5 h-5 text-purple-400 shrink-0 mt-0.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
        <div>
          <div class="text-sm text-silver mb-1">Uses the <code class="text-purple-300 bg-purple-500/10 px-1 rounded">claude</code> CLI with your active subscription.</div>
          <div class="text-xs text-silver/60 mb-2">Make sure you're logged in — run <code class="bg-void/50 px-1 rounded">claude</code> in a terminal to verify. No API key or credits needed.</div>
          <div class="flex items-center gap-2">
            <span class="w-2 h-2 rounded-full" :class="aiStatus?.cli_available ? 'bg-green-400' : 'bg-red-400'"></span>
            <span class="text-xs" :class="aiStatus?.cli_available ? 'text-green-400' : 'text-red-400'">
              {{ aiStatus?.cli_available ? 'Claude CLI detected' : 'Claude CLI not found in PATH' }}
            </span>
          </div>
        </div>
      </div>
      <div class="mt-3">
        <button @click="testApiKey" :disabled="testLoading"
                class="inline-flex items-center gap-1.5 h-8 px-3 text-xs font-medium rounded-lg border border-purple-500/30 text-purple-300 hover:bg-purple-500/10 transition-all duration-200 disabled:opacity-50">
          <svg v-if="testLoading" class="w-3 h-3 animate-spin" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
          <span v-else>Test Connection</span>
        </button>
        <div v-if="testResult" class="mt-2 text-sm px-3 py-2 rounded-lg" :class="testResult.success ? 'bg-green-500/10 text-green-400' : 'bg-red-500/10 text-red-400'">
          {{ testResult.message }}
        </div>
      </div>
    </div>

    <!-- API Key mode -->
    <div v-else class="mb-3">
      <label for="ai_api_key" class="block text-sm font-medium text-silver mb-1">Claude API Key</label>
      <div class="flex gap-2">
        <div class="relative flex-1">
          <input
            :type="showApiKey ? 'text' : 'password'"
            class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm pr-10"
            id="ai_api_key"
            placeholder="sk-ant-api03-..."
            v-model="config.ai_api_key"
          />
          <button @click="showApiKey = !showApiKey" class="absolute right-2 top-1/2 -translate-y-1/2 text-storm hover:text-silver transition-colors">
            <svg v-if="!showApiKey" class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z"/></svg>
            <svg v-else class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13.875 18.825A10.05 10.05 0 0112 19c-4.478 0-8.268-2.943-9.543-7a9.97 9.97 0 011.563-3.029m5.858.908a3 3 0 114.243 4.243M9.878 9.878l4.242 4.242M9.88 9.88l-3.29-3.29m7.532 7.532l3.29 3.29M3 3l3.59 3.59m0 0A9.953 9.953 0 0112 5c4.478 0 8.268 2.943 9.543 7a10.025 10.025 0 01-4.132 5.411m0 0L21 21"/></svg>
          </button>
        </div>
        <button @click="testApiKey" :disabled="testLoading || !config.ai_api_key"
                class="inline-flex items-center gap-1.5 h-10 px-3 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice transition-all duration-200 disabled:opacity-50 disabled:cursor-not-allowed shrink-0">
          <svg v-if="testLoading" class="w-4 h-4 animate-spin" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
          <span v-else>Test</span>
        </button>
      </div>
      <div class="text-sm text-storm mt-1">Get an API key from <a href="https://console.anthropic.com/settings/keys" target="_blank" class="text-ice hover:text-ice/80">console.anthropic.com</a>. Uses Claude Haiku (fast, cheap).</div>
      <div v-if="testResult" class="mt-2 text-sm px-3 py-2 rounded-lg" :class="testResult.success ? 'bg-green-500/10 text-green-400' : 'bg-red-500/10 text-red-400'">
        {{ testResult.message }}
      </div>
    </div>

    <!-- Cache TTL -->
    <div class="mb-3">
      <label for="ai_cache_ttl_hours" class="block text-sm font-medium text-silver mb-1">Cache Duration</label>
      <div class="flex items-center gap-3">
        <input type="range" min="24" max="720" step="24"
               class="flex-1 accent-ice h-1.5 rounded-lg cursor-pointer"
               id="ai_cache_ttl_hours"
               v-model="config.ai_cache_ttl_hours" />
        <span class="text-sm text-silver font-medium w-12 text-right">{{ config.ai_cache_ttl_hours ? Math.round(config.ai_cache_ttl_hours / 24) + 'd' : '7d' }}</span>
      </div>
      <div class="text-sm text-storm mt-1">How long to cache AI optimization results before re-querying. Default: 7 days.</div>
    </div>

    <!-- Timeout -->
    <div class="mb-3">
      <label for="ai_timeout_ms" class="block text-sm font-medium text-silver mb-1">API Timeout</label>
      <div class="flex items-center gap-2">
        <input type="number" min="1000" max="30000" step="500"
               class="w-32 bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
               id="ai_timeout_ms"
               v-model="config.ai_timeout_ms"
               placeholder="5000" />
        <span class="text-sm text-storm">ms</span>
      </div>
      <div class="text-sm text-storm mt-1">Maximum time to wait for an AI response. Longer timeouts allow for retries but may delay cache misses.</div>
    </div>

    <!-- Optimization Cache -->
    <div class="p-4 bg-deep rounded-lg border border-storm/50">
      <div class="flex items-center justify-between mb-3 cursor-pointer" @click="cacheExpanded = !cacheExpanded">
        <div class="flex items-center gap-2">
          <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider">Optimization Cache</div>
          <span class="px-1.5 py-0.5 rounded text-xs font-mono bg-twilight text-silver">{{ Array.isArray(aiCache) ? aiCache.length : 0 }}</span>
        </div>
        <div class="flex items-center gap-2">
          <button @click.stop="handleClearCache"
                  class="text-xs text-storm hover:text-red-400 transition-colors"
                  :class="{ 'opacity-50 pointer-events-none': !Array.isArray(aiCache) || aiCache.length === 0 }">
            Clear All
          </button>
          <svg class="w-4 h-4 text-storm transition-transform" :class="{ 'rotate-180': cacheExpanded }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
        </div>
      </div>
      <div v-if="cacheExpanded && Array.isArray(aiCache) && aiCache.length > 0" class="space-y-2 max-h-96 overflow-y-auto scrollbar-hidden">
        <div v-for="(entry, i) in aiCache" :key="i" class="py-2" :class="i > 0 ? 'border-t border-storm/20' : ''">
          <div class="flex items-center justify-between text-sm cursor-pointer" @click="entry._expanded = !entry._expanded">
            <div class="min-w-0 flex-1">
              <div class="text-silver font-medium truncate">{{ entry.device_name }}{{ entry.app_name ? ' + ' + entry.app_name : '' }}</div>
              <div class="text-xs text-silver/60">
                {{ entry.display_mode || '—' }} · {{ entry.target_bitrate_kbps ? (entry.target_bitrate_kbps / 1000).toFixed(0) + ' Mbps' : '—' }}
                <span v-if="entry.preferred_codec"> · {{ entry.preferred_codec.toUpperCase() }}</span>
                · {{ entry.source }}
              </div>
            </div>
            <div class="flex items-center gap-2 shrink-0 ml-3">
              <div class="text-xs text-storm" v-if="entry.cached_at">
                {{ new Date(entry.cached_at * 1000).toLocaleDateString([], { month: 'short', day: 'numeric' }) }}
              </div>
              <svg class="w-3 h-3 text-storm transition-transform" :class="{ 'rotate-180': entry._expanded }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
            </div>
          </div>
          <!-- Optimization diff view -->
          <div v-if="entry._expanded" class="mt-2 p-3 bg-void/50 rounded-lg text-xs space-y-1.5">
            <div class="flex justify-between" v-if="entry.display_mode">
              <span class="text-storm">Display Mode</span>
              <span class="text-ice font-mono">{{ entry.display_mode }}</span>
            </div>
            <div class="flex justify-between" v-if="entry.target_bitrate_kbps">
              <span class="text-storm">Target Bitrate</span>
              <span class="text-ice font-mono">{{ (entry.target_bitrate_kbps / 1000).toFixed(1) }} Mbps</span>
            </div>
            <div class="flex justify-between" v-if="entry.nvenc_tune">
              <span class="text-storm">NVENC Tune</span>
              <span class="text-ice font-mono">{{ entry.nvenc_tune === 1 ? 'Quality' : entry.nvenc_tune === 2 ? 'Low Latency' : 'Ultra Low Latency' }}</span>
            </div>
            <div class="flex justify-between" v-if="entry.color_range != null">
              <span class="text-storm">Color Range</span>
              <span class="text-ice font-mono">{{ entry.color_range === 0 ? 'Client' : entry.color_range === 1 ? 'Limited' : 'Full' }}</span>
            </div>
            <div class="flex justify-between" v-if="entry.preferred_codec">
              <span class="text-storm">Preferred Codec</span>
              <span class="text-ice font-mono">{{ entry.preferred_codec.toUpperCase() }}</span>
            </div>
            <div class="flex justify-between" v-if="entry.hdr != null">
              <span class="text-storm">HDR</span>
              <span class="font-mono" :class="entry.hdr ? 'text-green-400' : 'text-storm'">{{ entry.hdr ? 'Yes' : 'No' }}</span>
            </div>
            <div v-if="entry.reasoning" class="pt-1.5 border-t border-storm/20">
              <span class="text-storm">AI Reasoning: </span>
              <span class="text-silver/80 italic">{{ entry.reasoning }}</span>
            </div>
          </div>
        </div>
      </div>
      <div v-else-if="cacheExpanded" class="text-sm text-storm text-center py-3">No cached optimizations yet</div>
    </div>

    <!-- Device Knowledge Base -->
    <div class="p-4 bg-deep rounded-lg border border-storm/50">
      <div class="flex items-center justify-between mb-3">
        <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider">Device Knowledge Base</div>
        <span class="px-1.5 py-0.5 rounded text-xs font-mono bg-twilight text-silver">{{ filteredDevices.length }} devices</span>
      </div>
      <input type="text" v-model="deviceSearch" @input="filterDevices" placeholder="Search devices..."
             class="w-full bg-void/50 border border-storm/50 rounded-lg px-3 py-2 text-sm text-silver focus:border-ice focus:outline-none mb-3" />
      <div class="space-y-1 max-h-72 overflow-y-auto scrollbar-hidden">
        <div v-for="device in filteredDevices" :key="device.name" class="flex items-center justify-between py-2 px-2 rounded-lg hover:bg-twilight/30 transition-colors">
          <div class="min-w-0 flex-1">
            <div class="text-sm text-silver font-medium">{{ device.name }}</div>
            <div class="text-xs text-silver/60">
              <span class="capitalize">{{ device.type }}</span>
              <span v-if="device.display_mode"> · {{ device.display_mode }}</span>
              <span v-if="device.preferred_codec"> · {{ device.preferred_codec.toUpperCase() }}</span>
              <span v-if="device.ideal_bitrate_kbps"> · {{ (device.ideal_bitrate_kbps / 1000).toFixed(0) }} Mbps</span>
              <span v-if="device.hdr_capable" class="text-purple-400"> HDR</span>
            </div>
          </div>
          <div class="flex items-center gap-1 shrink-0">
            <span class="px-1.5 py-0.5 rounded text-xs" :class="{
              'bg-green-500/10 text-green-400': device.type === 'handheld',
              'bg-blue-500/10 text-blue-400': device.type === 'phone',
              'bg-purple-500/10 text-purple-400': device.type === 'desktop',
              'bg-yellow-500/10 text-yellow-400': device.type === 'tablet'
            }">{{ device.type }}</span>
          </div>
        </div>
        <div v-if="filteredDevices.length === 0" class="text-sm text-storm text-center py-3">
          {{ deviceSearch ? 'No matching devices' : 'Loading devices...' }}
        </div>
      </div>
    </div>
  </div>
</template>
