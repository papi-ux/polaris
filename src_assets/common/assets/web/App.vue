<template>
  <SpaceParticles v-if="!showNav" :dense="true" />
  <div v-if="showNav" class="flex h-screen bg-background text-foreground relative z-[1]">
    <!-- Mobile overlay -->
    <div v-if="sidebarOpen" class="fixed inset-0 bg-void/70 backdrop-blur-sm z-30 md:hidden transition-opacity" @click="sidebarOpen = false"></div>

    <!-- Sidebar -->
    <aside
      class="bg-void border-r border-storm flex flex-col shrink-0 transition-all duration-200 ease-in-out z-40"
      :class="[
        sidebarOpen ? 'translate-x-0' : '-translate-x-full md:translate-x-0',
        sidebarCollapsed ? 'w-16' : 'w-64',
        'fixed inset-y-0 left-0 md:static'
      ]"
    >
      <div class="p-4 border-b border-storm">
        <h1 class="text-xl font-bold text-ice flex items-center gap-2" :class="{ 'justify-center': sidebarCollapsed }">
          <img src="/images/logo-polaris.svg" class="h-8" alt="Polaris">
          <span v-if="!sidebarCollapsed">Polaris</span>
        </h1>
      </div>
      <nav class="flex-1 p-3 space-y-1">
        <router-link v-for="(item, idx) in navItems" :key="item.to" :to="item.to" class="sidebar-link" active-class="active" :title="sidebarCollapsed ? item.label : ''" @click="sidebarOpen = false">
          <div v-html="item.icon" class="w-5 h-5 shrink-0"></div>
          <span v-if="!sidebarCollapsed">{{ item.label }}</span>
          <kbd v-if="!sidebarCollapsed" class="ml-auto text-xs text-storm/50 hidden lg:inline">{{ idx + 1 }}</kbd>
        </router-link>
      </nav>
      <div class="px-3 mb-1">
        <button
          @click="currentTheme = currentTheme === 'oled' ? 'polaris' : 'oled'; setTheme(currentTheme)"
          class="flex items-center gap-2 w-full px-3 py-2 rounded-lg text-sm transition-colors"
          :class="currentTheme === 'oled' ? 'text-ice bg-ice/10' : 'text-storm hover:text-silver hover:bg-twilight/50'"
          :title="currentTheme === 'oled' ? t('navbar.theme_switch_polaris') : t('navbar.theme_switch_oled')"
        >
          <svg v-if="currentTheme === 'oled'" class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M20.354 15.354A9 9 0 018.646 3.646 9.003 9.003 0 0012 21a9.003 9.003 0 008.354-5.646z"/></svg>
          <svg v-else class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 3v1m0 16v1m9-9h-1M4 12H3m15.364 6.364l-.707-.707M6.343 6.343l-.707-.707m12.728 0l-.707.707M6.343 17.657l-.707.707M16 12a4 4 0 11-8 0 4 4 0 018 0z"/></svg>
          <span v-if="!sidebarCollapsed">{{ currentTheme === 'oled' ? t('navbar.theme_oled') : t('navbar.theme_polaris') }}</span>
        </button>
      </div>
      <button
        class="flex items-center justify-center p-2 mx-3 mb-2 rounded-lg text-storm hover:text-silver hover:bg-twilight/50 transition-colors"
        @click="toggleCollapse"
        :title="sidebarCollapsed ? 'Expand sidebar' : 'Collapse sidebar'"
      >
        <svg class="w-4 h-4 transition-transform" :class="{ 'rotate-180': sidebarCollapsed }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 19l-7-7 7-7m8 14l-7-7 7-7"/></svg>
      </button>
      <div class="p-3 border-t border-storm text-sm text-storm" :class="{ 'text-center': sidebarCollapsed }">
        <template v-if="!sidebarCollapsed">
          v{{ appVersion }} &middot; Polaris
          <div class="text-xs text-storm/50 mt-1">{{ paletteShortcut }}</div>
        </template>
        <template v-else>
          <div class="text-xs">v{{ compactVersion }}</div>
        </template>
      </div>
    </aside>

    <!-- Main content -->
    <main class="flex-1 overflow-auto bg-background relative">
      <SpaceParticles :dense="false" :absolute="true" />
      <!-- Mobile header -->
      <div class="flex items-center gap-3 p-4 border-b border-storm md:hidden">
        <button @click="sidebarOpen = !sidebarOpen" class="text-silver hover:text-ice transition-colors">
          <svg class="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6h16M4 12h16M4 18h16"/></svg>
        </button>
        <div class="min-w-0">
          <h1 class="text-lg font-bold text-ice flex items-center gap-2">
            <img src="/images/logo-polaris.svg" class="h-6" alt="Polaris">
            Polaris
          </h1>
          <div class="text-xs text-storm truncate">{{ currentPageLabel }}</div>
        </div>
        <div class="ml-auto px-2 py-1 rounded-full text-[10px] font-medium border border-storm/40 text-storm">
          {{ route.meta?.section || 'Core' }}
        </div>
      </div>

      <div class="max-w-7xl mx-auto p-6 relative z-[1]">
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

const navItems = computed(() => [
  { to: '/', icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 5a1 1 0 011-1h4a1 1 0 011 1v5a1 1 0 01-1 1H5a1 1 0 01-1-1V5zm10 0a1 1 0 011-1h4a1 1 0 011 1v2a1 1 0 01-1 1h-4a1 1 0 01-1-1V5zM4 14a1 1 0 011-1h4a1 1 0 011 1v2a1 1 0 01-1 1H5a1 1 0 01-1-1v-2zm10 0a1 1 0 011-1h4a1 1 0 011 1v5a1 1 0 01-1 1h-4a1 1 0 01-1-1v-5z"/></svg>', label: i18nReady ? t('navbar.dashboard') : 'Dashboard', section: 'Status' },
  { to: '/info', icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>', label: i18nReady ? t('navbar.home') : 'Home', section: 'Info' },
  { to: '/apps', icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2V6zm10 0a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2V6zM4 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2v-2zm10 0a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2v-2z"/></svg>', label: i18nReady ? t('navbar.applications') : 'Applications', section: 'Library' },
  { to: '/pin', icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z"/></svg>', label: i18nReady ? t('navbar.pin') : 'Clients & Pairing', section: 'Devices' },
  { to: '/config', icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.066 2.573c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.573 1.066c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.066-2.573c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/></svg>', label: i18nReady ? t('navbar.configuration') : 'Configuration', section: 'Settings' },
  { to: '/password', icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 15v2m-6 4h12a2 2 0 002-2v-6a2 2 0 00-2-2H6a2 2 0 00-2 2v6a2 2 0 002 2zm10-10V7a4 4 0 00-8 0v4h8z"/></svg>', label: i18nReady ? t('navbar.password') : 'Password', section: 'Security' },
  { to: '/troubleshooting', icon: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 9l3 3-3 3m5 0h3M5 20h14a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z"/></svg>', label: i18nReady ? t('navbar.troubleshoot') : 'Troubleshooting', section: 'Support' },
])
const currentPageLabel = computed(() => navItems.value.find((item) => item.to === route.path)?.label || 'Polaris')
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
         transition-all duration-200 relative
         hover:bg-twilight/60 hover:text-silver;
}
.sidebar-link.active,
.sidebar-link.router-link-exact-active {
  @apply bg-twilight/80 text-ice;
  box-shadow: inset 3px 0 0 var(--color-ice), var(--shadow-inset-glow);
}
</style>
