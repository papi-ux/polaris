<script setup>
import { computed, reactive, watch } from 'vue'
import { titleStep } from '../mission-control.js'

// The Mission Control stage: one title card per state, library artwork behind it, the main action
// on it and the readings along its lower edge. It only lays out what the dashboard passes in.
const props = defineProps({
  mode: { type: String, default: 'idle' },
  slate: { type: String, default: '' },
  timecode: { type: String, default: '' },
  kicker: { type: String, default: '' },
  title: { type: String, default: '' },
  meta: { type: Array, default: () => [] },
  note: { type: String, default: '' },
  noteTone: { type: String, default: '' },
  artSrc: { type: String, default: '' },
  // The live preview takes the artwork's place when the viewer asks for it.
  hideArt: { type: Boolean, default: false },
  // An expanded preview takes the stage's full width on its own row.
  previewExpanded: { type: Boolean, default: false },
})

const titleId = `mission-stage-title-${Math.random().toString(36).slice(2)}`
// Load state is kept per image, so a title the viewer features again fades back in at once and a
// cover that failed is retried only when it is featured anew.
const loadedArt = reactive(new Set())
const failedArt = reactive(new Set())
watch(() => props.artSrc, src => { failedArt.delete(src) })
// The artwork the stage is built from. Each change of image crossfades: the new layer fades in over
// the one leaving.
const art = computed(() => ({ kind: 'poster', src: props.artSrc }))
const showArt = computed(() => Boolean(art.value.src) && !failedArt.has(art.value.src) && !props.hideArt)
const artLayers = computed(() => (showArt.value ? [art.value.src] : []))
const step = computed(() => titleStep(props.title))
const metaItems = computed(() => props.meta.filter(Boolean))
</script>

<template>
  <section class="mc-stage" :class="[`is-${mode}`, { 'has-art': showArt }]" :aria-labelledby="titleId" :aria-busy="mode === 'loading' ? 'true' : undefined" data-mission-stage :data-stage-mode="mode">
    <div class="mc-stage-art" aria-hidden="true">
      <TransitionGroup name="mc-xfade">
        <img v-for="src in artLayers" :key="src" class="mc-stage-backdrop" :class="{ 'is-loaded': loadedArt.has(src) }" :src="src" alt=""
          @load="loadedArt.add(src)" @error="failedArt.add(src)" />
      </TransitionGroup>
    </div>
    <div class="mc-stage-scrim" aria-hidden="true"></div>

    <div class="mc-stage-top">
      <span class="mc-slate"><span class="mc-marker" :class="`is-${mode}`" aria-hidden="true"></span>{{ slate }}</span>
      <span v-if="timecode" class="mc-timecode">{{ timecode }}</span>
    </div>

    <div class="mc-stage-body">
      <div v-if="showArt" class="mc-keyart" aria-hidden="true" data-stage-artwork>
        <TransitionGroup name="mc-xfade">
          <img v-for="src in artLayers" :key="src" :class="{ 'is-loaded': loadedArt.has(src) }" :src="src" alt=""
            @load="loadedArt.add(src)" @error="failedArt.add(src)" />
        </TransitionGroup>
      </div>
      <!-- The opted-in preview sits where the key art was, whole and uncropped, inside the composition. -->
      <div v-if="$slots.preview" class="mc-stage-preview" :class="{ 'is-expanded': previewExpanded }"><slot name="preview" /></div>
      <div class="mc-stage-copy">
        <p v-if="kicker" class="mc-kicker">{{ kicker }}</p>
        <template v-if="mode === 'loading'">
          <h2 :id="titleId" class="sr-only">{{ title }}</h2>
          <div class="mt-3 grid gap-3" aria-hidden="true">
            <div class="skeleton-shimmer h-12 w-full max-w-sm rounded-lg"></div>
            <div class="skeleton-shimmer h-4 w-full max-w-64 rounded-md"></div>
          </div>
        </template>
        <h2 v-else :id="titleId" class="mc-stage-title" :class="`is-${step}`" :data-title-step="step">{{ title }}</h2>
        <span class="mc-rule" aria-hidden="true"></span>
        <p v-if="metaItems.length" class="mc-stage-meta">
          <span v-for="(item, index) in metaItems" :key="index">{{ item }}</span>
        </p>
        <p v-if="note" class="mc-stage-note" :class="noteTone">{{ note }}</p>
        <slot name="reason" />
        <div v-if="$slots.actions" class="mc-stage-actions mt-4 flex flex-wrap gap-2.5"><slot name="actions" /></div>
      </div>
      <div v-if="$slots.aside" class="mc-stage-aside"><slot name="aside" /></div>
    </div>

    <slot name="signals" />
  </section>
</template>
