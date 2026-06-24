<script setup>
import { ref, onMounted } from 'vue'
import { getNextTheme, getTheme, getThemeMeta, toggleTheme } from './theme.js'

const props = defineProps({
  collapsed: { type: Boolean, default: false }
})

const activeTheme = ref(getThemeMeta(getTheme()))
const nextTheme = ref(getThemeMeta(getNextTheme(activeTheme.value.id)))

function syncTheme(theme = getTheme()) {
  activeTheme.value = getThemeMeta(theme)
  nextTheme.value = getThemeMeta(getNextTheme(activeTheme.value.id))
}

onMounted(() => {
  syncTheme()
})

function toggle() {
  const next = toggleTheme()
  syncTheme(next)
}
</script>

<template>
  <button
    @click="toggle"
    class="flex items-center gap-2 w-full px-3 py-2 rounded-lg text-sm transition-colors"
    :class="activeTheme.id !== 'polaris' ? 'text-ice bg-ice/10' : 'text-storm hover:text-silver hover:bg-twilight/50'"
    :title="`Switch to ${nextTheme.label} theme`"
  >
    <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 3v1m0 16v1m9-9h-1M4 12H3m15.364 6.364l-.707-.707M6.343 6.343l-.707-.707m12.728 0l-.707.707M6.343 17.657l-.707.707M16 12a4 4 0 11-8 0 4 4 0 018 0z"/></svg>
    <span v-if="!props.collapsed">{{ activeTheme.shortLabel }}</span>
  </button>
</template>
