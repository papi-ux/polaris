<template>
  <SpaceParticles v-if="!showNav" :dense="true" />
  <div v-if="showNav" class="relative z-[1] flex h-screen bg-background text-foreground">
    <!-- Mobile overlay -->
    <div v-if="sidebarOpen" class="fixed inset-0 bg-void/70 backdrop-blur-sm z-30 md:hidden transition-opacity" @click="sidebarOpen = false"></div>

    <!-- Sidebar -->
    <aside
      class="bg-void border-r border-storm flex flex-col shrink-0 transition-[width,transform] duration-200 ease-in-out z-40"
      :class="[
        sidebarOpen ? 'translate-x-0' : '-translate-x-full md:translate-x-0',
        sidebarCollapsed ? 'w-16' : 'w-64',
        'fixed inset-y-0 left-0 md:static'
      ]"
    >
      <div class="border-b border-storm/20 px-4 py-4">
        <h1 class="flex items-center gap-2 text-xl font-bold text-ice" :class="{ 'justify-center': sidebarCollapsed }">
          <img src="/images/logo-polaris.svg" class="h-8" alt="Polaris">
          <div v-if="!sidebarCollapsed" class="min-w-0">
            <div>Polaris</div>
            <div class="mt-0.5 text-[10px] font-medium uppercase tracking-[0.2em] text-storm/80">Host Console</div>
          </div>
        </h1>
      </div>
      <nav class="flex-1 overflow-y-auto p-3 scrollbar-hidden">
        <div
          v-for="(section, sectionIndex) in navSections"
          :key="section.key"
          class="space-y-1"
          :class="sectionIndex > 0 ? 'mt-3 border-t border-storm/15 pt-3' : ''"
        >
          <div
            v-if="!sidebarCollapsed"
            class="px-3 pb-1 text-[10px] font-semibold uppercase tracking-[0.24em] text-storm/70"
          >
            {{ section.label }}
          </div>
          <router-link
            v-for="item in section.items"
            :key="item.to"
            :to="item.to"
            class="sidebar-link group"
            active-class="active"
            :title="sidebarCollapsed ? item.label : ''"
            @click="sidebarOpen = false"
          >
            <div v-html="item.icon" class="w-5 h-5 shrink-0"></div>
            <span v-if="!sidebarCollapsed">{{ item.label }}</span>
            <kbd v-if="!sidebarCollapsed" class="ml-auto hidden text-[11px] text-storm/40 transition-opacity duration-150 lg:inline group-hover:opacity-70 group-focus-within:opacity-70" :class="$route.path === item.to ? 'opacity-70' : 'opacity-0'">
              {{ item.shortcut }}
            </kbd>
          </router-link>
        </div>
      </nav>
      <div class="px-3 mb-1">
        <button
          type="button"
          @click="currentTheme = currentTheme === 'oled' ? 'polaris' : 'oled'; setTheme(currentTheme)"
          class="focus-ring flex w-full items-center gap-2 rounded-lg px-3 py-2 text-sm transition-[background-color,color,border-color] duration-200"
          :class="currentTheme === 'oled' ? 'text-ice bg-ice/10' : 'text-storm hover:text-silver hover:bg-twilight/50'"
          :title="currentTheme === 'oled' ? t('navbar.theme_switch_polaris') : t('navbar.theme_switch_oled')"
        >
          <svg v-if="currentTheme === 'oled'" class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M20.354 15.354A9 9 0 018.646 3.646 9.003 9.003 0 0012 21a9.003 9.003 0 008.354-5.646z"/></svg>
          <svg v-else class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 3v1m0 16v1m9-9h-1M4 12H3m15.364 6.364l-.707-.707M6.343 6.343l-.707-.707m12.728 0l-.707.707M6.343 17.657l-.707.707M16 12a4 4 0 11-8 0 4 4 0 018 0z"/></svg>
          <span v-if="!sidebarCollapsed">{{ currentTheme === 'oled' ? t('navbar.theme_oled') : t('navbar.theme_polaris') }}</span>
        </button>
      </div>
      <div class="px-3 mb-1">
        <button
          type="button"
          class="focus-ring flex w-full items-center gap-2 rounded-lg border border-storm/20 bg-deep/30 px-3 py-2 text-left text-sm text-silver transition-[background-color,border-color,color] duration-200 hover:border-ice/25 hover:bg-twilight/35 hover:text-ice"
          :title="paletteShortcut"
          @click="commandPaletteOpen = true"
        >
          <svg class="h-4 w-4 shrink-0 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m21 21-4.35-4.35M10.5 18a7.5 7.5 0 1 1 0-15 7.5 7.5 0 0 1 0 15Z"/></svg>
          <template v-if="!sidebarCollapsed">
            <div class="min-w-0 flex-1">
              <div class="truncate font-medium">Command Palette</div>
              <div class="mt-0.5 text-[11px] text-storm">Jump between pages and host actions.</div>
            </div>
            <kbd class="rounded-md border border-storm/20 bg-void/60 px-2 py-1 text-[11px] text-storm">
              {{ isMac ? '\u2318K' : 'Ctrl+K' }}
            </kbd>
          </template>
          <kbd v-else class="mx-auto rounded-md border border-storm/20 bg-void/60 px-2 py-1 text-[11px] text-storm">
            K
          </kbd>
        </button>
      </div>
      <button
        type="button"
        class="focus-ring mx-3 mb-2 flex items-center justify-center rounded-lg p-2 text-storm transition-[background-color,color] duration-200 hover:bg-twilight/50 hover:text-silver"
        @click="toggleCollapse"
        :title="sidebarCollapsed ? 'Expand sidebar' : 'Collapse sidebar'"
      >
        <svg class="w-4 h-4 transition-transform" :class="{ 'rotate-180': sidebarCollapsed }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 19l-7-7 7-7m8 14l-7-7 7-7"/></svg>
      </button>
      <div class="border-t border-storm/20 p-3 text-sm text-storm" :class="{ 'text-center': sidebarCollapsed }">
        <template v-if="!sidebarCollapsed">
          <div>v{{ appVersion }} &middot; Polaris</div>
          <div class="mt-1 text-xs text-storm/50">{{ paletteShortcut }}</div>
        </template>
        <template v-else>
          <div class="text-xs">v{{ compactVersion }}</div>
        </template>
      </div>
    </aside>

    <!-- Main content -->
    <main class="relative flex-1 overflow-auto overflow-x-hidden bg-background">
      <SpaceParticles :dense="false" :absolute="true" />
      <!-- Mobile header -->
      <div class="flex items-center gap-3 border-b border-storm/20 px-4 py-4 md:hidden">
        <button type="button" aria-label="Toggle navigation" @click="sidebarOpen = !sidebarOpen" class="focus-ring text-silver transition-colors duration-200 hover:text-ice">
          <svg class="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6h16M4 12h16M4 18h16"/></svg>
        </button>
        <div class="min-w-0">
          <h1 class="text-lg font-bold text-ice flex items-center gap-2">
            <img src="/images/logo-polaris.svg" class="h-6" alt="Polaris">
            Polaris
          </h1>
          <div class="text-xs text-storm truncate">{{ currentPageLabel }}</div>
        </div>
        <div class="ml-auto rounded-full border border-storm/30 bg-deep/60 px-2.5 py-1 text-[10px] font-medium text-storm">
          {{ currentPageSection }}
        </div>
      </div>

      <div class="relative z-[1] mx-auto max-w-[1360px] px-5 py-5 md:px-6 md:py-6">
        <div :key="pageKey" class="page-enter">
          <router-view :key="$route.path" />
        </div>
      </div>
    </main>
    <CommandPalette v-model="commandPaletteOpen" />
    <Toast />
  </div>
  <div v-else>
    <router-view />
    <Toast />
  </div>
</template>

<script setup>
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import CommandPalette from './CommandPalette.vue'
import Toast from './components/Toast.vue'
import SpaceParticles from './components/SpaceParticles.vue'
import { initTheme, getTheme, setTheme as setThemeFn } from './theme.js'

const route = useRoute()
const currentTheme = ref(getTheme())
function setTheme(t) { setThemeFn(t); currentTheme.value = t }
const router = useRouter()
const commandPaletteOpen = ref(false)
const sidebarOpen = ref(false)
const sidebarCollapsed = ref(localStorage.getItem('sidebarCollapsed') === 'true')
const appVersion = ref('1.0.0')
const isMac = navigator.platform.toUpperCase().indexOf('MAC') >= 0

let i18nReady = false
let t = (k) => k
try {
  const i18n = useI18n()
  t = i18n.t
  i18nReady = true
} catch (e) {
  // i18n not yet ready
}

const navSections = computed(() => {
  const sections = [
    {
      key: 'streaming',
      label: i18nReady ? t('navbar.group_streaming') : 'Streaming',
      items: [
        {
          to: '/',
          icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 5a1 1 0 011-1h4a1 1 0 011 1v5a1 1 0 01-1 1H5a1 1 0 01-1-1V5zm10 0a1 1 0 011-1h4a1 1 0 011 1v2a1 1 0 01-1 1h-4a1 1 0 01-1-1V5zM4 14a1 1 0 011-1h4a1 1 0 011 1v2a1 1 0 01-1 1H5a1 1 0 01-1-1v-2zm10 0a1 1 0 011-1h4a1 1 0 011 1v5a1 1 0 01-1 1h-4a1 1 0 01-1-1v-5z"/></svg>',
          label: i18nReady ? t('navbar.dashboard') : 'Dashboard',
        },
        {
          to: '/apps',
          icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2V6zm10 0a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2V6zM4 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2v-2zm10 0a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2v-2z"/></svg>',
          label: i18nReady ? t('navbar.library') : 'Library',
        },
        {
          to: '/pin',
          icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z"/></svg>',
          label: i18nReady ? t('navbar.pairing') : 'Pairing',
        },
        {
          to: '/browser-stream',
          icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M7 7h10a4 4 0 014 4v2a4 4 0 01-4 4H7a4 4 0 01-4-4v-2a4 4 0 014-4z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12h6m-2-2l2 2-2 2"/></svg>',
          label: i18nReady ? t('navbar.browser_stream') : 'Browser Stream',
        },
      ],
    },
    {
      key: 'host',
      label: i18nReady ? t('navbar.group_host') : 'Host',
      items: [
        {
          to: '/config',
          icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.066 2.573c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.573 1.066c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.066-2.573c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/></svg>',
          label: i18nReady ? t('navbar.settings') : 'Settings',
        },
        {
          to: '/password',
          icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 15v2m-6 4h12a2 2 0 002-2v-6a2 2 0 00-2-2H6a2 2 0 00-2 2v6a2 2 0 002 2zm10-10V7a4 4 0 00-8 0v4h8z"/></svg>',
          label: i18nReady ? t('navbar.security') : 'Security',
        },
      ],
    },
    {
      key: 'support',
      label: i18nReady ? t('navbar.group_support') : 'Support',
      items: [
        {
          to: '/info',
          icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>',
          label: i18nReady ? t('navbar.system') : 'System',
        },
        {
          to: '/troubleshooting',
          icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 9l3 3-3 3m5 0h3M5 20h14a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z"/></svg>',
          label: i18nReady ? t('navbar.troubleshoot') : 'Troubleshooting',
        },
      ],
    },
  ]

  let shortcut = 1
  return sections.map((section) => ({
    ...section,
    items: section.items.map((item) => ({
      ...item,
      shortcut: shortcut++,
      sectionLabel: section.label,
    })),
  }))
})
const navItems = computed(() => navSections.value.flatMap((section) => section.items))
const currentNavItem = computed(() => navItems.value.find((item) => item.to === route.path) || null)
const currentPageLabel = computed(() => currentNavItem.value?.label || 'Polaris')
const currentPageSection = computed(() => currentNavItem.value?.sectionLabel || 'Core')
const paletteShortcut = computed(() => `${isMac ? '\u2318' : 'Ctrl+'}K ${i18nReady ? t('navbar.search_hint') : 'to search'}`)
const compactVersion = computed(() => appVersion.value.split('.')[0] || '1')
const authRoutes = new Set(['/login', '/welcome', '/recover'])

const showNav = computed(() => {
  return !authRoutes.has(route.path)
})

function toggleCollapse() {
  sidebarCollapsed.value = !sidebarCollapsed.value
  localStorage.setItem('sidebarCollapsed', sidebarCollapsed.value)
}

function handleKeydown(e) {
  // Command palette
  if ((e.ctrlKey || e.metaKey) && e.key === 'k') {
    e.preventDefault()
    commandPaletteOpen.value = !commandPaletteOpen.value
    return
  }

  // Escape to close modals/sidebar
  if (e.key === 'Escape') {
    if (commandPaletteOpen.value) {
      commandPaletteOpen.value = false
      return
    }
    if (sidebarOpen.value) {
      sidebarOpen.value = false
      return
    }
  }

  // Number keys 1-7 for nav (only when not in an input)
  const tag = document.activeElement?.tagName?.toLowerCase()
  if (tag === 'input' || tag === 'textarea' || tag === 'select') return
  if (commandPaletteOpen.value) return

  const num = parseInt(e.key)
  if (num >= 1 && num <= navItems.value.length) {
    router.push(navItems.value[num - 1].to)
  }
}

// Page enter animation on route change
const pageKey = ref(0)
watch(() => route.path, () => { pageKey.value++ })
watch(() => route.path, () => { sidebarOpen.value = false })

onMounted(() => {
  initTheme()
  window.addEventListener('keydown', handleKeydown)
  fetch('./api/config', { credentials: 'include' })
    .then((response) => response.ok ? response.json() : null)
    .then((data) => {
      if (data?.version) {
        appVersion.value = data.version
      }
    })
    .catch(() => {})
})

onUnmounted(() => {
  window.removeEventListener('keydown', handleKeydown)
})
</script>

<style>
@reference "./app.css";

.sidebar-link {
  @apply flex items-center gap-3 px-3 py-2 rounded-lg text-storm no-underline
         transition-[background-color,color,box-shadow] duration-200 relative
         hover:bg-twilight/60 hover:text-silver;
}
.sidebar-link.active,
.sidebar-link.router-link-exact-active {
  @apply bg-twilight/80 text-ice;
  box-shadow: inset 2px 0 0 rgba(200, 214, 229, 0.85), var(--shadow-inset-glow);
}
</style>
