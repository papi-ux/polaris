<script setup>
import { computed, onMounted, onUnmounted, reactive, ref, watch } from 'vue'
import { titleStep } from '../mission-control.js'
import { onThemeTokensChange, readThemeTokens } from '../theme-bridge.js'

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
for (const key of ['artSrc', 'heroSrc']) watch(() => props[key], src => { failedArt.delete(src) })
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
// The logo stands in for the title text once it has loaded and passed the screen below; the text
// stays the heading for assistive technology. Until then, and whenever the logo fails the screen
// or the screen cannot run, the text title stays visible.
//
// A verdict belongs to the one logo element on screen. A new logo or a new theme (a new scrim)
// mounts a fresh element, so no verdict is reused, and an event from an element that is no longer
// on screen settles nothing. Logos keep their own state, apart from the cover and background.
const logoMount = ref(0)
const logoVerdict = ref(null)
const missingLogos = reactive(new Set())
watch(() => props.logoSrc, src => { missingLogos.delete(src); logoMount.value++ })
let stopThemeWatch = null
onMounted(() => { stopThemeWatch = onThemeTokensChange(() => { logoMount.value++ }) })
onUnmounted(() => stopThemeWatch?.())
const currentVerdict = computed(() => {
  const verdict = logoVerdict.value
  return verdict && verdict.mount === logoMount.value && verdict.src === props.logoSrc ? verdict.pass : null
})
const showLogo = computed(() => props.mode !== 'loading' && Boolean(props.logoSrc) &&
  !missingLogos.has(props.logoSrc) && currentVerdict.value !== false)
const logoShown = computed(() => showLogo.value && currentVerdict.value === true)
const isCurrentLogo = img => img?.getAttribute('src') === props.logoSrc && Number(img.dataset.logoMount) === logoMount.value
function logoLoaded(event) {
  if (!isCurrentLogo(event.target)) return
  logoVerdict.value = { mount: logoMount.value, src: props.logoSrc, pass: logoReadable(event.target, readThemeTokens(['void']).void) }
}
function logoFailed(event) {
  if (isCurrentLogo(event.target)) missingLogos.add(props.logoSrc)
}

const channel = value => { const n = value / 255; return n <= 0.04045 ? n / 12.92 : ((n + 0.055) / 1.055) ** 2.4 }
const luminance = (r, g, b) => 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)
// The brightest a background can be on the stage: white at its 0.6 brightness.
const BRIGHTEST_ART = 0.6 * 255
// The most of that background the scrim lets through anywhere: its thinnest edge is 15% scrim.
const ART_THROUGH = 0.85

// A screen for faint, dark and mid-tone logos, not a contrast guarantee: real artwork varies. Each
// solid-ink pixel (at least half opaque) of a small copy of the logo is composed, as the browser
// draws it, over the darkest backdrop the logo can sit on (the theme's scrim) and over the
// brightest (the scrim letting the brightest background through). The logo passes when ink covers
// at least a tenth of the copy and at least 80% of the ink reaches 3:1 against both backdrops. No
// context, unreadable pixels or an unknown scrim colour fail the screen.
function logoReadable(img, scrimColor) {
  // The built stylesheet shortens hex where it can (OLED's #000000 is #000); anything else is unknown.
  const scrim = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(String(scrimColor || '').trim())
  if (!scrim) return false
  const hex = scrim[1].length === 3 ? scrim[1].replace(/./g, digit => digit + digit) : scrim[1]
  const darkest = [0, 2, 4].map(at => parseInt(hex.slice(at, at + 2), 16))
  const brightest = darkest.map(value => value * (1 - ART_THROUGH) + BRIGHTEST_ART * ART_THROUGH)
  const backdrops = [darkest, brightest].map(rgb => ({ rgb, light: luminance(...rgb) }))
  try {
    const canvas = document.createElement('canvas')
    canvas.width = 96; canvas.height = 32
    const context = canvas.getContext('2d')
    if (!context) return false
    context.drawImage(img, 0, 0, canvas.width, canvas.height)
    const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data
    let ink = 0, clear = 0
    for (let i = 0; i < pixels.length; i += 4) {
      const alpha = pixels[i + 3] / 255
      if (alpha < 0.5) continue
      ink++
      const readable = backdrops.every(({ rgb, light }) => {
        const shown = luminance(...rgb.map((under, c) => pixels[i + c] * alpha + under * (1 - alpha)))
        return (Math.max(shown, light) + 0.05) / (Math.min(shown, light) + 0.05) >= 3
      })
      if (readable) clear++
    }
    return ink >= (pixels.length / 4) * 0.1 && clear >= ink * 0.8
  } catch {
    return false
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
          <img v-if="showLogo" v-show="logoShown" :key="`${logoSrc}#${logoMount}`" class="mc-stage-logo" :src="logoSrc" alt="" data-stage-logo
            :data-logo-mount="logoMount" @load="logoLoaded" @error="logoFailed" />
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
