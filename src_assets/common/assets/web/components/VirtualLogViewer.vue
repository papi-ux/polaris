<template>
  <div class="relative overflow-hidden rounded-xl border border-storm/20 bg-deep/80" :style="{ height: containerHeight + 'px' }">
    <div class="absolute inset-x-0 top-0 z-10 flex flex-wrap items-center justify-between gap-2 border-b border-storm/15 bg-void/75 px-3 py-2 backdrop-blur-sm">
      <div class="flex min-w-0 flex-wrap items-center gap-2 text-[11px] uppercase tracking-[0.18em] text-storm">
        <span class="truncate">{{ lineCountLabel }}</span>
        <span v-if="severityCounts.fatal" class="rounded-full border border-red-500/20 bg-red-500/10 px-2 py-0.5 text-red-200">Fatal {{ severityCounts.fatal }}</span>
        <span v-if="severityCounts.warning" class="rounded-full border border-amber-300/20 bg-amber-300/10 px-2 py-0.5 text-amber-200">Warn {{ severityCounts.warning }}</span>
        <span v-if="severityCounts.error" class="rounded-full border border-red-400/15 bg-red-400/5 px-2 py-0.5 text-red-100">Err {{ severityCounts.error }}</span>
        <span v-if="paused" class="rounded-full border border-amber-300/25 bg-amber-300/10 px-2 py-0.5 text-amber-200">Paused</span>
        <span v-else-if="followTail" class="rounded-full border border-green-500/25 bg-green-500/10 px-2 py-0.5 text-green-300">Following</span>
      </div>
      <div class="flex items-center gap-1.5">
        <button class="rounded-md border border-storm/20 bg-void/60 px-2 py-1 text-[11px] text-storm transition-colors hover:border-storm/40 hover:text-silver" @click="followTail = !followTail">
          {{ followTail ? 'Unfollow' : 'Follow' }}
        </button>
        <button class="rounded-md border border-storm/20 bg-void/60 px-2 py-1 text-[11px] text-storm transition-colors hover:border-storm/40 hover:text-silver" @click="togglePause">
          {{ paused ? 'Resume' : 'Pause' }}
        </button>
        <button class="rounded-md border border-storm/20 bg-void/60 px-2 py-1 text-[11px] text-storm transition-colors hover:border-storm/40 hover:text-silver" @click="scrollToBottom">
          Bottom
        </button>
        <button class="inline-flex h-7 w-7 items-center justify-center rounded-md border border-storm/20 bg-void/60 text-storm transition-colors hover:border-storm/40 hover:text-ice" aria-label="Copy visible log lines" @click="$emit('copy')">
          <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 16H6a2 2 0 01-2-2V6a2 2 0 012-2h8a2 2 0 012 2v2m-6 12h8a2 2 0 002-2v-8a2 2 0 00-2-2h-8a2 2 0 00-2 2v8a2 2 0 002 2z"/></svg>
        </button>
      </div>
    </div>
    <div
      ref="scrollContainer"
      role="log"
      aria-label="Polaris runtime logs"
      class="overflow-auto font-mono text-xs text-silver scrollbar-hidden"
      :style="{ height: containerHeight + 'px', paddingTop: '3rem' }"
      @scroll="onScroll"
    >
      <div :style="{ height: totalHeight + 'px', position: 'relative' }">
        <div
          v-for="item in visibleLines"
          :key="item.index"
          class="absolute left-0 right-0 flex gap-2 px-3 py-0.5 whitespace-pre-wrap break-all"
          :style="{ top: item.top + 'px' }"
        >
          <span class="w-10 shrink-0 text-right text-[10px] text-storm/55">{{ item.index + 1 }}</span>
          <span class="mt-1 h-1.5 w-1.5 shrink-0 rounded-full" :class="severityDotClass(item.severity)"></span>
          <span
            class="min-w-0 flex-1"
            :class="severityTextClass(item.severity)"
          >{{ item.line }}</span>
        </div>
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
const renderedLogs = ref(props.logs)
const followTail = ref(true)
const paused = ref(false)
const atBottom = ref(true)
const lineFont = '12px monospace'
const lineHeight = 18
const bufferLines = 10

// Split logs into lines and pre-measure heights with pretext
const lines = computed(() => renderedLogs.value.split('\n'))
const lineCountLabel = computed(() => `${Math.max(lines.value.filter(Boolean).length, 0)} visible lines`)
const severityCounts = computed(() => {
  const counts = { fatal: 0, warning: 0, error: 0 }
  lines.value.forEach((line) => {
    const severity = severityForLine(line)
    if (severity === 'fatal') counts.fatal += 1
    else if (severity === 'warning') counts.warning += 1
    else if (severity === 'error') counts.error += 1
  })
  return counts
})

const lineHeights = computed(() => {
  const containerWidth = Math.max((scrollContainer.value?.clientWidth || 600) - 64, 260)
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
      top: offsets[i],
      severity: severityForLine(lines.value[i])
    })
  }
  return result
})

function severityForLine(line) {
  if (line.includes('Fatal:')) return 'fatal'
  if (line.includes('Warning:')) return 'warning'
  if (line.includes('Error:')) return 'error'
  return 'info'
}

function severityDotClass(severity) {
  if (severity === 'fatal') return 'bg-red-400'
  if (severity === 'warning') return 'bg-amber-300'
  if (severity === 'error') return 'bg-rose-300'
  return 'bg-storm/35'
}

function severityTextClass(severity) {
  if (severity === 'fatal') return 'text-red-300'
  if (severity === 'warning') return 'text-amber-200'
  if (severity === 'error') return 'text-red-100'
  return ''
}

function onScroll() {
  const el = scrollContainer.value
  scrollTop.value = el?.scrollTop || 0
  if (el) {
    atBottom.value = el.scrollTop + el.clientHeight >= el.scrollHeight - 40
  }
}

function scrollToBottom(enableFollow = true) {
  if (scrollContainer.value) {
    scrollContainer.value.scrollTop = scrollContainer.value.scrollHeight
    atBottom.value = true
    if (enableFollow) {
      followTail.value = true
    }
  }
}

function togglePause() {
  paused.value = !paused.value
  if (!paused.value) {
    renderedLogs.value = props.logs
    nextTick(() => {
      if (followTail.value) {
        scrollToBottom()
      }
    })
  }
}

watch(() => props.logs, async (nextLogs) => {
  if (paused.value) {
    return
  }

  renderedLogs.value = nextLogs
  await nextTick()

  if (followTail.value) {
    scrollToBottom(false)
  }
})

onMounted(() => {
  scrollToBottom(false)
})
</script>
