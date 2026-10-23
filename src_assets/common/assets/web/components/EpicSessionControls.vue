<script setup>
import { computed, onMounted, ref } from 'vue'
import Button from './Button.vue'
import { useEpicSession } from '../composables/useEpicSession.js'

// Owner-confirmed control for an active Epic Preview game in Mission Control.
// The game is unmanaged by default. A signed-in host-console owner may, in two
// explicit steps, adopt the exact observed game so that ending the session can
// close it; otherwise End leaves it running and the owner closes it in the
// Epic launcher. Cancelling sends nothing. This panel never ends or kills a
// process itself — End stays the existing session action.
const { state, loading, loadError, actionError, acting, load, adopt, release, start } = useEpicSession()
const confirming = ref(false)

onMounted(() => { start() })

const canAdopt = computed(() => state.available && state.can_adopt && state.state === 'confirmable')
const isManaged = computed(() => state.available && state.state === 'managed')
const runtimeLabel = computed(() =>
  state.runtime === 'proton' ? 'Proton' : state.runtime === 'native' ? 'Native Linux' : 'Runtime unknown')

function startConfirm() { confirming.value = true }
function cancelConfirm() { confirming.value = false } // no request, no signal
async function confirmAdopt() {
  const ok = await adopt()
  if (ok) confirming.value = false
}
async function leaveRunning() { await release() }
</script>

<template>
  <!-- Only shown when there is an active Epic session, or when its status could not be read. -->
  <section
    v-if="state.available || loadError"
    class="rounded-xl border border-storm/20 bg-deep/40 p-4"
    data-epic-session-controls
  >
    <template v-if="state.available">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <div class="min-w-0">
          <div class="text-sm font-semibold text-silver truncate" data-epic-app-name>{{ state.app_name || 'Epic game' }}</div>
          <div class="mt-1 flex flex-wrap items-center gap-1.5">
            <span class="control-chip">Epic (Preview)</span>
            <span class="control-chip">Mirror Desktop</span>
            <span class="control-chip">{{ runtimeLabel }}</span>
          </div>
        </div>
        <span class="meta-pill" data-epic-state>{{
          isManaged ? 'Polaris will close on End'
          : state.state === 'closing' ? 'Closing…'
          : state.state === 'unavailable' ? 'State unavailable'
          : 'Unmanaged'
        }}</span>
      </div>

      <!-- Honest state when the backend has an active game it cannot read. -->
      <p v-if="state.state === 'unavailable'" class="mt-3 text-xs text-warning-bright" data-epic-unavailable>
        Polaris can't read this game's state right now. It is still running; nothing has been stopped.
      </p>

      <p v-else-if="state.state === 'closing'" class="mt-3 text-xs text-storm" data-epic-closing>
        Polaris is asking this game to close.
      </p>

      <!-- Managed: Polaris holds the owner's grant; offer to hand it back. -->
      <template v-else-if="isManaged">
        <p class="mt-3 text-xs text-support">
          You asked Polaris to close this game. Ending the session will close only this game, never the Epic
          launcher, your sign-in or other games. If it restarts you'll confirm again.
        </p>
        <div class="mt-3">
          <Button variant="outline" size="sm" :loading="acting" :disabled="acting" data-epic-release @click="leaveRunning">
            Leave it running
          </Button>
        </div>
      </template>

      <!-- Default unmanaged: explain manual close, and offer the two-step grant when adoptable. -->
      <template v-else>
        <p class="mt-3 text-xs text-support" data-epic-unmanaged>
          This game is running unmanaged. Ending the session leaves it running — close it yourself in the Epic
          launcher. Disconnecting and resuming keep it running.
        </p>

        <template v-if="canAdopt">
          <div v-if="!confirming" class="mt-3">
            <Button variant="outline" size="sm" data-epic-adopt-start @click="startConfirm">
              Let Polaris close this game
            </Button>
          </div>

          <!-- Step two: explicit consent before any request is sent. -->
          <div v-else class="mt-3 rounded-lg border border-storm/25 bg-deep/55 p-3" data-epic-adopt-consent>
            <div class="text-sm font-semibold text-silver">Let Polaris close this game?</div>
            <p class="mt-1 text-xs text-support">
              Polaris is watching the game currently running for this Epic session. It may have been started
              outside Polaris. If you confirm, ending the session can close this game. By default Polaris leaves
              it running on disconnect or End. If the game restarts, you'll need to confirm again.
            </p>
            <div class="mt-3 flex flex-wrap gap-2">
              <Button variant="primary" size="sm" :loading="acting" :disabled="acting" data-epic-adopt-confirm @click="confirmAdopt">
                Yes, let Polaris close it
              </Button>
              <Button variant="outline" size="sm" :disabled="acting" data-epic-adopt-cancel @click="cancelConfirm">
                Cancel
              </Button>
            </div>
          </div>
        </template>
      </template>

      <!-- An adopt/release that was refused or went stale: surfaced once, never auto-retried. -->
      <p v-if="actionError" class="mt-3 text-xs text-danger-bright" role="alert" data-epic-action-error>
        {{ actionError }}
      </p>
    </template>

    <!-- The session status itself could not be read (transport / auth). -->
    <template v-else>
      <div class="flex flex-wrap items-center justify-between gap-2">
        <div class="text-xs text-warning-bright" data-epic-load-error>{{ loadError }}</div>
        <Button variant="outline" size="sm" :loading="loading" :disabled="loading" data-epic-retry @click="load">
          Retry
        </Button>
      </div>
    </template>
  </section>
</template>
