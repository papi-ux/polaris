<template>
  <div class="relative" :style="{ height: containerHeight + 'px' }">
    <button class="copy-icon absolute top-2 right-2 z-10 text-storm hover:text-ice" @click="$emit('copy')">
      <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 16H6a2 2 0 01-2-2V6a2 2 0 012-2h8a2 2 0 012 2v2m-6 12h8a2 2 0 002-2v-8a2 2 0 00-2-2h-8a2 2 0 00-2 2v8a2 2 0 002 2z"/></svg>
    </button>
    <div
      ref="scrollContainer"
      class="overflow-auto font-mono text-xs text-silver bg-deep rounded-lg p-2 scrollbar-hidden"
      :style="{ height: containerHeight + 'px' }"
      @scroll="onScroll"
    >
      <div :style="{ height: totalHeight + 'px', position: 'relative' }">
        <div
          v-for="item in visibleLines"
          :key="item.index"
          class="absolute left-0 right-0 px-2 whitespace-pre-wrap break-all"
          :style="{ top: item.top + 'px' }"
          :class="item.line.includes('Error') || item.line.includes('Fatal') ? 'text-red-400' : item.line.includes('Warning') ? 'text-yellow-400' : ''"
        >{{ item.line }}</div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, watch, onMounted, nextTick } from 'vue'
import { prepare, layout } from '@chenglou/pretext'

const props = defineProps({
  logs: { type: String, default: '' },
  containerHeight: { type: Number, default: 400 }
})

defineEmits(['copy'])

const scrollContainer = ref(null)
const scrollTop = ref(0)
const lineFont = '12px monospace'
const lineHeight = 18
const bufferLines = 10

// Split logs into lines and pre-measure heights with pretext
const lines = computed(() => props.logs.split('\n'))

const lineHeights = computed(() => {
  const containerWidth = scrollContainer.value?.clientWidth || 600
  return lines.value.map(line => {
    if (!line || line.length < containerWidth / 7) {
      // Short lines are single-height, skip pretext for performance
      return lineHeight
    }
    try {
      const prepared = prepare(line, lineFont)
      const result = layout(prepared, containerWidth - 20, lineHeight)
      return Math.max(lineHeight, result.height)
    } catch {
      return lineHeight
    }
  })
})

const lineOffsets = computed(() => {
  const offsets = [0]
  for (let i = 0; i < lineHeights.value.length; i++) {
    offsets.push(offsets[i] + lineHeights.value[i])
  }
  return offsets
})

const totalHeight = computed(() => {
  const offsets = lineOffsets.value
  return offsets[offsets.length - 1] || 0
})

const visibleLines = computed(() => {
  const offsets = lineOffsets.value
  const top = scrollTop.value
  const bottom = top + props.containerHeight
  const result = []

  // Binary search for first visible line
  let lo = 0, hi = lines.value.length - 1
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (offsets[mid + 1] < top - bufferLines * lineHeight) lo = mid + 1
    else hi = mid
  }

  for (let i = Math.max(0, lo - bufferLines); i < lines.value.length; i++) {
    if (offsets[i] > bottom + bufferLines * lineHeight) break
    result.push({
      index: i,
      line: lines.value[i],
      top: offsets[i]
    })
  }
  return result
})

function onScroll() {
  scrollTop.value = scrollContainer.value?.scrollTop || 0
}

// Auto-scroll to bottom when new logs arrive
watch(() => props.logs, async () => {
  await nextTick()
  if (scrollContainer.value) {
    const el = scrollContainer.value
    const wasAtBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 50
    if (wasAtBottom) {
      el.scrollTop = el.scrollHeight
    }
  }
})

onMounted(() => {
  if (scrollContainer.value) {
    scrollContainer.value.scrollTop = scrollContainer.value.scrollHeight
  }
})
</script>
