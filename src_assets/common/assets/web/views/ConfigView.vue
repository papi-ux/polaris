<template>
  <div class="page-shell">
    <section class="page-header settings-page-header sticky top-4 z-20">
      <div class="settings-toolbar-copy">
        <div class="settings-toolbar-title-row">
          <h1 class="page-title">{{ $t('navbar.settings') }}</h1>
          <span class="meta-pill">{{ commandCenterTitle }}</span>
          <span
            v-if="hasUnsavedChanges"
            class="meta-pill border-amber-300/40 bg-amber-300/10 text-amber-300"
          >
            {{ $t('config.pending_badge') }}
          </span>
          <span
            v-else
            class="meta-pill border-green-400/30 bg-green-400/10 text-green-300"
          >
            {{ $t('config.synced_badge') }}
          </span>
          <span v-if="config && searchQuery" class="meta-pill">{{ searchSummary }}</span>
        </div>
      </div>

      <div class="settings-toolbar-actions">
        <input
          v-model="searchQuery"
          type="text"
          autocomplete="off"
          :placeholder="$t('config.search_placeholder')"
          class="focus-ring settings-search-input"
          @input="onSearch"
        >

        <div class="settings-command-actions">
          <button class="focus-ring settings-action-button settings-action-button-secondary" @click="resetLocalChanges" :disabled="!hasUnsavedChanges || saving || restarting">
            {{ $t('config.reset_local') }}
          </button>
          <button class="focus-ring settings-action-button settings-action-button-primary" @click="save" :disabled="!config || !hasUnsavedChanges || saving || restarting" tabindex="0">
            {{ saving ? $t('config.saving') : $t('_common.save') }}
          </button>
          <button class="focus-ring settings-action-button settings-action-button-accent" @click="apply" :disabled="!config || saving || restarting || (!saved && !hasUnsavedChanges)" tabindex="0">
            {{ restarting ? $t('config.restarting') : $t('_common.apply') }}
          </button>
        </div>
      </div>
    </section>

  <!-- Skeleton while loading -->
  <div v-if="!config" class="space-y-4">
    <Skeleton type="text" />
    <Skeleton type="card" />
    <Skeleton type="card" />
    <Skeleton type="card" />
  </div>

  <div v-if="config" class="settings-workspace" :class="{ 'is-searching': !!searchQuery }">
    <aside class="settings-sidebar">
      <div class="section-kicker">{{ $t('config.settings_map_title') }}</div>

      <div class="settings-nav-groups">
        <div v-for="group in tabGroups" :key="group.id" class="settings-nav-group">
          <div class="settings-nav-group-title">{{ group.label }}</div>
          <div class="settings-nav-list">
            <button
              v-for="item in group.items"
              :key="item.id"
              class="focus-ring settings-nav-button"
              :class="{ 'is-active': item.id === activeNavId }"
              @click="selectNavTab(item.id)"
              :tabindex="0"
            >
              <span class="settings-nav-name">{{ item.name }}</span>
            </button>
          </div>
        </div>
      </div>
    </aside>

    <div class="settings-main">
      <div v-if="mobileNavItems.length" class="settings-mobile-tabs">
        <button
          v-for="item in mobileNavItems"
          :key="item.id"
          class="focus-ring settings-mobile-tab"
          :class="{ 'is-active': item.id === activeNavId }"
          @click="selectNavTab(item.id)"
          :tabindex="0"
        >
          {{ item.name }}
        </button>
      </div>

      <section v-if="searchQuery" class="settings-search-strip">
        <div class="settings-search-strip-copy">
          <div class="settings-search-strip-title">
            {{ searchHasResults ? $t('config.search_focus_title') : $t('config.search_empty_title') }}
          </div>
          <div class="settings-search-strip-note">
            {{ searchHasResults ? searchSummary : searchEmptyCopy }}
          </div>
        </div>
        <div class="shrink-0">
          <button class="focus-ring settings-action-button settings-action-button-secondary" @click="clearSearch">
            {{ $t('config.clear_search') }}
          </button>
        </div>
      </section>

      <section v-if="currentTabIsEncoder" class="settings-subnav-card">
        <div class="settings-subnav-row">
          <button
            v-for="tab in encoderTabs"
            :key="tab.id"
            class="focus-ring settings-subnav-button"
            :class="{ 'is-active': tab.id === currentTab }"
            @click="currentTab = tab.id"
            :tabindex="0"
          >
            {{ tab.name }}
          </button>
        </div>
      </section>

      <div v-if="!searchQuery || searchHasResults" ref="settingsContentRef" class="settings-tab-content">
        <general
          v-if="shouldShowTab('general')"
          :config="config"
          :global-prep-cmd="global_prep_cmd"
          :global-state-cmd="global_state_cmd"
          :server-cmd="server_cmd"
          :platform="platform">
        </general>

        <inputs
          v-if="shouldShowTab('input')"
          :config="config"
          :platform="platform">
        </inputs>

        <audio-video
          v-if="shouldShowTab('av')"
          :config="config"
          :platform="platform"
          :vdisplay="vdisplayStatus"
        >
        </audio-video>

        <network
          v-if="shouldShowTab('network')"
          :config="config"
          :platform="platform">
        </network>

        <files
          v-if="shouldShowTab('files')"
          :config="config"
          :platform="platform">
        </files>

        <advanced
          v-if="shouldShowTab('advanced')"
          :config="config"
          :platform="platform">
        </advanced>

        <ai-optimizer
          v-if="shouldShowTab('ai')"
          :config="config">
        </ai-optimizer>

        <container-encoders
          :current-tab="searchQuery ? currentTab : currentTab"
          :config="config"
          :platform="platform">
        </container-encoders>
      </div>
    </div>
  </div>

  <div class="h-4"></div>
  </div>
</template>

<script setup>
import { ref, computed, provide, onMounted, inject, watch, onUnmounted, nextTick } from 'vue'
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
const settingsContentRef = ref(null)
let searchHighlightTimeout = null
let highlightedSearchTarget = null
const writeOnlySecrets = {
  ai_api_key: { presentFlag: 'has_ai_api_key', clearFlag: 'clear_ai_api_key' },
  steamgriddb_api_key: { presentFlag: 'has_steamgriddb_api_key', clearFlag: 'clear_steamgriddb_api_key' },
}
const tabs = ref([
  {
    id: "general",
    name: "General",
    group: "core",
    groupLabel: "Core Setup",
    summary: "Host identity, tray behavior, metadata, and automation.",
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
    group: "core",
    groupLabel: "Core Setup",
    summary: "Controller, keyboard, mouse, cursor, and touch behavior.",
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
    group: "core",
    groupLabel: "Core Setup",
    summary: "Audio routing, displays, capture path, and stream delivery.",
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
    group: "core",
    groupLabel: "Core Setup",
    summary: "Discovery, pairing access, ports, encryption, and trusted networks.",
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
    group: "host",
    groupLabel: "Host & Runtime",
    summary: "Apps, logs, certificates, credentials, and runtime paths.",
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
    group: "host",
    groupLabel: "Host & Runtime",
    summary: "Compatibility flags, codec advertising, capture path, and encoder choice.",
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
    group: "host",
    groupLabel: "Host & Runtime",
    summary: "Provider, runtime, cache, and optimization defaults.",
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
    group: "encoders",
    groupLabel: "Encoder Profiles",
    summary: "Latency, bitrate shaping, AQ, and NVIDIA tuning.",
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
    group: "encoders",
    groupLabel: "Encoder Profiles",
    summary: "Preset, entropy coding, and HEVC fallback behavior.",
    options: {
      "qsv_preset": "medium",
      "qsv_coder": "auto",
      "qsv_slow_hevc": "disabled",
    },
  },
  {
    id: "amd",
    name: "AMD AMF Encoder",
    group: "encoders",
    groupLabel: "Encoder Profiles",
    summary: "Rate control, quality, VBAQ, and AMD tuning.",
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
    group: "encoders",
    groupLabel: "Encoder Profiles",
    summary: "Entropy coding, realtime mode, and software fallback.",
    options: {
      "vt_coder": "auto",
      "vt_software": "auto",
      "vt_realtime": "enabled",
    },
  },
  {
    id: "vaapi",
    name: "VA-API Encoder",
    group: "encoders",
    groupLabel: "Encoder Profiles",
    summary: "Strict rate-control behavior for AMD and Intel GPUs.",
    options: {
      "vaapi_strict_rc_buffer": "disabled",
    },
  },
  {
    id: "sw",
    name: "Software Encoder",
    group: "encoders",
    groupLabel: "Encoder Profiles",
    summary: "CPU preset and tune selection for fallback paths.",
    options: {
      "sw_preset": "superfast",
      "sw_tune": "zerolatency",
    },
  },
])

provide('platform', computed(() => platform.value))

const activeTabMeta = computed(() => tabs.value.find(tab => tab.id === currentTab.value))
const encoderTabs = computed(() => tabs.value.filter(tab => tab.group === 'encoders'))
const currentTabIsEncoder = computed(() => activeTabMeta.value?.group === 'encoders')
const activeNavId = computed(() => currentTabIsEncoder.value ? 'encoders' : currentTab.value)
function normalizeSearchValue(value) {
  return String(value || '').toLowerCase().trim()
}

function translatedSearchText(key) {
  const translated = i18n?.t?.(key)
  if (typeof translated !== 'string') return ''
  return normalizeSearchValue(translated === key ? '' : translated)
}

function optionSearchText(optionKey) {
  return [
    normalizeSearchValue(optionKey),
    translatedSearchText(`config.${optionKey}`),
    translatedSearchText(`config.${optionKey}_desc`)
  ]
    .filter(Boolean)
    .join(' ')
}

const searchResults = computed(() => {
  const query = normalizeSearchValue(searchQuery.value)
  if (!query) {
    return tabs.value.map(tab => ({ tab, firstOptionKey: null }))
  }

  return tabs.value
    .map((tab) => {
      const tabText = [
        normalizeSearchValue(tab.name),
        normalizeSearchValue(tab.summary),
        normalizeSearchValue(tab.groupLabel)
      ].join(' ')

      const matchedOptionKeys = Object.keys(tab.options).filter((optionKey) =>
        optionSearchText(optionKey).includes(query)
      )

      if (!tabText.includes(query) && matchedOptionKeys.length === 0) {
        return null
      }

      return {
        tab,
        firstOptionKey: matchedOptionKeys[0] || Object.keys(tab.options)[0] || null
      }
    })
    .filter(Boolean)
})

const matchingTabs = computed(() => searchResults.value.map((entry) => entry.tab))
const matchedOptionByTab = computed(() => Object.fromEntries(
  searchResults.value.map((entry) => [entry.tab.id, entry.firstOptionKey])
))
const tabGroups = computed(() => {
  const order = [
    { id: 'core', label: 'Core Setup' },
    { id: 'host', label: 'Host & Runtime' },
  ]

  return order
    .map(group => ({
      ...group,
      items: matchingTabs.value
        .filter(tab => tab.group === group.id)
        .map(tab => ({ id: tab.id, name: tab.name })),
    }))
    .filter(group => group.items.length > 0)
    .concat(
      encoderTabs.value.some(tab => matchingTabs.value.some(match => match.id === tab.id))
        ? [{
            id: 'encoders',
            label: 'Encoder Profiles',
            items: [{ id: 'encoders', name: 'Encoder Profiles' }],
          }]
        : []
    )
})
const mobileNavItems = computed(() => tabGroups.value.flatMap(group => group.items))
const searchSummary = computed(() => i18n.t('config.search_results', { query: searchQuery.value, count: matchingTabs.value.length }))
const searchHasResults = computed(() => matchingTabs.value.length > 0)
const hasUnsavedChanges = computed(() => {
  if (!config.value || !initialSerialized.value) return false
  return JSON.stringify(serialize()) !== initialSerialized.value
})
const activePanelTitle = computed(() => currentTabIsEncoder.value ? 'Encoder Profiles' : (activeTabMeta.value?.name || 'General'))
const commandCenterTitle = computed(() => {
  if (searchQuery.value && !searchHasResults.value) return i18n.t('config.search_empty_heading')
  if (searchQuery.value) return i18n.t('config.search_focus_heading')
  return activePanelTitle.value
})
const searchEmptyCopy = computed(() => i18n.t('config.search_empty_desc', { query: searchQuery.value }))

function shouldShowTab(tabId) {
  if (searchQuery.value) {
    return matchingTabs.value.some(tab => tab.id === tabId)
  }
  return currentTab.value === tabId
}

function onSearch() {
  if (!searchQuery.value) {
    clearSearchHighlight()
  }
}

function clearSearch() {
  searchQuery.value = ''
  clearSearchHighlight()
}

function escapeSelector(value) {
  if (typeof window !== 'undefined' && window.CSS?.escape) {
    return window.CSS.escape(value)
  }
  return String(value).replace(/["\\#.:()[\]]/g, '\\$&')
}

function clearSearchHighlight() {
  if (highlightedSearchTarget) {
    highlightedSearchTarget.classList.remove('settings-search-hit')
    highlightedSearchTarget = null
  }
  if (searchHighlightTimeout) {
    clearTimeout(searchHighlightTimeout)
    searchHighlightTimeout = null
  }
}

function resolveSearchTarget(optionKey) {
  if (!settingsContentRef.value || !optionKey) return null
  const escaped = escapeSelector(optionKey)
  const root = settingsContentRef.value
  const directMatch = root.querySelector(`[data-setting-key="${optionKey}"]`)
  if (directMatch) return directMatch

  const field = root.querySelector(`#${escaped}`)
  if (field) {
    return (
      field.closest('[data-setting-key]') ||
      field.closest('.settings-search-block') ||
      field.parentElement ||
      field
    )
  }

  const label = root.querySelector(`label[for="${escaped}"]`)
  if (label) {
    return label.closest('[data-setting-key]') || label.parentElement || label
  }

  return null
}

async function focusSearchTarget(optionKey) {
  if (!searchQuery.value || !optionKey) return
  await nextTick()
  const target = resolveSearchTarget(optionKey)
  if (!target) return

  clearSearchHighlight()
  highlightedSearchTarget = target
  target.classList.add('settings-search-hit')
  target.scrollIntoView({ behavior: 'smooth', block: 'center', inline: 'nearest' })

  searchHighlightTimeout = window.setTimeout(() => {
    clearSearchHighlight()
  }, 2200)
}

function selectNavTab(tabId) {
  if (tabId === 'encoders') {
    currentTab.value = currentTabIsEncoder.value
      ? currentTab.value
      : (encoderTabs.value[0]?.id || currentTab.value)
    return
  }
  currentTab.value = tabId
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

function finalizeWriteOnlySecrets(configCopy) {
  Object.entries(writeOnlySecrets).forEach(([secretKey, meta]) => {
    const hasStoredSecret = !!config.value?.[meta.presentFlag]
    const clearRequested = !!config.value?.[meta.clearFlag]
    const nextValue = configCopy[secretKey]

    if (clearRequested) {
      configCopy[secretKey] = ''
      return
    }

    if (hasStoredSecret && !nextValue) {
      delete configCopy[secretKey]
    }
  })
}

function stripClientOnlyConfigFields(configCopy) {
  Object.values(writeOnlySecrets).forEach((meta) => {
    delete configCopy[meta.presentFlag]
    delete configCopy[meta.clearFlag]
  })
  delete configCopy.has_api_key
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

  finalizeWriteOnlySecrets(configCopy)
  stripClientOnlyConfigFields(configCopy)

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
    config.value.trusted_subnet_auto_pairing ??= "disabled"
    config.value.has_ai_api_key = !!config.value.has_ai_api_key
    config.value.has_steamgriddb_api_key = !!config.value.has_steamgriddb_api_key
    config.value.has_api_key = !!config.value.has_api_key
    config.value.clear_ai_api_key = false
    config.value.clear_steamgriddb_api_key = false

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
              focusSearchTarget(stripped_hash)
            }, 400)
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

watch(searchQuery, async (value) => {
  if (value && searchResults.value.length > 0) {
    currentTab.value = searchResults.value[0].tab.id
    await focusSearchTarget(searchResults.value[0].firstOptionKey)
    return
  }

  clearSearchHighlight()
})

watch(currentTab, async (value) => {
  if (!searchQuery.value) return
  await focusSearchTarget(matchedOptionByTab.value[value])
})

onUnmounted(() => {
  clearSearchHighlight()
  window.removeEventListener("hashchange", handleHash)
})
</script>

<style scoped>
</style>
