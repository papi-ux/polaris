<script setup>
import { computed, ref, toRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import ConfirmActionDialog from './ConfirmActionDialog.vue'
import { useTailscalePeer } from '../composables/useTailscalePeer.js'
import { useToast } from '../composables/useToast'
import { connectionChip, sessionElapsedLabel } from '../live-session.js'

// The stream at a glance and what can be done to it: the game, the device playing it, the host's
// confirmed connection, how long it has run, then Disconnect Client and End. A fact the host has
// not reported is left out rather than filled in. Loaded while a stream is live, so nothing here
// weighs on the page before one.
const props = defineProps({
  title: { type: String, default: '' },
  appId: { type: String, default: '' },
  artSrc: { type: String, default: '' },
  device: { type: String, default: '' },
  stats: { type: Object, default: null },
  // When this page saw the stream begin, for a host that does not send its own count.
  witnessedStartMs: { type: Number, default: 0 },
  // Disconnect needs the one pairing record the stream names; End is allowed where the host says
  // which app is running.
  canDisconnect: { type: Boolean, default: false },
  canEnd: { type: Boolean, default: false },
  disconnecting: { type: Boolean, default: false },
})
const emit = defineEmits(['disconnect', 'ended'])

const { t } = useI18n()
const { toast: showToast } = useToast()
const name = computed(() => props.title || t('mission_control.session_card_label'))
const connection = computed(() => connectionChip(tailscalePeer.value, t))
const tailscalePeer = useTailscalePeer(toRef(props, 'stats'))
// Reading stats re-computes the label on every report.
const elapsed = computed(() => sessionElapsedLabel({ hostSeconds: props.stats?.session_elapsed_s, witnessedStartMs: props.witnessedStartMs }))
// A cover that fails to load gives way to the plain tile, and a new cover gets its own attempt.
const artFailed = ref(false)
watch(() => props.artSrc, () => { artFailed.value = false })
const showArt = computed(() => Boolean(props.artSrc) && !artFailed.value)

const endConfirmOpen = ref(false)
const ending = ref(false)
const endTarget = computed(() => JSON.stringify([props.stats?.stream_instance_id ?? null, props.appId]))
const confirmedEndTarget = ref(null)
function retireEndConfirmation() {
  endConfirmOpen.value = false
  confirmedEndTarget.value = null
}
function requestEnd() {
  if (!props.canEnd || ending.value) return
  confirmedEndTarget.value = endTarget.value
  endConfirmOpen.value = true
}
// A new stream can have the same app and label. Retire the old intent even if End stays allowed;
// ordinary telemetry updates for the same stream/app do not dismiss it.
watch(endTarget, retireEndConfirmation, { flush: 'sync' })
// A confirmation left open for a session that can no longer be ended closes untouched.
watch(() => props.canEnd, (allowed) => {
  if (!allowed && !ending.value) retireEndConfirmation()
})
async function endSession() {
  if (!props.canEnd || ending.value || !endConfirmOpen.value || confirmedEndTarget.value !== endTarget.value) return
  ending.value = true
  try {
    const response = await fetch('./api/apps/close', {
      method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
    })
    const result = await response.json().catch(() => ({}))
    if (!response.ok || result.status !== true) throw new Error(`HTTP ${response.status}`)
    endConfirmOpen.value = false
    showToast(t('mission_control.session_end_success'), 'success')
    emit('ended')
  } catch (e) {
    console.error(e)
    showToast(t('mission_control.session_end_error'), 'error')
  } finally {
    ending.value = false
  }
}
</script>

<template>
  <section class="mc-session" :aria-label="t('mission_control.session_card_label')" data-live-session-card>
    <div class="mc-session-layout">
      <div class="mc-session-main">
        <div class="mc-session-art" aria-hidden="true">
          <img v-if="showArt" :src="artSrc" alt="" data-session-art @error="artFailed = true" />
          <svg v-else viewBox="0 0 24 24" width="24" height="24" fill="currentColor" data-session-art-fallback><path d="M12 2l2.2 7.8L22 12l-7.8 2.2L12 22l-2.2-7.8L2 12l7.8-2.2z" /></svg>
        </div>
        <div class="mc-session-copy">
          <p class="mc-session-title" data-session-title>{{ name }}</p>
          <dl class="mc-session-facts">
            <div v-if="device" data-session-device>
              <dt>{{ t('mission_control.session_device') }}</dt>
              <dd>{{ device }}</dd>
            </div>
            <div v-if="connection" data-session-connection>
              <dt>{{ t('mission_control.session_connection') }}</dt>
              <dd>
                <span class="control-chip" :title="connection.note" :data-session-transport="connection.transport">{{ connection.label }}</span>
                <span class="sr-only">{{ connection.note }}</span>
              </dd>
            </div>
            <div v-if="elapsed" data-session-elapsed>
              <dt>{{ t('mission_control.session_elapsed') }}</dt>
              <dd class="mc-session-time">{{ elapsed }}</dd>
            </div>
          </dl>
        </div>
      </div>
      <div v-if="canDisconnect || canEnd" class="mc-session-actions">
        <button
          v-if="canDisconnect"
          type="button"
          class="focus-ring dashboard-action-button dashboard-action-button-ghost disabled:cursor-wait disabled:opacity-70"
          :disabled="disconnecting"
          :aria-label="device ? t('mission_control.session_disconnect_device', { device }) : undefined"
          data-disconnect-client
          @click="$emit('disconnect')"
        >
          {{ t('dashboard.disconnect_client') }}
        </button>
        <button
          v-if="canEnd"
          type="button"
          class="focus-ring dashboard-action-button mc-action-danger disabled:cursor-wait disabled:opacity-70"
          :disabled="ending"
          :aria-label="t('mission_control.session_end_title', { name })"
          data-end-session
          @click="requestEnd"
        >
          {{ t('mission_control.session_end') }}
        </button>
      </div>
    </div>
    <ConfirmActionDialog
      v-model="endConfirmOpen"
      :title="t('mission_control.session_end_confirm_title')"
      :message="t('mission_control.session_end_confirm_message', { name })"
      :impact-items="[t('mission_control.session_end_impact_stream'), t('mission_control.session_end_impact_game')]"
      :confirm-label="t('mission_control.session_end')"
      :cancel-label="t('_common.cancel')"
      :pending="ending"
      :pending-label="t('mission_control.session_end_pending')"
      @confirm="endSession"
    />
  </section>
</template>

<style scoped>
/* One row on a tablet or wider; on a phone the art and facts sit above actions that take the card's
   full width. Its own width decides, so enlarged text counts too. */
.mc-session {
  container: mc-session / inline-size;
  border: 1px solid var(--edge-raised);
  border-radius: 0.75rem;
  background: var(--surface-raised);
}

.mc-session-main,
.mc-session-actions {
  padding: 0.875rem 1rem;
}

.mc-session-main {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.875rem;
  min-width: 0;
}

.mc-session-art {
  display: grid;
  place-items: center;
  flex: none;
  width: 3.25rem;
  aspect-ratio: 2 / 3;
  overflow: hidden;
  border-radius: 0.375rem;
  background: var(--color-deep);
  color: var(--color-storm);
}

.mc-session-art img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}

.mc-session-copy {
  flex: 1 1 12rem;
  min-width: 0;
}

.mc-session-title {
  font-size: 1.0625rem;
  font-weight: 700;
  line-height: 1.25;
  color: var(--color-ice);
  overflow-wrap: anywhere;
}

.mc-session-facts {
  display: flex;
  flex-wrap: wrap;
  gap: 0.375rem 1.25rem;
  margin-top: 0.375rem;
}

.mc-session-facts dt {
  font-size: 0.75rem;
  line-height: 1.3;
  color: var(--color-support);
}

.mc-session-facts dd {
  font-size: 0.875rem;
  line-height: 1.4;
  color: var(--color-silver);
  overflow-wrap: anywhere;
}

.mc-session-time {
  font-variant-numeric: tabular-nums;
}

.mc-session-actions {
  display: grid;
  grid-auto-flow: column;
  grid-auto-columns: minmax(0, 1fr);
  gap: 0.5rem;
  padding-top: 0;
}

.mc-session-actions .dashboard-action-button {
  min-height: 2.75rem;
}

@container mc-session (max-width: 20rem) {
  .mc-session-actions {
    grid-auto-flow: row;
  }
}

@container mc-session (min-width: 36rem) {
  .mc-session-layout {
    display: flex;
    align-items: center;
  }

  .mc-session-main {
    flex: 1;
  }

  .mc-session-actions {
    flex: none;
    padding-top: 0.875rem;
  }
}
</style>
