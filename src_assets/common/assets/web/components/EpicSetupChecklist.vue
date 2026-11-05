<template>
  <details ref="details" class="epic-setup" data-epic-private-setup :open="expanded" @toggle="expanded = $event.target.open" @keydown.esc.stop.prevent="close">
    <summary ref="entry">Epic Private Stream setup</summary>
    <section v-if="expanded" role="region" aria-labelledby="epic-setup-title">
      <h3 id="epic-setup-title">Epic Private Stream setup</h3>
      <p>Open Epic on this PC and select Rescan Sources, then quit Epic completely. Repeat after Polaris restarts or Epic updates.</p>
      <label for="epic-setup-game">Game to check</label>
      <select id="epic-setup-game" v-model="chosen">
        <option value="">Choose a game</option>
        <option v-for="game in choices" :key="game.key" :value="game.key">{{ game.name }}</option>
      </select>
      <p>{{ presentation.message }}</p>
      <p v-if="scanning">Scanning sources. Current setup facts are cleared until a fresh check completes.</p>
      <p v-if="switching">The launch provider is changing. Setup checks resume after it finishes.</p>
      <p v-if="presentation.notice">{{ presentation.notice }}</p>
      <p v-if="presentation.scanMessage">{{ presentation.scanMessage }}</p>
      <p v-if="scanError">The latest source scan failed. Any previous successful scan is historical; check again before relying on current setup.</p>
      <ol>
        <li v-for="row in presentation.rows" :key="row.id" :data-epic-row="row.id">
          <strong>{{ row.title }}</strong>
          <span class="epic-setup-state"><span aria-hidden="true">{{ row.status === 'Complete' ? '✓' : '–' }}</span> {{ row.status }}</span>
          <p>{{ row.message }}</p>
        </li>
      </ol>
      <div class="epic-setup-actions">
        <button type="button" :aria-disabled="loading || scanning || switching" :aria-busy="loading" @click="retry">Check again</button>
        <button type="button" :aria-disabled="scanning || switching" :aria-busy="scanning" @click="rescan">Rescan Sources</button>
      </div>
      <p>These are sampled checks, not a reservation. Polaris checks again when you launch. A checklist receipt does not change launch permissions.</p>
      <p>For a game's first play on this PC, use Mirror Desktop before Private Stream. A check cannot confirm sign-in, updates or cloud save synchronization.</p>
      <p class="epic-setup-live" role="status" aria-live="polite" aria-atomic="true">{{ presentation.summary }}</p>
    </section>
  </details>
</template>

<script setup>
import { computed, ref } from 'vue'
import { useEpicSetup } from '../composables/useEpicSetup'
import { readEpicTitle, readEpicScanEnvelope } from '../epic-setup-contract'
import { epicSetupPresentation } from '../epic-setup-state'

const props = defineProps({
  games: { type: Array, default: () => [] }, scanning: Boolean, switching: Boolean,
  scanError: { type: String, default: '' }, scan: { type: Function, required: true },
})
const expanded = ref(false), chosen = ref(''), entry = ref(null), details = ref(null)
const choices = computed(() => props.games.flatMap(game => {
  const title = readEpicTitle({ namespace_id: game.namespace, item_id: game.item_id, artifact_id: game.artifact_id })
  return title && game.provider !== 'heroic' ? [{ key: JSON.stringify(title), title, name: game.name }] : []
}))
const selection = computed(() => choices.value.find(game => game.key === chosen.value)?.title || null)
// Staging checkboxes are deliberately excluded. Provider/install changes are observations to discard.
const context = computed(() => JSON.stringify([props.scanning, props.switching, props.scanError,
  props.games.map(({ namespace, item_id, artifact_id, provider, install, installed_platform, runtime }) =>
    [namespace, item_id, artifact_id, provider, install, installed_platform, runtime])]))
const { state, loading, refresh, scanResult } = useEpicSetup({
  active: () => expanded.value && !props.scanning && !props.switching, selection, context,
})
const presentation = computed(() => epicSetupPresentation(state.value))
function retry() { if (!loading.value && !props.scanning && !props.switching) refresh() }
async function rescan() {
  if (props.scanning || props.switching) return
  let envelope
  try { envelope = await props.scan() } catch { /* The scanner owns its visible failure. No action retry. */ }
  // Never infer a receipt from overall scan success. The next GET owns the facts.
  scanResult(readEpicScanEnvelope(envelope))
}
function close() {
  expanded.value = false
  if (details.value) details.value.open = false
  entry.value?.focus()
}
</script>

<style scoped>
.epic-setup { margin-block: 1rem; color: var(--color-silver); overflow-wrap: anywhere; min-width: 0; }
.epic-setup summary, .epic-setup button, .epic-setup select { min-height: 44px; min-width: 44px; padding: .65rem; cursor: pointer; font: inherit; color: inherit; }
.epic-setup summary { width: fit-content; max-width: 100%; }
.epic-setup section { border: 1px solid var(--color-storm); border-radius: .5rem; padding: 1rem; }
.epic-setup h3, .epic-setup strong { font-weight: 600; }
.epic-setup p { margin-block: .6rem; }
.epic-setup ol { list-style: decimal; padding-inline-start: 1.5rem; }
.epic-setup li { margin-block: 1rem; }
.epic-setup-state { display: block; }
.epic-setup label { display: block; }
.epic-setup select { max-width: 100%; width: 100%; background: var(--color-deep); border: 1px solid var(--color-storm); }
.epic-setup-actions { display: flex; flex-wrap: wrap; gap: .5rem; }
.epic-setup button { background: var(--color-deep); border: 1px solid var(--color-storm); border-radius: .5rem; white-space: normal; }
.epic-setup [aria-disabled="true"] { cursor: wait; }
.epic-setup :is(summary, select, button):focus-visible { outline: 2px solid var(--color-ice); outline-offset: 2px; }
.epic-setup-live { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); }
@media (prefers-reduced-motion: reduce) { .epic-setup * { animation: none; transition: none; scroll-behavior: auto; } }
@media (forced-colors: active) {
  .epic-setup section, .epic-setup button, .epic-setup select { border-color: ButtonText; }
  .epic-setup :is(summary, select, button):focus-visible { outline-color: Highlight; }
}
</style>
