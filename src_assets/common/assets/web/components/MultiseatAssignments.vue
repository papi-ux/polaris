<template>
  <section v-if="state.enabled || loadError" class="section-card" aria-labelledby="profile-assignment-title"
           :aria-busy="loading || creating || managing">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div class="min-w-0">
        <h2 id="profile-assignment-title" class="section-title">{{ $t('spaces.your_spaces') }}</h2>
        <p class="mt-1 max-w-2xl text-sm text-storm">{{ $t('spaces.a_space_keeps') }}</p>
      </div>
      <div v-if="state.enabled" class="flex flex-wrap items-center gap-2">
        <span class="meta-pill">{{ countLabel }}</span>
        <span v-if="state.capacity" class="control-chip" data-spaces-capacity>
          {{ $t('spaces.capacity', { active: state.capacity.concurrent_active, limit: state.capacity.concurrent_limit }) }}
        </span>
      </div>
    </div>
    <p v-if="loadError" class="mt-4 text-sm text-warning-bright" role="alert">{{ loadError }}</p>
    <p v-if="state.failed" class="mt-4 text-sm text-warning-bright" role="alert">{{ $t('spaces.restore_failed') }}</p>
    <p v-else-if="state.changing" class="mt-4 text-sm text-storm" role="status">{{ $t('spaces.applying') }}</p>
    <p v-else-if="state.enabled && !state.available" class="mt-4 text-sm text-storm" role="status">{{ $t('spaces.unavailable') }}</p>
    <p v-if="state.enabled && clientsReady && !devices.length" class="mt-4 text-sm text-storm">{{ $t('spaces.pair_first') }}</p>
    <p v-if="streamLock" :id="lockReasonId" class="mt-4 text-sm text-warning-bright" role="status" data-stream-lock>{{ streamLock }}</p>
    <SpacesList v-if="state.enabled" :profiles="state.profiles" :clients="clients" :manageable="state.management_available"
                :creation-available="state.creation_available"
                :removal-available="state.removal_available"
                :runtime-move-available="state.runtime_move_available" :runtime-move-job="state.runtime_move_job"
                :activity="loadError ? null : state.activity" :refreshing="loading"
                :locked="locked" :lock-reason-id="streamLock ? lockReasonId : ''" :ready="ready" :refresh="loadProfiles"
                @busy="managing = $event" />
    <MultiseatProfileCreate v-if="state.enabled && state.creation_available" :profiles="state.profiles"
                            :launchers="state.launchers || []" :job="state.runtime_move_job || null"
                           :locked="locked" :ready="ready" :refreshing="loading" :refresh="loadProfiles" @busy="creating = $event" />
    <SpacesDevices v-if="state.enabled" :state="state" :clients="clients"
                   :by-default="typeof state.desktop_by_default === 'boolean' ? state.desktop_by_default : undefined"
                   :access-available="state.access_available" :locked="locked" :lock-reason-id="streamLock ? lockReasonId : ''"
                   :refresh="loadProfiles" @busy="managing = $event" />
    <div class="mt-4 flex flex-wrap items-center justify-between gap-3">
      <p v-if="state.enabled && devices.length" class="text-xs text-storm">{{ $t('spaces.one_device') }}</p>
      <Button variant="ghost" size="sm" :loading="loading" :disabled="creating || managing" data-spaces-refresh @click="refresh">
        {{ loading ? $t('spaces.refreshing') : $t('spaces.refresh') }}
      </Button>
    </div>
  </section>
</template>

<script setup>
import { computed, inject, onMounted, ref } from 'vue'
import Button from './Button.vue'
import SpacesList from './SpacesList.vue'
import SpacesDevices from './SpacesDevices.vue'
import MultiseatProfileCreate from './MultiseatProfileCreate.vue'
import { useSpacesSnapshot } from '../composables/useSpacesSnapshot.js'
import { deviceNameLabels } from '../device-names.js'
import { activeSpaces, listedDevices } from '../spaces-devices.js'

const emit = defineEmits(['snapshot'])
const props = defineProps({
  clients: { type: Array, default: () => [] },
  clientsReady: { type: Boolean, default: true },
})
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const creating = ref(false), managing = ref(false)
const lockReasonId = 'spaces-stream-lock'

const { state, loading, loadError, load, start } = useSpacesSnapshot({
  busy: () => creating.value || managing.value,
  onSnapshot: next => emit('snapshot', { ...next }),
  messages: { load: t('spaces.load_failed'), verify: t('spaces.verify_failed') },
})

const countLabel = computed(() => {
  const count = activeSpaces(state).length
  return t(count === 1 ? 'spaces.count_one' : 'spaces.count_many', { count })
})
const nameLabels = computed(() => deviceNameLabels(props.clients, { t, fallback: t('spaces.paired_device') }))
const deviceName = client => nameLabels.value.get(client?.uuid) || client?.friendly_name || client?.name || t('spaces.paired_device')
const devices = computed(() => listedDevices(state, props.clients))
const ready = computed(() => state.available && !state.changing && !state.failed && !loadError.value)
// A live Space stream holds the catalog: every change waits for it, and the
// controls say so instead of sending the request to a 409.
const streamActivity = computed(() => Array.isArray(state.activity) ? state.activity : [])
const streamLock = computed(() => {
  const item = streamActivity.value[0]
  if (!item) return ''
  const device = props.clients.find(client => client.uuid === item.client_id)
  const name = device ? deviceName(device) : t('spaces.paired_device')
  return t(item.state === 'stopping' ? 'spaces.stream_lock_stopping' : 'spaces.stream_lock_running', { device: name })
})
const locked = computed(() => creating.value || managing.value || !!loadError.value ||
  state.changing || state.failed || !state.available || streamActivity.value.length > 0)

const loadProfiles = () => load()

async function refresh() {
  if (creating.value || managing.value || loading.value) return
  await loadProfiles()
}

onMounted(() => { start() })
</script>
