<script setup>
import { nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { onThemeTokensChange, readThemeTokens, withAlpha } from '../theme-bridge.js'

// The stage's lower edge: one reading per cell, from Mission Control's helpers. The live cells
// carry sparklines of the dashboard's rolling history. uPlot loads only when a sparkline is shown,
// and no sparkline is drawn under reduced motion.
const props = defineProps({
  cells: { type: Array, default: () => [] },
  charts: { type: Boolean, default: false },
  prefersReducedMotion: { type: Boolean, default: false },
  history: { type: Object, default: () => ({ timestamps: [], series: {} }) },
  historyRevision: { type: Number, default: 0 },
  // Live Tuning's recorded changes, per sparkline, on the history's time axis, and one quiet line
  // telling the newest. Both come from live-tuning-timeline.js; with none, nothing is drawn or said.
  markers: { type: Object, default: () => ({}) },
  note: { type: String, default: '' },
})

const SPARK_HEIGHT = 32
const root = ref(null)
const sparkEls = {}
const charts = {}
let uPlotLib = null
let drawTuningMarkers = null
let resizeObserver = null
let unsubscribeTheme = null
let unmounted = false

async function loadUPlot() {
  if (!uPlotLib) {
    const mod = await import('uplot')
    const timeline = await import('../live-tuning-timeline.js')
    await import('uplot/dist/uPlot.min.css')
    drawTuningMarkers = timeline.drawTuningMarkers
    uPlotLib = mod.default
  }
  return uPlotLib
}

function chartOptions(key, width) {
  const tokens = readThemeTokens(['silver', 'ice'])
  const color = tokens.silver
  return {
    width,
    height: SPARK_HEIGHT,
    hooks: { draw: [u => drawTuningMarkers(u, props.markers?.[key], tokens.ice || color, uPlotLib?.pxRatio || 1)] },
    cursor: { show: false },
    legend: { show: false },
    axes: [{ show: false }, { show: false }],
    scales: {
      y: {
        range: (u, min, max) => {
          const span = max - min
          const pad = span > 0 ? span * 0.2 : (Math.abs(max) * 0.15 || 1)
          return [min - pad, max + pad]
        },
      },
    },
    series: [
      {},
      {
        stroke: withAlpha(color, 0.6),
        width: 1.5,
        ...(uPlotLib?.paths?.spline ? { paths: uPlotLib.paths.spline() } : {}),
        points: {
          show: true,
          filter: (u, seriesIdx) => {
            const len = u.data[seriesIdx]?.length || 0
            return len ? [len - 1] : []
          },
          size: 5,
          width: 0,
          fill: color,
        },
      },
    ],
  }
}

function destroyChart(key) {
  charts[key]?.destroy()
  delete charts[key]
}

function destroyAll() {
  for (const key of Object.keys(charts)) destroyChart(key)
}

function pushData() {
  const ts = [...(props.history.timestamps || [])]
  for (const [key, chart] of Object.entries(charts)) chart.setData([ts, [...(props.history.series?.[key] || [])]])
}

// A slot that is hidden or not laid out yet has no width; its chart waits for the first real size
// instead of drawing at a guessed one, and every later size change (rotation, wrapping, enlarged
// text) resizes it.
async function syncCharts() {
  if (!props.charts || props.prefersReducedMotion) { destroyAll(); return }
  if (!Object.keys(sparkEls).length) return
  const lib = await loadUPlot().catch(() => null)
  if (!lib || unmounted || !props.charts || props.prefersReducedMotion) return
  for (const [key, el] of Object.entries(sparkEls)) {
    const width = Math.floor(el.clientWidth)
    if (!el.isConnected || width <= 0) continue
    if (!charts[key]) charts[key] = new lib(chartOptions(key, width), [[], []], el)
    else if (charts[key].width !== width) charts[key].setSize({ width, height: SPARK_HEIGHT })
  }
  pushData()
}

// Stable ref callbacks: a new function per render would unmount and remount every chart each tick.
const sparkRefs = Object.fromEntries(['quality', 'latency', 'fps', 'loss', 'bitrate', 'encode'].map(key => [key, (el) => {
  if (el) sparkEls[key] = el
  else { destroyChart(key); delete sparkEls[key] }
}]))

watch(() => props.historyRevision, () => nextTick(syncCharts))
watch(() => [props.charts, props.prefersReducedMotion], () => nextTick(syncCharts))

onMounted(() => {
  if (typeof ResizeObserver === 'function' && root.value) {
    resizeObserver = new ResizeObserver(() => { syncCharts() })
    resizeObserver.observe(root.value)
  }
  // Canvas cannot follow CSS variables, so a theme swap redraws the lines in the new tokens.
  unsubscribeTheme = onThemeTokensChange(() => { destroyAll(); syncCharts() })
  nextTick(syncCharts)
})

onUnmounted(() => {
  unmounted = true
  resizeObserver?.disconnect()
  resizeObserver = null
  unsubscribeTheme?.()
  destroyAll()
})
</script>

<template>
  <div class="mc-signals-wrap" role="group">
    <dl ref="root" class="mc-signals">
      <div v-for="cell in cells" :key="cell.key" class="mc-signal" :class="{ 'is-missing': cell.missing, 'is-recap': cell.recap }" v-bind="cell.attrs" :title="cell.title || undefined">
        <!-- A linked cell opens from its label, and the link's hit area covers the whole cell. -->
        <dt class="mc-signal-label">
          <router-link v-if="cell.link" :to="cell.link.to" class="focus-ring mc-signal-link" :aria-label="cell.link.label" :aria-description="cell.link.description || undefined">{{ cell.label }}<span aria-hidden="true"> ›</span></router-link>
          <template v-else>{{ cell.label }}</template>
        </dt>
        <dd class="mc-signal-value" :class="[cell.missing ? 'text-storm' : cell.tone, { 'is-word': cell.word }]">
          <span v-if="cell.grade" class="dashboard-grade-badge mc-signal-grade" :class="cell.tone">{{ cell.grade }}</span>
          <span class="mc-signal-number">{{ cell.value }}</span><span v-if="cell.unit" class="mc-signal-unit">{{ cell.unit }}</span>
          <span v-if="cell.tag" class="mc-signal-tag">{{ cell.tag }}</span>
        </dd>
        <dd v-if="cell.sub" class="mc-signal-sub">{{ cell.sub }}</dd>
        <dd v-if="charts && cell.spark && !prefersReducedMotion" class="mc-signal-spark" aria-hidden="true">
          <div v-if="!prefersReducedMotion" class="dashboard-strip-spark" :ref="sparkRefs[cell.spark]"></div>
        </dd>
      </div>
    </dl>
    <p v-if="note" class="mc-signals-note" data-live-tuning-note>{{ note }}</p>
  </div>
</template>
