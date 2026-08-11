<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from 'vue'
import { getTheme, getThemeMeta, onThemeChange, setTheme, THEMES } from './theme.js'

const props = defineProps({
  collapsed: { type: Boolean, default: false }
})

const open = ref(false)
const activeTheme = ref(getThemeMeta(getTheme()))
const rootEl = ref(null)
const optionRefs = ref([])

const activeIndex = computed(() => THEMES.findIndex((theme) => theme.id === activeTheme.value.id))

let unsubscribe = null

onMounted(() => {
  activeTheme.value = getThemeMeta(getTheme())
  unsubscribe = onThemeChange((theme) => {
    activeTheme.value = getThemeMeta(theme)
  })
  document.addEventListener('pointerdown', onDocumentPointerDown, true)
})

onBeforeUnmount(() => {
  if (unsubscribe) unsubscribe()
  document.removeEventListener('pointerdown', onDocumentPointerDown, true)
})

function onDocumentPointerDown(event) {
  if (open.value && rootEl.value && !rootEl.value.contains(event.target)) {
    open.value = false
  }
}

async function togglePicker() {
  open.value = !open.value
  if (open.value) {
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
      class="focus-ring flex w-full items-center gap-2 rounded-lg px-3 py-2 text-sm transition-[background-color,color,border-color] duration-200"
      :class="open ? 'text-ice bg-ice/10' : 'text-storm hover:text-silver hover:bg-twilight/50'"
      :title="`Theme: ${activeTheme.label}`"
    >
      <span
        class="inline-block h-3.5 w-3.5 shrink-0 rounded-full border border-ice/25"
        :style="{ background: activeTheme.preview.accent }"
        aria-hidden="true"
      ></span>
      <span v-if="!props.collapsed" class="truncate">{{ activeTheme.shortLabel }}</span>
      <svg v-if="!props.collapsed" class="ml-auto h-3 w-3 shrink-0 opacity-60" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 15l7-7 7 7"/></svg>
    </button>

    <div
      v-if="open"
      role="listbox"
      aria-label="Theme"
      class="theme-picker-panel"
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
        <span class="min-w-0 flex-1 text-left">
          <span class="block truncate text-sm font-semibold text-silver">{{ theme.label }}</span>
          <span class="block truncate text-[10px] leading-tight text-storm">{{ theme.subtitle }}</span>
        </span>
        <span v-if="theme.id === activeTheme.id" class="theme-picker-current">Current</span>
      </button>
    </div>
  </div>
</template>
