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
  // The title's Steam background and logo from the host, when it has them.
  heroSrc: { type: String, default: '' },
  logoSrc: { type: String, default: '' },
  // The live preview takes the artwork's place when the viewer asks for it.
  hideArt: { type: Boolean, default: false },
  // An expanded preview takes the stage's full width on its own row.
  previewExpanded: { type: Boolean, default: false },
})

const titleId = `mission-stage-title-${Math.random().toString(36).slice(2)}`
// Load state is kept per image, so a title the viewer features again fades back in at once and an
// image that failed is retried only when it is featured anew.
const loadedArt = reactive(new Set())
const failedArt = reactive(new Set())
for (const key of ['artSrc', 'heroSrc', 'logoSrc']) watch(() => props[key], src => { failedArt.delete(src) })
const usable = src => Boolean(src) && !failedArt.has(src)
// The cover frames the title. Behind it is the title's Steam background when the host has one,
// otherwise a softened copy of the cover. Each change of image crossfades: the new layer fades in
// over the one leaving.
const showArt = computed(() => usable(props.artSrc) && !props.hideArt)
const backdropLayers = computed(() => {
  if (props.hideArt) return []
  if (usable(props.heroSrc)) return [{ kind: 'hero', src: props.heroSrc }]
  return showArt.value ? [{ kind: 'poster', src: props.artSrc }] : []
})
const keyartLayers = computed(() => (showArt.value ? [props.artSrc] : []))
// The logo stands in for the title text once it has loaded, the text staying the heading for
// assistive technology. A logo too dark to read on the stage's dark scrim keeps the text instead.
const showLogo = computed(() => props.mode !== 'loading' && usable(props.logoSrc))
const logoShown = computed(() => showLogo.value && loadedArt.has(props.logoSrc))
function logoLoaded(src, event) {
  if (logoReadable(event.target)) loadedArt.add(src)
  else failedArt.add(src)
}
// Alpha-weighted relative luminance of a small copy of the logo: at least 0.12 holds 3:1 against
// the near-black scrim, the contrast a large graphic needs.
function logoReadable(img) {
  try {
    const canvas = document.createElement('canvas')
    canvas.width = 32; canvas.height = 12
    const context = canvas.getContext('2d')
    if (!context) return true
    context.drawImage(img, 0, 0, canvas.width, canvas.height)
    const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data
    const linear = value => { const n = value / 255; return n <= 0.04045 ? n / 12.92 : ((n + 0.055) / 1.055) ** 2.4 }
    let weight = 0, light = 0
    for (let i = 0; i < pixels.length; i += 4) {
      const alpha = pixels[i + 3] / 255
      weight += alpha
      light += alpha * (0.2126 * linear(pixels[i]) + 0.7152 * linear(pixels[i + 1]) + 0.0722 * linear(pixels[i + 2]))
    }
    return weight > 0 && light / weight >= 0.12
  } catch {
    return true
  }
}
const step = computed(() => titleStep(props.title))
const metaItems = computed(() => props.meta.filter(Boolean))
</script>

<template>
  <section class="mc-stage" :class="[`is-${mode}`, { 'has-art': showArt }]" :aria-labelledby="titleId" :aria-busy="mode === 'loading' ? 'true' : undefined" data-mission-stage :data-stage-mode="mode">
    <div class="mc-stage-art" aria-hidden="true">
      <TransitionGroup name="mc-xfade">
        <img v-for="layer in backdropLayers" :key="layer.src" class="mc-stage-backdrop" :class="{ 'is-loaded': loadedArt.has(layer.src), 'is-hero': layer.kind === 'hero' }"
          :src="layer.src" alt="" :data-stage-hero="layer.kind === 'hero' ? '' : undefined" @load="loadedArt.add(layer.src)" @error="failedArt.add(layer.src)" />
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
          <img v-for="src in keyartLayers" :key="src" :class="{ 'is-loaded': loadedArt.has(src) }" :src="src" alt=""
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
        <h2 v-else :id="titleId" class="mc-stage-title" :class="`is-${step}`" :data-title-step="step">
          <img v-if="showLogo" v-show="logoShown" :key="logoSrc" class="mc-stage-logo" :src="logoSrc" alt="" data-stage-logo
            @load="logoLoaded(logoSrc, $event)" @error="failedArt.add(logoSrc)" />
          <span :class="{ 'sr-only': logoShown }">{{ title }}</span>
        </h2>
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
