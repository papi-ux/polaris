<template>
  <div class="surface-subtle p-4" data-trusted-network>
    <div class="text-sm font-medium text-silver">{{ $t('welcome.network_trusted_title') }}</div>
    <p class="mt-1 text-sm leading-relaxed text-storm">{{ $t('welcome.network_trusted_intro') }}</p>
    <div class="mt-3 rounded-2xl border border-warning/25 bg-warning/10 px-4 py-3 text-sm text-warning-bright">
      {{ $t('welcome.network_trusted_warning') }}
    </div>

    <div class="mt-3 space-y-2">
      <div v-if="loading" class="text-sm text-storm">{{ $t('welcome.network_detecting') }}</div>
      <div v-else-if="loadFailed" class="text-sm text-warning-bright">{{ $t('welcome.network_detect_failed') }}</div>
      <div v-else-if="!networks.length" class="text-sm text-storm">{{ $t('welcome.network_none_detected') }}</div>
      <div
        v-for="network in networks"
        :key="network.cidr"
        class="flex flex-wrap items-center justify-between gap-3 rounded-xl border border-storm/20 bg-void/40 px-3 py-2"
        :data-network="network.cidr"
      >
        <div class="min-w-0 break-words text-sm text-silver">
          {{ $t('welcome.network_on_interface', { cidr: network.cidr, interfaces: interfacesOf(network) }) }}
        </div>
        <button
          type="button"
          class="inline-flex h-9 shrink-0 items-center justify-center rounded-xl border border-ice/40 bg-ice/10 px-3 text-xs font-semibold text-ice transition-colors hover:bg-ice/20 disabled:cursor-not-allowed disabled:opacity-60"
          :disabled="busy || isTrusted(network.cidr)"
          @click="trust(network.cidr)"
        >
          {{ isTrusted(network.cidr) ? $t('welcome.network_already_trusted') : $t('welcome.network_trust_this') }}
        </button>
      </div>
    </div>

    <div class="mt-4">
      <label for="welcomeTrustedCidr" class="mb-1 block text-sm font-medium text-storm">{{ $t('welcome.network_manual_label') }}</label>
      <div class="flex flex-wrap items-center gap-3">
        <input
          id="welcomeTrustedCidr"
          v-model="manualCidr"
          type="text"
          autocomplete="off"
          spellcheck="false"
          class="settings-input min-w-0 flex-1 font-mono text-sm"
          :placeholder="$t('welcome.network_manual_placeholder')"
          @keydown.enter.prevent="trustManual"
        />
        <button
          type="button"
          class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 disabled:cursor-not-allowed disabled:opacity-60"
          :disabled="busy || !manualCidr.trim()"
          @click="trustManual"
        >
          {{ busy ? $t('welcome.network_saving') : $t('welcome.network_manual_save') }}
        </button>
      </div>
      <p v-if="manualError" class="mt-1 text-xs text-danger-bright" role="alert">{{ $t(manualError) }}</p>
    </div>

    <p v-if="trusted.length" class="mt-3 break-words text-xs text-storm" data-trusted-now>
      {{ $t('welcome.network_trusted_now', { subnets: trusted.join(', ') }) }}
    </p>
    <p v-if="trusted.length && !autoPairing" class="mt-1 text-xs text-storm">{{ $t('welcome.network_auto_pairing_off') }}</p>

    <div
      v-if="outcome"
      class="mt-3 rounded-2xl border px-4 py-3 text-sm"
      :class="outcome.ok ? 'border-success/25 bg-success/10 text-success-bright' : 'border-danger/25 bg-danger/10 text-danger-bright'"
      role="status"
    >
      {{ $t(outcome.textKey, outcome.params || {}) }}
    </div>
    <p class="mt-3 text-xs text-storm">{{ $t('welcome.network_trusted_later') }}</p>
  </div>
</template>

<script setup>
import { computed, getCurrentInstance, onMounted, ref, watch } from 'vue'
import { isTruthySetting } from '../client-settings-sync.js'
import {
  normalizeCidr,
  parseTrustedSubnets,
  serializeTrustedSubnets,
  trustsSubnet,
  withTrustedSubnet,
} from '../trusted-network.js'

// Messages are chosen in script code, so the component keeps its own $t, as the wizard does.
const instance = getCurrentInstance()
const $t = (key, params) => instance?.proxy?.$t?.(key, params) ?? key

const props = defineProps({
  configData: { type: Object, default: null },
  patchConfig: { type: Function, required: true },
})
const emit = defineEmits(['saved'])

const networks = ref([])
const loading = ref(true)
const loadFailed = ref(false)
const manualCidr = ref('')
const manualError = ref('')
const busy = ref(false)
const outcome = ref(null)
const trusted = ref(parseTrustedSubnets(props.configData?.trusted_subnets))
const autoPairing = ref(isTruthySetting(props.configData?.trusted_subnet_auto_pairing))

watch(() => [props.configData?.trusted_subnets, props.configData?.trusted_subnet_auto_pairing], ([subnets, pairing]) => {
  if (busy.value) return
  trusted.value = parseTrustedSubnets(subnets)
  autoPairing.value = isTruthySetting(pairing)
})

const trustedAndOn = computed(() => autoPairing.value ? trusted.value : [])

function isTrusted(cidr) {
  return trustsSubnet(trustedAndOn.value, cidr)
}

function interfacesOf(network) {
  const names = Array.isArray(network.interfaces) && network.interfaces.length ? network.interfaces : [network.interface]
  return names.filter(Boolean).join(', ')
}

onMounted(async () => {
  try {
    const response = await fetch('./api/setup/networks', { credentials: 'include' })
    const payload = response.ok ? await response.json() : null
    if (!payload || payload.status === false || !Array.isArray(payload.networks)) {
      loadFailed.value = true
      return
    }
    networks.value = payload.networks.filter((network) => network && typeof network.cidr === 'string')
  } catch {
    loadFailed.value = true
  } finally {
    loading.value = false
  }
})

// Adds the network to what is already trusted, never replacing an entry, and turns on pairing
// without a PIN, which is what makes a trusted network mean anything.
async function trust(cidr) {
  busy.value = true
  outcome.value = null
  try {
    const next = withTrustedSubnet(trusted.value, cidr)
    const result = await props.patchConfig({
      trusted_subnets: serializeTrustedSubnets(next),
      trusted_subnet_auto_pairing: 'enabled',
    })
    if (!result.ok) {
      outcome.value = { ok: false, textKey: 'welcome.network_trust_save_failed', params: { error: result.error || '' } }
      return false
    }
    trusted.value = next
    autoPairing.value = true
    outcome.value = {
      ok: true,
      textKey: result.restartRequired === false ? 'welcome.network_trust_saved' : 'welcome.network_trust_saved_restart',
      params: { cidr },
    }
    emit('saved', result)
    return true
  } finally {
    busy.value = false
  }
}

async function trustManual() {
  if (busy.value || !manualCidr.value.trim()) return
  const normalized = normalizeCidr(manualCidr.value)
  if (normalized.error) {
    manualError.value = normalized.error === 'too_broad' ? 'welcome.network_manual_too_broad' : 'welcome.network_manual_invalid'
    return
  }
  manualError.value = ''
  if (await trust(normalized.cidr)) {
    manualCidr.value = ''
  }
}
</script>
