<script setup>
import { computed, ref, watch } from 'vue'
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
const artFailed = ref(false)
const artLoaded = ref(false)
watch(() => props.artSrc, () => { artFailed.value = false; artLoaded.value = false })
const showArt = computed(() => Boolean(props.artSrc) && !artFailed.value && !props.hideArt)
const step = computed(() => titleStep(props.title))
const metaItems = computed(() => props.meta.filter(Boolean))
</script>

<template>
  <section class="mc-stage" :class="[`is-${mode}`, { 'has-art': showArt }]" :aria-labelledby="titleId" :aria-busy="mode === 'loading' ? 'true' : undefined" data-mission-stage :data-stage-mode="mode">
    <div class="mc-stage-art" aria-hidden="true">
      <img v-if="showArt" class="mc-stage-backdrop" :class="{ 'is-loaded': artLoaded }" :src="artSrc" alt=""
        @load="artLoaded = true" @error="artFailed = true" />
    </div>
    <div class="mc-stage-scrim" aria-hidden="true"></div>
    <div class="mc-frame-marks" aria-hidden="true"><i></i><i></i><i></i><i></i></div>

    <div class="mc-stage-top">
      <span class="mc-slate"><span class="mc-marker" :class="`is-${mode}`" aria-hidden="true"></span>{{ slate }}</span>
      <span v-if="timecode" class="mc-timecode">{{ timecode }}</span>
    </div>

    <div class="mc-stage-body">
      <div v-if="showArt" class="mc-keyart" :class="{ 'is-loaded': artLoaded }" aria-hidden="true" data-stage-artwork>
        <img :src="artSrc" alt="" @load="artLoaded = true" @error="artFailed = true" />
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
