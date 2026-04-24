<template>
  <div class="dashboard-telemetry-grid mt-4">
    <div class="card p-3">
      <div class="text-[10px] font-semibold uppercase tracking-wider text-green-400/80">FPS</div>
      <div ref="fpsChartEl" class="h-24 w-full"></div>
    </div>
    <div class="card p-3">
      <div class="text-[10px] font-semibold uppercase tracking-wider text-sky-400/80">Bitrate</div>
      <div ref="bitrateChartEl" class="h-24 w-full"></div>
    </div>
    <div class="card p-3">
      <div class="text-[10px] font-semibold uppercase tracking-wider text-amber-400/80">Latency</div>
      <div ref="latencyChartEl" class="h-24 w-full"></div>
    </div>
    <div class="card p-3">
      <div class="text-[10px] font-semibold uppercase tracking-wider text-red-400/80">Packet Loss</div>
      <div ref="lossChartEl" class="h-24 w-full"></div>
    </div>
  </div>
</template>

<script setup>
import { ref, watch, nextTick, onMounted, onUnmounted } from 'vue'

const props = defineProps({
  stats: { type: Object, default: null },
})

let uPlotLib = null

async function loadUPlot() {
  if (!uPlotLib) {
    const mod = await import('uplot')
    await import('uplot/dist/uPlot.min.css')
    uPlotLib = mod.default
  }
  return uPlotLib
}

const fpsChartEl = ref(null)
const bitrateChartEl = ref(null)
const latencyChartEl = ref(null)
const lossChartEl = ref(null)

let fpsChart = null
let bitrateChart = null
let latencyChart = null
let lossChart = null
let resizeObserver = null

const maxPoints = 60
const timestamps = ref([])
const fpsHistory = ref([])
const bitrateHistory = ref([])
const latencyHistory = ref([])
const lossHistory = ref([])

function makeChartOpts(color = '#c8d6e5') {
  const r = parseInt(color.slice(1,3), 16)
  const g = parseInt(color.slice(3,5), 16)
  const b = parseInt(color.slice(5,7), 16)
  return {
    width: 300,
    height: 96,
    cursor: { show: false },
    legend: { show: false },
    axes: [
      {
        stroke: '#4c5265',
        grid: { stroke: '#4c526520', width: 1 },
        ticks: { show: false },
        font: '9px sans-serif',
        values: () => [],
      },
      {
        stroke: '#687b81',
        grid: { stroke: '#4c526520', width: 1 },
        ticks: { stroke: '#4c5265', width: 1 },
        font: '9px sans-serif',
        size: 35,
      },
    ],
    series: [
      {},
      {
        stroke: color,
        width: 1.5,
        fill: `rgba(${r}, ${g}, ${b}, 0.06)`,
      },
    ],
  }
}

function initChart(el, opts) {
  if (!el || !uPlotLib) return null
  opts.width = el.clientWidth || 300
  return new uPlotLib(opts, [[], []], el)
}

function updateChartData(chart, ts, values) {
  if (chart) chart.setData([ts, values])
}

function resizeCharts() {
  const charts = [
    { chart: fpsChart, el: fpsChartEl.value },
    { chart: bitrateChart, el: bitrateChartEl.value },
    { chart: latencyChart, el: latencyChartEl.value },
    { chart: lossChart, el: lossChartEl.value },
  ]
  for (const { chart, el } of charts) {
    if (chart && el) {
      chart.setSize({ width: el.clientWidth, height: 96 })
    }
  }
}

async function setupCharts() {
  await loadUPlot()
  await nextTick()
  if (fpsChartEl.value && !fpsChart) fpsChart = initChart(fpsChartEl.value, makeChartOpts('#4ade80'))
  if (bitrateChartEl.value && !bitrateChart) bitrateChart = initChart(bitrateChartEl.value, makeChartOpts('#38bdf8'))
  if (latencyChartEl.value && !latencyChart) latencyChart = initChart(latencyChartEl.value, makeChartOpts('#fbbf24'))
  if (lossChartEl.value && !lossChart) lossChart = initChart(lossChartEl.value, makeChartOpts('#f87171'))
}

function destroyCharts() {
  if (fpsChart) { fpsChart.destroy(); fpsChart = null }
  if (bitrateChart) { bitrateChart.destroy(); bitrateChart = null }
  if (latencyChart) { latencyChart.destroy(); latencyChart = null }
  if (lossChart) { lossChart.destroy(); lossChart = null }
  timestamps.value = []
  fpsHistory.value = []
  bitrateHistory.value = []
  latencyHistory.value = []
  lossHistory.value = []
}

watch(
  () => props.stats,
  async (stats) => {
    if (!stats?.streaming) {
      destroyCharts()
      return
    }

    timestamps.value.push(Date.now() / 1000)
    fpsHistory.value.push(Number(stats.fps || 0))
    bitrateHistory.value.push(Number(stats.bitrate_kbps || 0) / 1000)
    latencyHistory.value.push(Number(stats.latency_ms || 0))
    lossHistory.value.push(Number(stats.packet_loss || 0))

    while (timestamps.value.length > maxPoints) {
      timestamps.value.shift()
      fpsHistory.value.shift()
      bitrateHistory.value.shift()
      latencyHistory.value.shift()
      lossHistory.value.shift()
    }

    if (!fpsChart || !bitrateChart || !latencyChart || !lossChart) {
      await setupCharts()
    }

    await nextTick()
    const ts = [...timestamps.value]
    updateChartData(fpsChart, ts, [...fpsHistory.value])
    updateChartData(bitrateChart, ts, [...bitrateHistory.value])
    updateChartData(latencyChart, ts, [...latencyHistory.value])
    updateChartData(lossChart, ts, [...lossHistory.value])
  },
  { immediate: true },
)

onMounted(() => {
  resizeObserver = new ResizeObserver(() => {
    resizeCharts()
  })
  const parent = fpsChartEl.value?.parentElement?.parentElement
  if (parent) {
    resizeObserver.observe(parent)
  }
})

onUnmounted(() => {
  destroyCharts()
  if (resizeObserver) {
    resizeObserver.disconnect()
    resizeObserver = null
  }
})
</script>
