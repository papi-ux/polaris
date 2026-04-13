<script setup>
import { ref, onMounted } from 'vue'
import { getTheme, toggleTheme } from './theme.js'

const props = defineProps({
  collapsed: { type: Boolean, default: false }
})

const isOled = ref(false)

onMounted(() => {
  isOled.value = getTheme() === 'oled'
})

function toggle() {
  const next = toggleTheme()
  isOled.value = next === 'oled'
}
</script>

<template>
  <button
    @click="toggle"
    class="flex items-center gap-2 w-full px-3 py-2 rounded-lg text-sm transition-colors"
    :class="isOled ? 'text-ice bg-ice/10' : 'text-storm hover:text-silver hover:bg-twilight/50'"
    :title="isOled ? 'Switch to Space Whale theme' : 'Switch to OLED Dark Galaxy'"
  >
    <svg v-if="isOled" class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M20.354 15.354A9 9 0 018.646 3.646 9.003 9.003 0 0012 21a9.003 9.003 0 008.354-5.646z"/></svg>
    <svg v-else class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 3v1m0 16v1m9-9h-1M4 12H3m15.364 6.364l-.707-.707M6.343 6.343l-.707-.707m12.728 0l-.707.707M6.343 17.657l-.707.707M16 12a4 4 0 11-8 0 4 4 0 018 0z"/></svg>
    <span v-if="!props.collapsed">{{ isOled ? 'OLED Galaxy' : 'Space Whale' }}</span>
  </button>
</template>
