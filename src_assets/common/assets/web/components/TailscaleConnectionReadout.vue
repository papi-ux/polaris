<template>
  <section class="section-card" data-tailscale-peer-readout>
    <div class="section-kicker">{{ $t('troubleshooting.tailscale_peer_kicker') }}</div>
    <h2 class="section-title">{{ $t('troubleshooting.tailscale_peer_title') }}</h2>
    <p class="mt-3 text-sm font-medium text-silver" role="status" data-tailscale-peer-status>{{ label }}</p>
    <p class="section-copy">{{ $t(evidence.reason === 'no_active_stream' ? 'troubleshooting.tailscale_peer_idle' : 'troubleshooting.tailscale_peer_scope') }}</p>
    <p class="mt-2 text-xs text-storm">{{ $t('troubleshooting.tailscale_peer_quality') }}</p>
    <a href="https://tailscale.com/docs/reference/connection-types" target="_blank" rel="noopener" class="focus-ring mt-3 inline-block text-sm text-ice hover:underline">{{ $t('troubleshooting.tailscale_peer_help') }}</a>
  </section>
</template>

<script setup>
import { computed, inject, onBeforeUnmount, ref, watch } from 'vue'
import { normalizeTailscalePeer } from '../tailscale-connection-readout.js'

const props = defineProps({ stats: { type: Object, default: null } })
const i18n = inject('i18n')
const raw = ref(null)
const now = ref(Date.now())
let receivedAt = 0
let activeRequest = null
let disposed = false
const evidence = computed(() => normalizeTailscalePeer(
  performance.now() - receivedAt <= 10000 ? raw.value : null,
  { now: now.value, generation: props.stats?.stream_instance_id, streaming: props.stats?.streaming === true },
))
const label = computed(() => i18n.t(`troubleshooting.tailscale_peer_${evidence.value.confirmed ? evidence.value.transport : 'unverified'}`))

async function refresh() {
  if (disposed || activeRequest) return
  if (!props.stats?.streaming) { raw.value = null; return }
  const controller = new AbortController()
  activeRequest = controller
  const timeout = setTimeout(() => controller.abort(), 3000)
  try {
    const response = await fetch('./api/network/connection', { credentials: 'include', signal: controller.signal, cache: 'no-store' })
    const value = response.ok ? await response.json() : null
    if (!disposed) { raw.value = value; receivedAt = performance.now(); now.value = Date.now() }
  } catch {
    if (!disposed) raw.value = null
  } finally {
    clearTimeout(timeout)
    activeRequest = null
  }
}
watch(() => [props.stats?.streaming, props.stats?.stream_instance_id], () => { raw.value = null; refresh() }, { immediate: true })
const refreshTimer = setInterval(refresh, 5000)
const expiryTimer = setInterval(() => { now.value = Date.now() }, 1000)
onBeforeUnmount(() => {
  disposed = true
  activeRequest?.abort()
  clearInterval(refreshTimer)
  clearInterval(expiryTimer)
})
</script>
