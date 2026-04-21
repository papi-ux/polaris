<template>
  <span
    ref="root"
    class="info-hint"
    @mouseenter="open = true"
    @mouseleave="open = false"
  >
    <button
      ref="trigger"
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

    <Teleport to="body">
      <transition name="info-hint-fade">
        <span
          v-if="open"
          ref="panel"
          :id="tooltipId"
          role="tooltip"
          class="info-hint-panel"
          :class="alignmentClass"
          :style="panelStyle"
          :data-align="props.align"
          :data-placement="placement"
        >
          <slot />
        </span>
      </transition>
    </Teleport>
  </span>
</template>

<script setup>
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'

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
const trigger = ref(null)
const panel = ref(null)
const open = ref(false)
const panelStyle = ref({})
const placement = ref('bottom')

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

function updatePanelPosition() {
  if (!open.value || !trigger.value || !panel.value) {
    return
  }

  if (window.matchMedia('(max-width: 47.99rem)').matches) {
    panelStyle.value = {}
    placement.value = 'bottom'
    return
  }

  const triggerRect = trigger.value.getBoundingClientRect()
  const panelRect = panel.value.getBoundingClientRect()
  const viewportWidth = window.innerWidth
  const viewportHeight = window.innerHeight
  const gutter = 12
  const gap = 10

  let left = triggerRect.left
  if (props.align === 'right') {
    left = triggerRect.right - panelRect.width
  } else if (props.align === 'center') {
    left = triggerRect.left + (triggerRect.width / 2) - (panelRect.width / 2)
  }
  left = Math.min(Math.max(left, gutter), viewportWidth - panelRect.width - gutter)

  let top = triggerRect.bottom + gap
  let nextPlacement = 'bottom'
  if (top + panelRect.height > viewportHeight - gutter) {
    const aboveTop = triggerRect.top - panelRect.height - gap
    if (aboveTop >= gutter) {
      top = aboveTop
      nextPlacement = 'top'
    } else {
      top = Math.max(gutter, viewportHeight - panelRect.height - gutter)
    }
  }

  panelStyle.value = {
    left: `${Math.round(left)}px`,
    top: `${Math.round(top)}px`,
  }
  placement.value = nextPlacement
}

onMounted(() => {
  document.addEventListener('pointerdown', handlePointerDown)
  window.addEventListener('resize', updatePanelPosition)
  window.addEventListener('scroll', updatePanelPosition, true)
})

onUnmounted(() => {
  document.removeEventListener('pointerdown', handlePointerDown)
  window.removeEventListener('resize', updatePanelPosition)
  window.removeEventListener('scroll', updatePanelPosition, true)
})

watch(open, async (isOpen) => {
  if (!isOpen) {
    return
  }

  await nextTick()
  updatePanelPosition()
})
</script>
