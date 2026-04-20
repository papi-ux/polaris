<template>
  <span
    ref="root"
    class="info-hint"
    @mouseenter="open = true"
    @mouseleave="open = false"
  >
    <button
      type="button"
      class="info-hint-trigger focus-ring"
      :class="sizeClass"
      :aria-describedby="tooltipId"
      :aria-expanded="open ? 'true' : 'false'"
      :aria-label="label"
      @focus="open = true"
      @blur="open = false"
      @click.stop.prevent="open = !open"
      @keydown.esc.stop.prevent="open = false"
    >
      <svg class="h-3.5 w-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.8" d="M12 8h.01M11 12h1v4h1m8-4a9 9 0 11-18 0 9 9 0 0118 0Z" />
      </svg>
    </button>

    <transition name="info-hint-fade">
      <span
        v-if="open"
        :id="tooltipId"
        role="tooltip"
        class="info-hint-panel"
        :class="alignmentClass"
        :data-align="props.align"
      >
        <slot />
      </span>
    </transition>
  </span>
</template>

<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'

const props = defineProps({
  align: {
    type: String,
    default: 'left',
  },
  label: {
    type: String,
    default: 'More information',
  },
  size: {
    type: String,
    default: 'md',
  },
})

const root = ref(null)
const open = ref(false)

const tooltipId = `info-hint-${Math.random().toString(36).slice(2, 10)}`

const alignmentClass = computed(() => {
  if (props.align === 'right') return 'right-0'
  if (props.align === 'center') return 'left-1/2 -translate-x-1/2'
  return 'left-0'
})

const sizeClass = computed(() => props.size === 'sm' ? 'info-hint-trigger--sm' : '')

function handlePointerDown(event) {
  if (!root.value?.contains(event.target)) {
    open.value = false
  }
}

onMounted(() => {
  document.addEventListener('pointerdown', handlePointerDown)
})

onUnmounted(() => {
  document.removeEventListener('pointerdown', handlePointerDown)
})
</script>
