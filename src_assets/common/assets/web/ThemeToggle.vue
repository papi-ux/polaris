<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from 'vue'
import { getTheme, getThemeMeta, onThemeChange, setTheme, THEMES } from './theme.js'

const props = defineProps({
  collapsed: { type: Boolean, default: false }
})

const open = ref(false)
const activeTheme = ref(getThemeMeta(getTheme()))
const rootEl = ref(null)
const panelEl = ref(null)
const optionRefs = ref([])
// The list is rendered on <body> with fixed coordinates taken from the button, so a scrolling
// sidebar or phone drawer can neither clip nor hide it. It opens on the side with more room,
// stays inside the viewport, scrolls inside when names wrap with enlarged text, and follows the
// button when the viewport rotates or the drawer scrolls.
const panelStyle = ref(null)

function placePanel() {
  const button = rootEl.value?.querySelector('button')
  if (!button) return
  const rect = button.getBoundingClientRect()
  const width = document.documentElement.clientWidth || window.innerWidth
  const height = window.innerHeight
  // Once the button itself has left the screen there is nothing to anchor to: close instead of
  // floating the list off it, keeping focus on the picker's own button.
  if (rect.bottom <= 0 || rect.top >= height || rect.right <= 0 || rect.left >= width) {
    const hadFocus = panelEl.value?.contains(document.activeElement)
    open.value = false
    if (hadFocus) button.focus({ preventScroll: true })
    return
  }
  const gap = 8
  const clamp = (value, low, high) => Math.min(Math.max(value, low), high)
  // A partly visible button anchors at its visible edge, so the list is always on screen.
  const anchorTop = clamp(rect.top, gap, height - gap)
  const anchorBottom = clamp(rect.bottom, gap, height - gap)
  const above = anchorTop - 2 * gap
  const below = height - anchorBottom - 2 * gap
  const up = above >= below
  const left = clamp(rect.left, gap, Math.max(gap, width - gap - 264))
  panelStyle.value = {
    left: `${left}px`,
    right: 'auto',
    maxWidth: `${Math.max(width - left - gap, 0)}px`,
    maxHeight: `min(30rem, ${Math.max(Math.floor(up ? above : below), 0)}px)`,
    // Set both insets, so the stylesheet's fallback corner never stretches the list.
    top: up ? 'auto' : `${anchorBottom + gap}px`,
    bottom: up ? `${height - anchorTop + gap}px` : 'auto',
  }
}

function followViewport(event) {
  if (!open.value) return
  // A rotation or resize can move the button out of the scrolled sidebar: bring it back first.
  if (event?.type === 'resize') rootEl.value?.querySelector('button')?.scrollIntoView({ block: 'nearest', inline: 'nearest' })
  placePanel()
}

const activeIndex = computed(() => THEMES.findIndex((theme) => theme.id === activeTheme.value.id))

let unsubscribe = null

onMounted(() => {
  activeTheme.value = getThemeMeta(getTheme())
  unsubscribe = onThemeChange((theme) => {
    activeTheme.value = getThemeMeta(theme)
  })
  document.addEventListener('pointerdown', onDocumentPointerDown, true)
  window.addEventListener('resize', followViewport)
  document.addEventListener('scroll', followViewport, true)
})

onBeforeUnmount(() => {
  if (unsubscribe) unsubscribe()
  document.removeEventListener('pointerdown', onDocumentPointerDown, true)
  window.removeEventListener('resize', followViewport)
  document.removeEventListener('scroll', followViewport, true)
})

function onDocumentPointerDown(event) {
  if (open.value && rootEl.value && !rootEl.value.contains(event.target) && !panelEl.value?.contains(event.target)) {
    open.value = false
  }
}

async function togglePicker() {
  open.value = !open.value
  if (open.value) {
    placePanel()
    await nextTick()
    const target = optionRefs.value[Math.max(activeIndex.value, 0)]
    target?.focus()
  }
}

function choose(themeId) {
  setTheme(themeId)
  close(true)
}

function close(focusButton = true) {
  open.value = false
  if (focusButton) rootEl.value?.querySelector('button')?.focus()
}

function moveFocus(delta) {
  const options = optionRefs.value.filter(Boolean)
  if (!options.length) return
  const current = options.indexOf(document.activeElement)
  const next = ((current === -1 ? Math.max(activeIndex.value, 0) : current) + delta + options.length) % options.length
  options[next]?.focus()
}
</script>

<template>
  <div ref="rootEl" class="relative">
    <button
      type="button"
      @click="togglePicker"
      @keydown.escape="close()"
      aria-label="Choose theme"
      aria-haspopup="listbox"
      :aria-expanded="open"
      class="focus-ring flex w-full flex-wrap items-center gap-x-2 gap-y-0.5 rounded-lg px-3 py-2 text-sm transition-[background-color,color,border-color] duration-200"
      :class="open ? 'text-ice bg-ice/10' : 'text-storm hover:text-silver hover:bg-twilight/50'"
      :title="`Theme: ${activeTheme.label}`"
    >
      <span
        class="inline-block h-3.5 w-3.5 shrink-0 rounded-full border border-ice/25"
        :style="{ background: activeTheme.preview.accent }"
        aria-hidden="true"
      ></span>
      <span v-if="!props.collapsed" class="truncate font-medium">Appearance</span>
      <!-- A long name (Director's) takes its own line rather than an ellipsis. -->
      <span v-if="!props.collapsed" class="ml-auto max-w-full truncate text-[11px] text-storm">{{ activeTheme.shortLabel }}</span>
      <svg v-if="!props.collapsed" class="ml-1 h-3 w-3 shrink-0 opacity-60" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 15l7-7 7 7"/></svg>
    </button>

    <Teleport to="body">
    <div
      v-if="open"
      ref="panelEl"
      role="listbox"
      aria-label="Theme"
      class="theme-picker-panel"
      :style="panelStyle"
      @keydown.escape.stop.prevent="close()"
      @keydown.down.stop.prevent="moveFocus(1)"
      @keydown.up.stop.prevent="moveFocus(-1)"
    >
      <button
        v-for="(theme, index) in THEMES"
        :key="theme.id"
        :ref="(el) => { optionRefs[index] = el }"
        type="button"
        role="option"
        :aria-selected="theme.id === activeTheme.id"
        class="theme-picker-option focus-ring"
        :class="{ 'is-current': theme.id === activeTheme.id }"
        @click="choose(theme.id)"
      >
        <span class="theme-picker-swatch" :style="{ background: theme.preview.window }" aria-hidden="true">
          <span class="theme-picker-swatch-card" :style="{ background: theme.preview.card }"></span>
          <span class="theme-picker-swatch-dot" :style="{ background: theme.preview.accent }"></span>
          <span class="theme-picker-swatch-bar" :style="{ background: theme.preview.accent }"></span>
        </span>
        <span class="theme-picker-text">
          <span class="theme-picker-heading">
            <span class="theme-picker-label">{{ theme.label }}</span>
            <span v-if="theme.id === activeTheme.id" class="theme-picker-current">Current</span>
          </span>
          <span class="block truncate text-[10px] leading-tight text-storm">{{ theme.subtitle }}</span>
        </span>
      </button>
    </div>
    </Teleport>
  </div>
</template>
