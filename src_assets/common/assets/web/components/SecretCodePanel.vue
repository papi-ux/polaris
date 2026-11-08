<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { isStarTrailCode } from '../secret-code.js'

// A small panel behind the version. It starts nothing and changes nothing on the host: POLARIS
// draws star trails around the logo, still under reduced motion, and any other text is ignored.
const LOGO_SRC = '/images/logo-polaris.svg'
const emit = defineEmits(['close'])
const { t } = useI18n()
const panelEl = ref(null)
const inputEl = ref(null)
const code = ref('')
const inputId = `secret-code-${Math.random().toString(36).slice(2)}`
// Bumped per play, so entering the code again plays the trails again.
const plays = ref(0)
const still = ref(false)
const announcement = computed(() => (plays.value ? t('secret_code.star_trail') : ''))
// Arcs of one sky turning around the pole: each ring's stretch of arc and how far it has turned.
const trails = [
  { r: 58, dash: '22 78', turn: 40, from: -70 },
  { r: 66, dash: '14 86', turn: 150, from: -90 },
  { r: 74, dash: '26 74', turn: 255, from: -110 },
  { r: 82, dash: '16 84', turn: 20, from: -130 },
  { r: 90, dash: '20 80', turn: 200, from: -150 },
]
let previousFocus = null

function submit() {
  if (isStarTrailCode(code.value)) {
    still.value = window.matchMedia?.('(prefers-reduced-motion: reduce)')?.matches === true
    plays.value++
  }
  code.value = ''
}

function close() {
  emit('close')
}

function handleKeydown(event) {
  if (event.key === 'Escape') {
    event.preventDefault()
    event.stopPropagation()
    close()
    return
  }
  if (event.key !== 'Tab') return
  const controls = Array.from(panelEl.value?.querySelectorAll('button, input') || [])
  if (!controls.length) return
  const first = controls[0]
  const last = controls[controls.length - 1]
  if (event.shiftKey && document.activeElement === first) {
    event.preventDefault()
    last.focus()
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault()
    first.focus()
  }
}

onMounted(async () => {
  previousFocus = document.activeElement
  await nextTick()
  inputEl.value?.focus()
})
onBeforeUnmount(() => {
  if (previousFocus instanceof HTMLElement && document.contains(previousFocus)) previousFocus.focus()
})
</script>

<template>
  <Teleport to="body">
    <div class="fixed inset-0 z-[80] flex items-start justify-center overflow-y-auto overscroll-contain bg-void/80 px-4 py-6" @click.self="close">
      <section
        ref="panelEl"
        role="dialog"
        aria-modal="true"
        :aria-labelledby="inputId + '-label'"
        class="my-auto w-full max-w-sm rounded-2xl border border-storm/30 bg-deep p-5 text-silver shadow-2xl"
        data-secret-code-panel
        @keydown="handleKeydown"
      >
        <div v-if="plays" :key="plays" class="star-trail" :class="{ 'is-still': still }" role="img" :aria-label="t('secret_code.star_trail')" data-star-trail :data-star-trail-motion="still ? 'still' : 'animated'">
          <svg viewBox="0 0 200 200" aria-hidden="true">
            <g v-for="trail in trails" :key="trail.r" :style="{ '--trail-from': `${trail.from}deg` }" class="star-trail-ring">
              <circle cx="100" cy="100" :r="trail.r" pathLength="100" :stroke-dasharray="trail.dash" :transform="`rotate(${trail.turn} 100 100)`" />
            </g>
          </svg>
          <img :src="LOGO_SRC" alt="" />
        </div>
        <form @submit.prevent="submit">
          <label :id="inputId + '-label'" :for="inputId" class="block text-base font-semibold text-silver">{{ t('secret_code.label') }}</label>
          <input
            :id="inputId"
            ref="inputEl"
            v-model="code"
            type="text"
            maxlength="32"
            autocomplete="off"
            autocapitalize="characters"
            autocorrect="off"
            spellcheck="false"
            enterkeyhint="go"
            class="focus-ring mt-2 min-h-11 w-full rounded-lg border border-storm/30 bg-void/60 px-3 text-base text-silver"
            data-secret-code-input
          />
          <div class="mt-4 flex justify-end gap-2">
            <button type="button" class="focus-ring min-h-11 rounded-lg border border-storm/30 px-4 text-sm font-medium text-silver" data-secret-code-close @click="close">{{ t('secret_code.close') }}</button>
            <button type="submit" class="focus-ring min-h-11 rounded-lg border border-ice/40 px-4 text-sm font-semibold text-ice" data-secret-code-submit>{{ t('secret_code.submit') }}</button>
          </div>
        </form>
        <p class="sr-only" role="status" aria-live="polite">{{ announcement }}</p>
      </section>
    </div>
  </Teleport>
</template>

<style scoped>
/* The trails end on the same picture reduced motion shows from the start. */
.star-trail {
  position: relative;
  width: min(12rem, 60vw);
  aspect-ratio: 1;
  margin: 0 auto 1rem;
  color: var(--color-ice);
}

.star-trail svg {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  fill: none;
  stroke: currentColor;
  stroke-width: 1.5;
  stroke-linecap: round;
  overflow: visible;
}

.star-trail img {
  position: absolute;
  inset: 32%;
  width: 36%;
  height: 36%;
  object-fit: contain;
}

.star-trail-ring {
  transform-origin: 100px 100px;
}

@media (prefers-reduced-motion: no-preference) {
  .star-trail:not(.is-still) .star-trail-ring {
    animation: star-trail-turn 2.4s cubic-bezier(0.2, 0.7, 0.2, 1) 1 both;
  }
}

@keyframes star-trail-turn {
  from {
    opacity: 0;
    transform: rotate(var(--trail-from));
  }

  to {
    opacity: 1;
    transform: rotate(0deg);
  }
}
</style>
