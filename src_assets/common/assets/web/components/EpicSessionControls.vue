<script setup>
import { computed, onMounted, ref, watch } from 'vue'
import Button from './Button.vue'
import { useEpicSession } from '../composables/useEpicSession.js'

// Mirror Desktop's game is unmanaged by default. A signed-in host-console owner may, in two
// explicit steps, adopt the exact observed game so that ending the session on
// purpose (End Session in the streaming client, or Disconnect Client here) can
// close it; otherwise it keeps running and the owner closes it in the Epic
// launcher. Cancelling sends nothing. This panel never ends or kills a process
// itself — End stays the existing session action. Private Stream instead owns
// its whole private session, including the launcher, and has no consent actions.
const { state, loading, loadError, actionError, acting, load, adopt, release, start } = useEpicSession()
// The verified session/revision the owner is confirming. An inconclusive poll
// pauses it and only the same verified revision resumes it; a different game or
// session drops it visibly, so it needs a fresh confirmation.
const consent = ref(null)
const consentReset = ref(false)

onMounted(() => { start() })

const canAdopt = computed(() => state.available && state.can_adopt && state.state === 'confirmable')
const isManaged = computed(() => state.available && state.state === 'managed')
const isPrivate = computed(() => state.state === 'session_managed')
const privateGameState = computed(() => state.game_state === 'managed' || state.game_state === 'waiting'
  ? state.game_state : 'unavailable')
const confirming = computed(() => consent.value !== null)
const consentPaused = computed(() => confirming.value && (state.state === 'unavailable' || Boolean(loadError.value)))
const consentMatches = () => state.session_id === consent.value.session_id && state.revision === consent.value.revision
const runtimeLabel = computed(() =>
  state.runtime === 'proton' ? 'Proton' : state.runtime === 'native' ? 'Native Linux' : 'Runtime unknown')

function dropConsent(visibly) { consent.value = null; consentReset.value = visibly }
// Default (pre) flush sees each poll's whole snapshot, never a half-applied one.
watch(() => [state.available, state.session_id, state.revision, state.state, state.can_adopt], () => {
  if (!state.available || isManaged.value || isPrivate.value) dropConsent(false) // ended, granted or session-managed
  else if (consent.value && state.session_id !== consent.value.session_id) dropConsent(true)
  else if (consent.value && state.state !== 'unavailable' && !(canAdopt.value && consentMatches())) dropConsent(true)
})

function startConfirm() { consent.value = { session_id: state.session_id, revision: state.revision }; consentReset.value = false }
function cancelConfirm() { dropConsent(false) } // no request, no signal
async function confirmAdopt() {
  if (!consent.value || consentPaused.value || acting.value) return
  if (!canAdopt.value || !consentMatches()) {
    dropConsent(true)
    return
  }
  const ok = await adopt()
  if (ok) consent.value = null
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
            <span class="control-chip">{{ isPrivate ? 'Private Stream' : 'Mirror Desktop' }}</span>
            <span class="control-chip">{{ runtimeLabel }}</span>
          </div>
        </div>
        <span class="meta-pill" data-epic-state>{{
          isPrivate ? (privateGameState === 'managed' ? 'Session-managed'
            : privateGameState === 'waiting' ? 'Waiting for game' : 'Game unverified')
          : isManaged ? 'Close on End: allowed'
          : state.state === 'closing' ? 'Closing…'
          : state.state === 'unavailable' ? 'State unavailable'
          : 'Close on End: off'
        }}</span>
      </div>

      <p v-if="state.uninspected_process_count > 0 && (canAdopt || isManaged)" class="mt-3 text-xs text-support" data-epic-inspection-limit>
        Some background programs could not be inspected. Permission applies only to this verified
        game; your other apps and the Epic launcher stay untouched.
        <span class="mt-1 block text-storm">{{ state.uninspected_process_count }} processes could not be inspected.</span>
      </p>

      <!-- Honest state when the backend has an active game it cannot read. -->
      <p v-if="state.code === 'epic_flatpak_game_control_unavailable'" class="mt-3 text-xs text-support" data-epic-flatpak-limit>
        Epic Flatpak supports Mirror Desktop only. End Session ends the stream and leaves the desktop game and
        launcher running. Disconnect and Resume leave them running too. Private Stream and close-game permission
        are unavailable for this package.
      </p>
      <template v-else-if="isPrivate">
        <p v-if="privateGameState === 'unavailable'" class="mt-3 text-xs text-warning-bright" data-epic-private-unavailable>
          Polaris can't verify the requested game. End still closes the whole private session.
        </p>
        <p class="mt-3 text-sm font-semibold text-silver" data-epic-private-session>
          End Session closes this private session, including Epic.
        </p>
        <p class="mt-1 text-xs text-support">Disconnect and Resume keep it running.</p>
        <details v-if="state.message" class="mt-3 text-xs text-support" data-epic-private-details>
          <summary class="cursor-pointer text-silver">How ending this session works</summary>
          <p class="mt-2">{{ state.message }}</p>
        </details>
      </template>

      <p v-else-if="state.state === 'unavailable'" class="mt-3 text-xs text-warning-bright" data-epic-unavailable>
        Polaris can't verify this game's current state. It will not switch your permission to another process.
      </p>

      <p v-else-if="state.state === 'closing'" class="mt-3 text-xs text-storm" data-epic-closing>
        Polaris is asking this game to close.
      </p>

      <!-- Managed: Polaris holds the owner's grant; offer to hand it back. -->
      <template v-else-if="isManaged">
        <p class="mt-3 text-sm font-semibold text-silver">
          Close on End is enabled. No further setup is needed.
        </p>
        <p class="mt-1 text-xs text-support">
          End Session asks this game to close. Disconnect and Resume in your streaming client keep it running.
        </p>
        <details class="mt-3 text-xs text-support">
          <summary class="cursor-pointer text-silver">How close permission works</summary>
          <p class="mt-2">
            Polaris will ask this exact game to close when the session ends: End Session in your streaming client,
            or Disconnect Client here in Mission Control. Disconnect and Resume in your streaming client keep it
            running. If Polaris cannot verify or close the game, it will tell you to close it on the host. The
            launcher and other games stay untouched. A restarted game needs a new confirmation.
          </p>
        </details>
        <div class="mt-3">
          <Button variant="ghost" size="sm" :loading="acting" :disabled="acting" data-epic-release @click="leaveRunning">
            Turn off close on End
          </Button>
        </div>
      </template>

      <!-- Default unmanaged: explain manual close, and offer the two-step grant when adoptable. -->
      <template v-else>
        <p class="mt-3 text-xs text-support" data-epic-unmanaged>
          Polaris is not managing this game's lifetime. Ending the session (End Session in your streaming client, or
          Disconnect Client here) leaves it running — close it yourself in the Epic launcher. Disconnect and Resume in
          your streaming client keep it running too.
        </p>

        <div v-if="canAdopt && !confirming" class="mt-3">
          <Button variant="outline" size="sm" data-epic-adopt-start @click="startConfirm">
            Allow close on End Session
          </Button>
        </div>
      </template>

      <p v-if="consentReset" class="mt-3 text-xs text-warning-bright" data-epic-consent-reset>
        The game or session changed before you confirmed, so nothing was granted. Confirm again if you still want it.
      </p>

      <!-- Step two: explicit consent before any request is sent. Held, not dropped, while verification is paused. -->
      <div v-if="confirming" class="mt-3 rounded-lg border border-storm/25 bg-deep/55 p-3" data-epic-adopt-consent>
        <div class="text-sm font-semibold text-silver">Allow Polaris to close this game on End Session?</div>
        <p class="mt-1 text-xs text-support">
          This does not close the game now. It lets Polaris ask this exact game to close when the session ends on
          purpose: End Session in your streaming client (Nova or Moonlight), or Disconnect Client here in Mission
          Control. Disconnect and Resume in your streaming client keep the game running. Polaris is watching the game
          currently running for this Epic session, which may have been started outside Polaris. If the game restarts,
          you'll need to confirm again.
        </p>
        <p v-if="consentPaused" class="mt-2 text-xs text-warning-bright" data-epic-consent-paused>
          Verification paused. Polaris can't verify this game right now, so you can't confirm yet. Your choice is kept
          only if the same game is verified again.
        </p>
        <div class="mt-3 flex flex-wrap gap-2">
          <Button variant="primary" size="sm" :loading="acting" :disabled="acting || consentPaused" data-epic-adopt-confirm @click="confirmAdopt">
            Yes, allow close on End Session
          </Button>
          <Button variant="outline" size="sm" :disabled="acting" data-epic-adopt-cancel @click="cancelConfirm">
            Cancel
          </Button>
        </div>
      </div>

      <Button v-if="state.state === 'unavailable' && state.can_release" class="mt-3" variant="outline" size="sm"
        :loading="acting" :disabled="acting" data-epic-release @click="leaveRunning">
        Turn off close on End
      </Button>

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
