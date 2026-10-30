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
import { computed, inject } from 'vue'
import { useTailscalePeer } from '../composables/useTailscalePeer.js'

const props = defineProps({ stats: { type: Object, default: null } })
const i18n = inject('i18n')
const evidence = useTailscalePeer(computed(() => props.stats))
const label = computed(() => i18n.t(`troubleshooting.tailscale_peer_${evidence.value.confirmed ? evidence.value.transport : 'unverified'}`))
</script>
