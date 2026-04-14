<template>
  <div class="flex flex-col gap-4 my-6">
    <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
      <div>
        <h1 class="text-2xl font-bold text-silver">{{ $t('config.configuration') }}</h1>
        <p class="text-sm text-storm mt-2 max-w-2xl">{{ $t('config.configuration_desc') }}</p>
      </div>
      <div v-if="config" class="grid grid-cols-2 gap-2 w-full lg:w-auto lg:min-w-[360px]">
        <div class="card p-3">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">{{ $t('config.summary_active_tab') }}</div>
          <div class="text-sm text-silver font-medium mt-1">{{ activeTabMeta?.name || 'General' }}</div>
          <div class="text-[10px] text-storm mt-1">{{ activeTabSummary }}</div>
        </div>
        <div class="card p-3">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">{{ $t('config.summary_state') }}</div>
          <div class="text-sm font-medium mt-1" :class="hasUnsavedChanges ? 'text-amber-300' : 'text-green-400'">
            {{ hasUnsavedChanges ? $t('config.unsaved_changes') : $t('config.all_changes_saved') }}
          </div>
          <div class="text-[10px] text-storm mt-1">
            {{ searchQuery ? searchSummary : $t('config.summary_state_desc') }}
          </div>
        </div>
      </div>
    </div>

    <div class="sticky top-4 z-20 glass rounded-2xl border border-storm/40 p-4">
      <div class="flex flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
        <div class="min-w-0">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">{{ $t('config.action_center') }}</div>
          <div class="text-sm text-silver font-medium mt-1">
            {{ hasUnsavedChanges ? $t('config.unsaved_banner') : $t('config.saved_banner') }}
          </div>
          <div class="text-[10px] text-storm mt-1">
            {{ saved ? $t('config.apply_note') : $t('config.restart_note_hint') }}
          </div>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <span class="px-2 py-1 rounded-full text-[10px] border border-storm/30 text-storm">{{ visibleTabCountLabel }}</span>
          <span v-if="hasUnsavedChanges" class="px-2 py-1 rounded-full text-[10px] border border-amber-300/40 text-amber-300">{{ $t('config.pending_badge') }}</span>
          <span v-else class="px-2 py-1 rounded-full text-[10px] border border-green-400/30 text-green-400">{{ $t('config.synced_badge') }}</span>
          <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice transition-all duration-200 disabled:opacity-50" @click="resetLocalChanges" :disabled="!hasUnsavedChanges || saving || restarting">
            {{ $t('config.reset_local') }}
          </button>
          <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200 disabled:opacity-50" @click="save" :disabled="!config || !hasUnsavedChanges || saving || restarting" tabindex="0">
            {{ saving ? $t('config.saving') : $t('_common.save') }}
          </button>
          <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice/20 text-ice hover:bg-ice/30 hover:shadow-[0_0_24px_rgba(34,197,94,0.2)] transition-all duration-200 disabled:opacity-50" @click="apply" :disabled="!config || saving || restarting || (!saved && !hasUnsavedChanges)" tabindex="0">
            {{ restarting ? $t('config.restarting') : $t('_common.apply') }}
          </button>
        </div>
      </div>
    </div>

    <input v-model="searchQuery" type="text" :placeholder="$t('config.search_placeholder')"
           class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
           @input="onSearch">
  </div>

  <!-- Skeleton while loading -->
  <div v-if="!config" class="space-y-4">
    <Skeleton type="text" />
    <Skeleton type="card" />
    <Skeleton type="card" />
    <Skeleton type="card" />
  </div>

  <div v-if="config">
    <!-- Tab navigation (hidden during search) -->
    <div v-if="!searchQuery" class="flex flex-wrap gap-1 border-b border-storm mb-4">
      <button
        v-for="tab in tabs"
        :key="tab.id"
        class="px-4 py-2 text-sm font-medium rounded-t-lg transition-colors"
        :class="tab.id === currentTab ? 'bg-twilight text-ice border-b-2 border-ice' : 'text-storm hover:text-silver hover:bg-twilight/50'"
        @click="currentTab = tab.id"
        :tabindex="0"
      >
        {{tab.name}}
      </button>
    </div>

    <!-- Search results indicator -->
    <div v-if="searchQuery" class="text-sm text-storm mb-4">
      {{ searchSummary }}
    </div>

    <!-- General Tab -->
    <general
      v-if="shouldShowTab('general')"
      :config="config"
      :global-prep-cmd="global_prep_cmd"
      :global-state-cmd="global_state_cmd"
      :server-cmd="server_cmd"
      :platform="platform">
    </general>

    <!-- Input Tab -->
    <inputs
      v-if="shouldShowTab('input')"
      :config="config"
      :platform="platform">
    </inputs>

    <!-- Audio/Video Tab -->
    <audio-video
      v-if="shouldShowTab('av')"
      :config="config"
      :platform="platform"
      :vdisplay="vdisplayStatus"
    >
    </audio-video>

    <!-- Network Tab -->
    <network
      v-if="shouldShowTab('network')"
      :config="config"
      :platform="platform">
    </network>

    <!-- Files Tab -->
    <files
      v-if="shouldShowTab('files')"
      :config="config"
      :platform="platform">
    </files>

    <!-- Advanced Tab -->
    <advanced
      v-if="shouldShowTab('advanced')"
      :config="config"
      :platform="platform">
    </advanced>

    <!-- AI Optimizer Tab -->
    <ai-optimizer
      v-if="shouldShowTab('ai')"
      :config="config">
    </ai-optimizer>

    <container-encoders
      :current-tab="searchQuery ? 'all' : currentTab"
      :config="config"
      :platform="platform">
    </container-encoders>
  </div>

  <div class="h-4"></div>
</template>

<script setup>
import { ref, computed, provide, onMounted, inject, watch, onUnmounted } from 'vue'
import General from '../configs/tabs/General.vue'
import Inputs from '../configs/tabs/Inputs.vue'
import Network from '../configs/tabs/Network.vue'
import Files from '../configs/tabs/Files.vue'
import Advanced from '../configs/tabs/Advanced.vue'
import AudioVideo from '../configs/tabs/AudioVideo.vue'
import AiOptimizer from '../configs/tabs/AiOptimizer.vue'
import ContainerEncoders from '../configs/tabs/ContainerEncoders.vue'
import Skeleton from '../components/Skeleton.vue'
import { useToast } from '../composables/useToast'

const { toast } = useToast()
const i18n = inject('i18n')

let fallbackDisplayModeCache = ""

const platform = ref("")
const saved = ref(false)
const restarted = ref(false)
const saving = ref(false)
const restarting = ref(false)
const config = ref(null)
const currentTab = ref("general")
const vdisplayStatus = ref("1")
const global_prep_cmd = ref([])
const global_state_cmd = ref([])
const server_cmd = ref([])
const searchQuery = ref("")
const initialSerialized = ref("")
const tabs = ref([
  {
    id: "general",
    name: "General",
    options: {
      "locale": "en",
      "sunshine_name": "",
      "min_log_level": 2,
      "global_prep_cmd": [],
      "global_state_cmd": [],
      "server_cmd": [],
      "notify_pre_releases": "disabled",
      "system_tray": "enabled",
      "hide_tray_controls": "disabled",
      "enable_pairing": "enabled",
      "enable_discovery": "enabled",
      "steamgriddb_api_key": "",
    },
  },
  {
    id: "input",
    name: "Input",
    options: {
      "controller": "enabled",
      "gamepad": "auto",
      "ds4_back_as_touchpad_click": "enabled",
      "motion_as_ds4": "enabled",
      "touchpad_as_ds4": "enabled",
      "ds5_inputtino_randomize_mac": "enabled",
      "back_button_timeout": -1,
      "keyboard": "enabled",
      "key_repeat_delay": 500,
      "key_repeat_frequency": 24.9,
      "always_send_scancodes": "enabled",
      "key_rightalt_to_key_win": "disabled",
      "mouse": "enabled",
      "mouse_cursor_visible": "disabled",
      "high_resolution_scrolling": "enabled",
      "native_pen_touch": "enabled",
      "enable_input_only_mode": "disabled",
      "forward_rumble": "enabled",
      "keybindings": "[0x10,0xA0,0x11,0xA2,0x12,0xA4]",
    },
  },
  {
    id: "av",
    name: "Audio/Video",
    options: {
      "audio_sink": "",
      "virtual_sink": "",
      "stream_audio": "enabled",
      "install_steam_audio_drivers": "enabled",
      "keep_sink_default": "enabled",
      "auto_capture_sink": "enabled",
      "adapter_name": "",
      "output_name": "",
      "fallback_mode": "",
      "isolated_virtual_display_option": "disabled",
      "dd_configuration_option": "disabled",
      "dd_resolution_option": "auto",
      "dd_manual_resolution": "",
      "dd_refresh_rate_option": "auto",
      "dd_manual_refresh_rate": "",
      "dd_hdr_option": "auto",
      "dd_wa_hdr_toggle_delay": 0,
      "dd_config_revert_delay": 3000,
      "dd_config_revert_on_disconnect": "disabled",
      "dd_mode_remapping": {"mixed": [], "resolution_only": [], "refresh_rate_only": []},
      "dd_wa_hdr_toggle": "disabled",
      "headless_mode": "disabled",
      "linux_prefer_gpu_native_capture": "enabled",
      "linux_capture_profile": "disabled",
      "double_refreshrate": "disabled",
      "max_bitrate": 0,
      "minimum_fps_target": 0,
      "adaptive_bitrate_enabled": "disabled",
      "adaptive_bitrate_min": 2000,
      "adaptive_bitrate_max": 100000,
    },
  },
  {
    id: "network",
    name: "Network",
    options: {
      "upnp": "disabled",
      "address_family": "ipv4",
      "port": 47989,
      "origin_web_ui_allowed": "lan",
      "external_ip": "",
      "lan_encryption_mode": 0,
      "wan_encryption_mode": 1,
      "ping_timeout": 10000,
      "trusted_subnets": [],
    },
  },
  {
    id: "files",
    name: "Config Files",
    options: {
      "file_apps": "",
      "credentials_file": "",
      "log_path": "",
      "pkey": "",
      "cert": "",
      "file_state": "",
    },
  },
  {
    id: "advanced",
    name: "Advanced",
    options: {
      "fec_percentage": 20,
      "qp": 28,
      "min_threads": 2,
      "limit_framerate": "enabled",
      "envvar_compatibility_mode": "disabled",
      "legacy_ordering": "disabled",
      "ignore_encoder_probe_failure": "disabled",
      "hevc_mode": 0,
      "av1_mode": 0,
      "capture": "",
      "encoder": "",
    },
  },
  {
    id: "ai",
    name: "AI",
    options: {
      "ai_enabled": "disabled",
      "ai_provider": "anthropic",
      "ai_model": "",
      "ai_auth_mode": "",
      "ai_api_key": "",
      "ai_base_url": "",
      "ai_use_subscription": "disabled",
      "ai_timeout_ms": 5000,
      "ai_cache_ttl_hours": 168,
    },
  },
  {
    id: "nv",
    name: "NVIDIA NVENC Encoder",
    options: {
      "nvenc_preset": 1,
      "nvenc_twopass": "quarter_res",
      "nvenc_spatial_aq": "disabled",
      "nvenc_vbv_increase": 0,
      "nvenc_realtime_hags": "enabled",
      "nvenc_latency_over_power": "enabled",
      "nvenc_opengl_vulkan_on_dxgi": "enabled",
      "nvenc_h264_cavlc": "disabled",
      "nvenc_intra_refresh": "disabled"
    },
  },
  {
    id: "qsv",
    name: "Intel QuickSync Encoder",
    options: {
      "qsv_preset": "medium",
      "qsv_coder": "auto",
      "qsv_slow_hevc": "disabled",
    },
  },
  {
    id: "amd",
    name: "AMD AMF Encoder",
    options: {
      "amd_usage": "ultralowlatency",
      "amd_rc": "vbr_latency",
      "amd_enforce_hrd": "disabled",
      "amd_quality": "balanced",
      "amd_preanalysis": "disabled",
      "amd_vbaq": "enabled",
      "amd_coder": "auto",
    },
  },
  {
    id: "vt",
    name: "VideoToolbox Encoder",
    options: {
      "vt_coder": "auto",
      "vt_software": "auto",
      "vt_realtime": "enabled",
    },
  },
  {
    id: "vaapi",
    name: "VA-API Encoder",
    options: {
      "vaapi_strict_rc_buffer": "disabled",
    },
  },
  {
    id: "sw",
    name: "Software Encoder",
    options: {
      "sw_preset": "superfast",
      "sw_tune": "zerolatency",
    },
  },
])

provide('platform', computed(() => platform.value))

const activeTabMeta = computed(() => tabs.value.find(tab => tab.id === currentTab.value))
const activeTabSummary = computed(() => {
  const tab = activeTabMeta.value
  if (!tab) return ''
  const optionCount = Object.keys(tab.options).length
  return i18n.t('config.summary_options', { count: optionCount })
})
const matchingTabs = computed(() => {
  if (!searchQuery.value) return tabs.value
  const query = searchQuery.value.toLowerCase()
  return tabs.value.filter(tab =>
    tab.name.toLowerCase().includes(query) ||
    Object.keys(tab.options).some(key => key.toLowerCase().includes(query))
  )
})
const visibleTabCountLabel = computed(() => i18n.t('config.visible_tabs', { count: matchingTabs.value.length, total: tabs.value.length }))
const searchSummary = computed(() => i18n.t('config.search_results', { query: searchQuery.value, count: matchingTabs.value.length }))
const hasUnsavedChanges = computed(() => {
  if (!config.value || !initialSerialized.value) return false
  return JSON.stringify(serialize()) !== initialSerialized.value
})

function shouldShowTab(tabId) {
  if (searchQuery.value) {
    const query = searchQuery.value.toLowerCase()
    const tab = tabs.value.find(t => t.id === tabId)
    if (!tab) return false
    if (tab.name.toLowerCase().includes(query)) return true
    return Object.keys(tab.options).some(key =>
      key.toLowerCase().includes(query) ||
      tab.name.toLowerCase().includes(query)
    )
  }
  return currentTab.value === tabId
}

function onSearch() {
  // Search is reactive via v-model, shouldShowTab handles the logic
}

function serialize() {
  // Validate fallback mode
  if (config.value.fallback_mode && !config.value.fallback_mode.match(/^\d+x\d+x\d+$/)) {
    config.value.fallback_mode = fallbackDisplayModeCache
  } else {
    fallbackDisplayModeCache = config.value.fallback_mode
  }
  let configCopy = JSON.parse(JSON.stringify(config.value))
  configCopy.global_prep_cmd = global_prep_cmd.value.filter(cmd => (cmd.do && cmd.do.trim()) || (cmd.undo && cmd.undo.trim()))
  configCopy.global_state_cmd = global_state_cmd.value.filter(cmd => (cmd.do && cmd.do.trim()) || (cmd.undo && cmd.undo.trim()))
  configCopy.dd_mode_remapping = configCopy.dd_mode_remapping
  configCopy.server_cmd = server_cmd.value.filter(cmd => (cmd.name && cmd.cmd && cmd.name.trim() && cmd.cmd.trim()))

  // Serialize trusted_subnets as comma-separated string for list_string_f parser
  if (Array.isArray(configCopy.trusted_subnets)) {
    configCopy.trusted_subnets = configCopy.trusted_subnets.filter(s => s && s.trim()).join(',')
  }

  return configCopy
}

function save() {
  if (!config.value || saving.value) return Promise.resolve(false)
  saving.value = true
  saved.value = false
  restarted.value = false

  let configCopy = serialize()

  tabs.value.forEach(tab => {
    Object.keys(tab.options).forEach(optionKey => {
      let delete_value = false
      if (JSON.stringify(configCopy[optionKey]) === JSON.stringify(tab.options[optionKey])) {
        delete_value = true
      }
      if (delete_value) {
        delete configCopy[optionKey]
      }
    })
  })

  return fetch("./api/config", {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify(configCopy),
  }).then((r) => {
    if (r.status === 200) {
      saved.value = true
      initialSerialized.value = JSON.stringify(serialize())
      toast(
        i18n.t('config.apply_note') || 'Configuration saved. Restart Polaris for changes to take effect.',
        'success',
        8000,
        {
          label: 'Restart Now',
          handler: () => apply()
        }
      )
      return saved.value
    }
    else {
      toast('Failed to save configuration', 'error')
      return false
    }
  }).finally(() => {
    saving.value = false
  })
}

function apply() {
  if (restarting.value) return
  saved.value = false
  restarted.value = false
  let savedPromise = save()

  savedPromise.then((result) => {
    if (result === true) {
      restarting.value = true
      restarted.value = true
      toast(i18n.t('config.restart_note') || 'Polaris is restarting...', 'info', 5000)
      setTimeout(() => {
        saved.value = false
        restarted.value = false
        restarting.value = false
      }, 5000)
      fetch("./api/restart", {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' }
      })
      .then((resp) => {
        if (resp.status !== 200) {
          location.reload()
          return
        }
      })
      .catch((e) => {
        console.error(e)
        restarting.value = false
        setTimeout(() => { location.reload() }, 1000)
      })
    }
  })
}

function resetLocalChanges() {
  if (!initialSerialized.value) return
  const serialized = JSON.parse(initialSerialized.value)
  Object.keys(config.value).forEach((key) => { delete config.value[key] })
  Object.assign(config.value, serialized)
  global_prep_cmd.value = Array.isArray(serialized.global_prep_cmd) ? serialized.global_prep_cmd : []
  global_state_cmd.value = Array.isArray(serialized.global_state_cmd) ? serialized.global_state_cmd : []
  server_cmd.value = Array.isArray(serialized.server_cmd) ? serialized.server_cmd : []
  saved.value = false
  restarted.value = false
}

// created() logic
fetch("./api/config", { credentials: 'include' })
  .then((r) => r.json())
  .then((r) => {
    config.value = r
    platform.value = config.value.platform

    if (platform.value === "windows") {
      tabs.value = tabs.value.filter((el) => el.id !== "vt" && el.id !== "vaapi")
    }
    if (platform.value === "linux") {
      tabs.value = tabs.value.filter((el) => el.id !== "amd" && el.id !== "qsv" && el.id !== "vt")
      let nvTab = tabs.value.find(t => t.id === "nv")
      if (nvTab) nvTab.name = "NVIDIA NVENC \u2605"
    }
    if (platform.value === "macos") {
      tabs.value = tabs.value.filter((el) => el.id !== "amd" && el.id !== "nv" && el.id !== "qsv" && el.id !== "vaapi")
    }

    // remove values we don't want in the config file
    delete config.value.platform
    delete config.value.status
    delete config.value.version

    vdisplayStatus.value = config.value.vdisplayStatus
    delete config.value.vdisplayStatus

    fallbackDisplayModeCache = config.value.fallback_mode || ""

    // Parse the special options before population if available
    const specialOptions = ["dd_mode_remapping", "global_prep_cmd", "global_state_cmd", "server_cmd"]
    for (const optionKey of specialOptions) {
      if (typeof config.value[optionKey] === "string") {
        config.value[optionKey] = JSON.parse(config.value[optionKey])
      } else {
        config.value[optionKey] = null
      }
    }

    config.value.dd_mode_remapping ??= {mixed: [], resolution_only: [], refresh_rate_only: []}
    config.value.global_prep_cmd ??= []
    config.value.global_state_cmd ??= []
    config.value.server_cmd ??= []

    // Parse trusted_subnets from comma/newline-separated string or JSON array
    if (typeof config.value.trusted_subnets === "string") {
      try {
        const parsed = JSON.parse(config.value.trusted_subnets)
        config.value.trusted_subnets = Array.isArray(parsed) ? parsed : [config.value.trusted_subnets]
      } catch {
        // Plain string like "10.0.0.0/24" or "10.0.0.0/24\n192.168.1.0/24"
        config.value.trusted_subnets = config.value.trusted_subnets
          .split(/[\n,]+/)
          .map(s => s.trim())
          .filter(s => s.length > 0)
      }
    }
    config.value.trusted_subnets ??= []

    // Populate default values from tabs options
    tabs.value.forEach(tab => {
      Object.keys(tab.options).forEach(optionKey => {
        if (config.value[optionKey] === undefined) {
          config.value[optionKey] = JSON.parse(JSON.stringify(tab.options[optionKey]))
        }
      })
    })

    if (platform.value === 'windows') {
      global_prep_cmd.value = config.value.global_prep_cmd.map((i) => {
        i.elevated = !!i.elevated
        return i
      })
      global_state_cmd.value = config.value.global_state_cmd.map((i) => {
        i.elevated = !!i.elevated
        return i
      })
      server_cmd.value = config.value.server_cmd.map((i) => {
        i.elevated = !!i.elevated
        return i
      })
    } else {
      global_prep_cmd.value = config.value.global_prep_cmd
      global_state_cmd.value = config.value.global_state_cmd
      server_cmd.value = config.value.server_cmd
    }

    initialSerialized.value = JSON.stringify(serialize())
  })

function handleHash() {
    let hash = window.location.hash
    let configHash = ''
    if (hash.includes('#/config#')) {
      configHash = hash.split('#/config#')[1]
    } else if (hash.includes('#/config')) {
      return
    }

    if (configHash) {
      let stripped_hash = configHash

      tabs.value.forEach(tab => {
        Object.keys(tab.options).forEach(key => {
          if (tab.id === stripped_hash || key === stripped_hash) {
            currentTab.value = tab.id
          }
          if (key === stripped_hash) {
            setTimeout(() => {
              let element = document.getElementById(stripped_hash)
              if (element) {
                element.scrollIntoView()
              }
            }, 2000)
          }

          if (currentTab.value === tab.id) {
            return true
          }
        })
      })
    }
  }

onMounted(() => {
  handleHash()
  window.addEventListener("hashchange", handleHash)
})

watch(searchQuery, (value) => {
  if (value && matchingTabs.value.length > 0) {
    currentTab.value = matchingTabs.value[0].id
  }
})

onUnmounted(() => {
  window.removeEventListener("hashchange", handleHash)
})
</script>

<style scoped>
.config-page {
  padding: 1em 0;
}
</style>
