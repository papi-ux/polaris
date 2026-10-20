<template>
  <SpaceParticles v-if="!showNav" :dense="true" />
  <!-- The shell is a size container: below 48rem of its own width (measured with the actual root
       font size, so enlarged text counts as well as narrow screens) it is a phone shell with a
       drawer; at or above it the sidebar stays beside the page. dvh follows mobile browser bars. -->
  <div v-if="showNav" class="@container relative z-[1] flex h-dvh bg-background text-foreground">
    <a href="#polaris-main" class="skip-link">Skip to main content</a>
    <!-- Mobile overlay -->
    <div v-if="sidebarOpen" data-sidebar-overlay class="fixed inset-0 bg-void/70 backdrop-blur-sm z-30 @3xl:hidden transition-opacity" @click="sidebarOpen = false"></div>

    <!-- Sidebar -->
    <aside
      id="polaris-sidebar"
      class="bg-void border-r border-storm flex max-w-[calc(100vw-3rem)] flex-col shrink-0 overflow-y-auto overscroll-contain scrollbar-hidden transition-[width,transform] duration-200 ease-in-out z-40"
      :class="[
        sidebarOpen ? 'translate-x-0' : '-translate-x-full @3xl:translate-x-0',
        sidebarCollapsed ? 'w-16' : 'w-64',
        'fixed inset-y-0 left-0 @3xl:static'
      ]"
    >
      <div class="border-b border-storm/20 px-4 py-4">
        <h1 class="flex items-center gap-2 text-xl font-bold text-ice leading-none mt-1" :class="{ 'justify-center': sidebarCollapsed }">
          <img src="/images/polaris-wordmark.svg?v=20260726" class="h-7 w-auto shrink-0 align-middle" alt="Polaris">
          <div v-if="!sidebarCollapsed" class="sr-only">Polaris</div>
        </h1>
      </div>
      <!-- The whole sidebar scrolls as one column, so its footer controls stay reachable on short
           landscape screens and with enlarged text. -->
      <nav class="flex-1 p-3">
        <div
          v-for="(section, sectionIndex) in navSections"
          :key="section.key"
          class="space-y-1"
          :class="sectionIndex > 0 ? 'mt-3 border-t border-storm/15 pt-3' : ''"
        >
          <div
            v-if="!sidebarCollapsed"
            class="px-3 pb-1 text-[10px] font-semibold uppercase tracking-eyebrow text-storm"
          >
            {{ section.label }}
          </div>
          <router-link
            v-for="item in section.items"
            :key="item.to"
            :to="item.to"
            :aria-label="item.badge ? `${item.label}, ${item.badge}` : item.label"
            class="sidebar-link group"
            active-class="active"
            :title="sidebarCollapsed ? `${item.label}${item.badge ? ` · ${item.badge}` : ''}` : ''"
            @click="sidebarOpen = false"
          >
            <div aria-hidden="true" v-html="item.icon" class="w-5 h-5 shrink-0"></div>
            <span v-if="!sidebarCollapsed">{{ item.label }}</span>
            <span
              v-if="!sidebarCollapsed && item.badge && section.key !== 'labs'"
              class="ml-auto rounded-full border border-warning/30 bg-warning/10 px-1.5 py-0.5 text-[9px] font-semibold uppercase tracking-eyebrow text-warning-bright"
            >
              {{ item.badge }}
            </span>
          </router-link>
        </div>
      </nav>

      <router-link
        data-sidebar-update-status
        :to="{ path: '/info', hash: '#update-center' }"
        class="focus-ring mx-3 mb-2 flex flex-wrap items-center gap-2 rounded-lg border border-storm/20 bg-deep/35 px-3 py-2 text-left text-sm text-silver no-underline transition-[background-color,border-color,color,box-shadow] duration-200 hover:border-ice/25 hover:bg-twilight/40 hover:text-ice"
        :class="{ 'justify-center px-2': sidebarCollapsed }"
        :title="sidebarUpdateTitle"
        :aria-label="sidebarUpdateTitle"
        @click="sidebarOpen = false"
      >
        <span
          data-sidebar-update-status-light
          class="h-2.5 w-2.5 shrink-0 rounded-full"
          :class="sidebarUpdateStatusLightClass"
          aria-hidden="true"
        ></span>
        <template v-if="!sidebarCollapsed">
          <span class="min-w-0 flex-auto text-xs font-medium text-silver [overflow-wrap:anywhere]">{{ sidebarUpdateLabel }}</span>
          <span class="text-[11px] text-storm">Updates</span>
        </template>
      </router-link>
      <div class="px-3">
        <ThemeToggle :collapsed="sidebarCollapsed" />
      </div>
      <div class="px-3 pb-1">
        <button
          type="button"
          data-sidebar-sign-out
          class="focus-ring flex w-full items-center gap-2 rounded-lg px-3 py-2 text-left text-sm text-storm transition-[background-color,color] duration-200 hover:bg-twilight/50 hover:text-silver disabled:cursor-not-allowed disabled:opacity-50"
          :class="{ 'justify-center px-2': sidebarCollapsed }"
          :disabled="signingOut"
          :title="signOutLabel"
          :aria-label="signOutLabel"
          @click="signOut"
        >
          <svg class="h-4 w-4 shrink-0 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15.75 9V5.25A2.25 2.25 0 0 0 13.5 3h-6A2.25 2.25 0 0 0 5.25 5.25v13.5A2.25 2.25 0 0 0 7.5 21h6a2.25 2.25 0 0 0 2.25-2.25V15m3 0 3-3m0 0-3-3m3 3H9"/></svg>
          <span v-if="!sidebarCollapsed" class="min-w-0 flex-1 font-medium [overflow-wrap:anywhere]">{{ signingOut ? signOutPendingLabel : signOutLabel }}</span>
        </button>
      </div>
      <div class="px-3 pb-2">
        <button
          type="button"
          class="focus-ring flex w-full flex-wrap items-center gap-2 rounded-lg px-3 py-2 text-left text-sm text-storm transition-[background-color,color] duration-200 hover:bg-twilight/50 hover:text-silver"
          :title="paletteShortcut"
          :aria-label="`Open command palette (${paletteShortcut})`"
          @click="commandPaletteOpen = true"
        >
          <svg class="h-4 w-4 shrink-0 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m21 21-4.35-4.35M10.5 18a7.5 7.5 0 1 1 0-15 7.5 7.5 0 0 1 0 15Z"/></svg>
          <template v-if="!sidebarCollapsed">
            <span class="min-w-0 flex-auto font-medium [overflow-wrap:anywhere]">Command Palette</span>
            <kbd class="ml-auto rounded-md border border-storm/20 bg-void/60 px-2 py-1 text-[11px] text-storm">
              {{ isMac ? '\u2318K' : 'Ctrl+K' }}
            </kbd>
          </template>
          <kbd v-else class="mx-auto rounded-md border border-storm/20 bg-void/60 px-2 py-1 text-[11px] text-storm">
            K
          </kbd>
        </button>
      </div>
      <div class="flex items-center border-t border-storm/20 px-3 py-2 text-xs text-storm" :class="sidebarCollapsed ? 'justify-center' : 'justify-between'">
        <span :title="`${displayVersion} · Polaris`">{{ sidebarCollapsed ? `v${compactVersion}` : displayVersion }}</span>
        <button
          type="button"
          class="focus-ring inline-flex items-center justify-center rounded-lg p-2 text-storm transition-[background-color,color] duration-200 hover:bg-twilight/50 hover:text-silver"
          @click="toggleCollapse"
          :title="sidebarCollapsed ? 'Expand sidebar' : 'Collapse sidebar'"
          :aria-label="sidebarCollapsed ? 'Expand sidebar' : 'Collapse sidebar'"
        >
          <svg class="w-4 h-4 transition-transform" :class="{ 'rotate-180': sidebarCollapsed }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 19l-7-7 7-7m8 14l-7-7 7-7"/></svg>
        </button>
      </div>
    </aside>

    <!-- Main content -->
    <main id="polaris-main" tabindex="-1" class="relative flex-1 overflow-auto overflow-x-hidden bg-background">
      <SpaceParticles :dense="false" :absolute="true" />
      <!-- Mobile header -->
      <!-- One compact row: a 44px menu target, the wordmark and the page name. The drawer
           already groups pages by section, so the header does not repeat it. -->
      <div data-mobile-header class="flex min-h-14 items-center gap-2 border-b border-storm/20 px-2 py-1.5 @3xl:hidden">
        <button ref="menuButton" type="button" aria-label="Toggle navigation" aria-controls="polaris-sidebar" :aria-expanded="sidebarOpen" @click="toggleSidebar" class="focus-ring inline-flex h-11 w-11 shrink-0 items-center justify-center rounded-lg text-silver transition-colors duration-200 hover:bg-twilight/50 hover:text-ice">
          <svg class="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6h16M4 12h16M4 18h16"/></svg>
        </button>
        <h1 class="flex min-w-0 max-w-[160px] items-center text-lg font-bold leading-none text-ice">
          <img src="/images/polaris-wordmark.svg?v=20260726" class="h-6 w-auto max-w-full object-contain align-middle" alt="Polaris">
        </h1>
        <span aria-hidden="true" class="text-storm/60">/</span>
        <span data-mobile-page-label class="min-w-0 truncate text-sm font-medium text-silver">{{ currentPageLabel }}</span>
      </div>

      <div class="relative z-[1] mx-auto max-w-[1360px] px-5 py-5 @3xl:px-6 @3xl:py-6">
        <SettingsUnreadableBanner v-if="settingsBannerVisible" />
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
import { ref, computed, watch, nextTick, onMounted, onUnmounted } from 'vue'
import { useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import CommandPalette from './CommandPalette.vue'
import Toast from './components/Toast.vue'
import SpaceParticles from './components/SpaceParticles.vue'
import SettingsUnreadableBanner from './components/SettingsUnreadableBanner.vue'
import ThemeToggle from './ThemeToggle.vue'
import { initTheme } from './theme.js'
import { clearCachedConfig, getCachedConfig } from './config-cache.js'
import { markWebUiUnauthenticated, webUiAuthenticated } from './auth-state.js'
import { isPublicRoute, normalizeRoutePath } from './router-helpers.js'
import { settingsUnreadable, showsSettingsBanner } from './settings-unreadable.js'
import { createNavSections, getNavItemByPath } from './nav-metadata.js'
import { buildUpdateCenterState, isPrereleaseOptIn, updateStatusLightClass } from './update-center.js'

const route = useRoute()
const commandPaletteOpen = ref(false)
const sidebarOpen = ref(false)
const sidebarCollapsed = ref(localStorage.getItem('sidebarCollapsed') === 'true')
const appVersion = ref('')
const appVersionLoading = ref(false)
const sidebarUpdateHost = ref({ platform: '', distro: {} })
const sidebarLatestRelease = ref(null)
const sidebarPrereleaseRelease = ref(null)
const sidebarNotifyPreReleases = ref(false)
const sidebarUpdateError = ref('')
const sidebarUpdateLoading = ref(false)
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

const navSections = computed(() => createNavSections(t, sidebarUpdateHost.value?.platform || ''))
const currentNavItem = computed(() => getNavItemByPath(navSections.value, route.path))
const currentPageLabel = computed(() => currentNavItem.value?.label || 'Polaris')
const paletteShortcut = computed(() => `${isMac ? '\u2318' : 'Ctrl+'}K ${i18nReady ? t('navbar.search_hint') : 'to search'}`)
const displayVersion = computed(() => appVersion.value ? `v${appVersion.value}` : 'v...')
const compactVersion = computed(() => appVersion.value ? appVersion.value.split('.')[0] : '...')
const sidebarUpdateState = computed(() => buildUpdateCenterState({
  currentVersion: appVersion.value || '',
  latestRelease: sidebarLatestRelease.value,
  prereleaseRelease: sidebarPrereleaseRelease.value,
  includePrereleases: sidebarNotifyPreReleases.value,
  host: sidebarUpdateHost.value,
}))
const sidebarUpdateLabel = computed(() => {
  if (sidebarUpdateLoading.value && !sidebarLatestRelease.value) return 'Checking…'
  if (sidebarUpdateError.value) return 'Check unavailable'
  if (sidebarUpdateState.value.status === 'update_available' && sidebarUpdateState.value.latestVersion) {
    return `${sidebarUpdateState.value.latestVersion} available`
  }
  return sidebarUpdateState.value.currentVersion ? `v${sidebarUpdateState.value.currentVersion} current` : sidebarUpdateState.value.statusLabel
})
const sidebarUpdateDetail = computed(() => {
  if (sidebarUpdateError.value) return sidebarUpdateError.value
  if (sidebarUpdateState.value.status === 'update_available') return 'Open System to copy or download'
  if (sidebarUpdateState.value.latestVersion) return `Latest ${sidebarUpdateState.value.latestVersion}`
  return 'System update details'
})
const sidebarUpdateTitle = computed(() => `Update Center: ${sidebarUpdateLabel.value}. ${sidebarUpdateDetail.value}`)
const sidebarUpdateStatusLightClass = computed(() => updateStatusLightClass(sidebarUpdateState.value.statusTone))
const showNav = computed(() => {
  return webUiAuthenticated.value && !isPublicRoute(route.path)
})
// A refused settings file (#782) is said on every signed-in page but Settings,
// which says it in place of its form.
const settingsBannerVisible = computed(() => showsSettingsBanner(settingsUnreadable.value, normalizeRoutePath(route.path)))

async function loadAppVersion() {
  if (appVersion.value || appVersionLoading.value) return
  appVersionLoading.value = true
  try {
    const data = await getCachedConfig()
    if (data?.version) {
      appVersion.value = data.version
    }
  } catch {
    // Auth routes can mount before the config endpoint is available.
  } finally {
    appVersionLoading.value = false
  }
}

let sidebarUpdateRefreshPending = false
async function refreshSidebarUpdateStatus() {
  if (sidebarUpdateLoading.value) {
    sidebarUpdateRefreshPending = true
    return
  }
  sidebarUpdateLoading.value = true
  sidebarUpdateError.value = ''
  try {
    const config = await getCachedConfig().catch(() => null)
    if (config?.version && !appVersion.value) {
      appVersion.value = config.version
    }
    sidebarNotifyPreReleases.value = isPrereleaseOptIn(config?.notify_pre_releases)

    const hostStatus = await fetch('./api/update-status', { credentials: 'include' }).then((r) => r.ok ? r.json() : null).catch(() => null)
    sidebarUpdateHost.value = hostStatus || { platform: config?.platform || '', distro: {} }
    if (hostStatus?.version) {
      appVersion.value = hostStatus.version
    }

    const latest = await fetch('https://api.github.com/repos/papi-ux/polaris/releases/latest').then((r) => r.ok ? r.json() : null)
    sidebarLatestRelease.value = latest
    if (sidebarNotifyPreReleases.value) {
      const releases = await fetch('https://api.github.com/repos/papi-ux/polaris/releases').then((r) => r.ok ? r.json() : [])
      sidebarPrereleaseRelease.value = Array.isArray(releases) ? releases.find((release) => release.prerelease && !release.draft) || null : null
    } else {
      sidebarPrereleaseRelease.value = null
    }
  } catch (error) {
    sidebarLatestRelease.value = null
    sidebarPrereleaseRelease.value = null
    sidebarUpdateError.value = 'Release check unavailable'
    console.error(error)
  } finally {
    sidebarUpdateLoading.value = false
    if (sidebarUpdateRefreshPending) {
      sidebarUpdateRefreshPending = false
      clearCachedConfig()
      void refreshSidebarUpdateStatus()
    }
  }
}

watch(showNav, (visible) => {
  if (visible) {
    void loadAppVersion()
    void refreshSidebarUpdateStatus()
  }
})

const signingOut = ref(false)
const signOutLabel = computed(() => (i18nReady ? t('navbar.sign_out') : 'Sign out'))
const signOutPendingLabel = computed(() => (i18nReady ? t('navbar.sign_out_pending') : 'Signing out…'))

// Ends the web session through the host. The auth guard treats the next
// probe as a confirmed sign-out, so a reload lands on the login page.
async function signOut() {
  signingOut.value = true
  try {
    const response = await fetch('./api/logout', {
      method: 'POST',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
    })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    markWebUiUnauthenticated()
    window.location.assign(`${window.location.pathname}#/login`)
    window.location.reload()
  } catch (error) {
    console.error('Sign out failed:', error)
    signingOut.value = false
    window.alert?.(i18nReady ? t('navbar.sign_out_error') : 'Polaris could not end the session. Reload and try again.')
  }
}

function toggleCollapse() {
  sidebarCollapsed.value = !sidebarCollapsed.value
  localStorage.setItem('sidebarCollapsed', sidebarCollapsed.value)
}

// The phone drawer takes focus when it opens, so keyboard users land on its first link rather than
// behind it, and hands focus back to the menu button when Escape closes it. It reopens at its top:
// it may have been left scrolled down to its footer, which would leave that first link off screen.
const menuButton = ref(null)
async function toggleSidebar() {
  sidebarOpen.value = !sidebarOpen.value
  if (!sidebarOpen.value) return
  await nextTick()
  const drawer = document.querySelector('#polaris-sidebar')
  if (!drawer) return
  drawer.scrollTop = 0
  drawer.querySelector('a[href]')?.focus({ preventScroll: true })
}

function handleKeydown(e) {
  // Command palette
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
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
      // Only a visible menu button (the phone shell) takes focus back.
      if (menuButton.value?.offsetParent) menuButton.value.focus()
      return
    }
  }

}

// Page enter animation on route change
const pageKey = ref(0)
watch(() => route.path, () => { pageKey.value++ })
watch(() => route.path, () => { sidebarOpen.value = false })

onMounted(() => {
  initTheme()
  window.addEventListener('keydown', handleKeydown)
  window.addEventListener('polaris-update-channel-changed', refreshSidebarUpdateStatus)
  if (showNav.value) {
    void loadAppVersion()
    void refreshSidebarUpdateStatus()
  }
})

onUnmounted(() => {
  window.removeEventListener('keydown', handleKeydown)
  window.removeEventListener('polaris-update-channel-changed', refreshSidebarUpdateStatus)
})
</script>

<style>
@reference "./app.css";

/* A badge (such as Preview) drops below its label when enlarged text leaves no room beside it. */
.sidebar-link {
  @apply flex flex-wrap items-center gap-x-3 gap-y-1 px-3 py-2 rounded-lg no-underline
         transition-[background-color,color,box-shadow] duration-200 relative
         hover:bg-twilight/60 hover:text-silver;
  color: var(--sidebar-fg);
}
.sidebar-link.active,
.sidebar-link.router-link-exact-active {
  color: var(--sidebar-active-fg);
  background: var(--sidebar-active-bg);
  box-shadow: inset 2px 0 0 var(--sidebar-active-bar), var(--shadow-inset-glow);
}
</style>
