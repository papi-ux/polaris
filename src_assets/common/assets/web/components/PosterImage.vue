<script setup>
import { computed, ref, watch } from 'vue'

const props = defineProps({
  name: { type: String, default: '' },
  src: { type: String, default: '' },
})
const failed = ref(false)
const loaded = ref(false)
watch(() => props.src, () => { failed.value = false; loaded.value = false })
// A poster fades in once it has decoded, so the fade never runs over a half-drawn image. A decode
// that settles after the artwork changed reveals nothing; one that fails still shows the image,
// which the browser then decodes as it paints.
function imageLoaded(event) {
  const src = props.src
  const show = () => { if (props.src === src) loaded.value = true }
  if (typeof event.target.decode === 'function') event.target.decode().then(show, show)
  else show()
}
const initials = computed(() => {
  const words = props.name.trim().split(/\s+/u).filter(Boolean)
  return words.length ? words.slice(0, 2).map(word => Array.from(word)[0]).join('').toLocaleUpperCase() : '?'
})
// Vary the mix within the current theme's palette rather than assigning unrelated colors.
const nameHash = computed(() => {
  let hash = 0
  for (const letter of props.name) hash = (hash * 31 + letter.codePointAt(0)) >>> 0
  return hash
})
</script>

<template>
  <div class="poster-image" :style="{ '--poster-accent-mix': `${30 + nameHash % 25}%`, '--poster-angle': `${95 + nameHash % 160}deg` }" data-poster-image>
    <div class="poster-fallback" :class="{ 'is-covered': src && !failed }" role="img" :aria-label="`${name || 'Untitled app'} artwork unavailable`" :aria-hidden="src && !failed ? 'true' : undefined" data-poster-fallback>
      <span aria-hidden="true">{{ initials }}</span>
    </div>
    <Transition name="poster-reveal">
      <div v-if="src && !failed && !loaded" class="poster-loading skeleton-shimmer" aria-hidden="true"></div>
    </Transition>
    <img v-if="src && !failed" :class="{ 'is-loaded': loaded }" :src="src" :alt="`${name || 'Untitled app'} artwork`" loading="lazy"
      @load="imageLoaded" @error="failed = true" />
  </div>
</template>

<style scoped>
.poster-image {
  position: relative;
  flex-shrink: 0;
  aspect-ratio: 2 / 3;
  overflow: hidden;
  border-radius: 0.5rem;
  background: var(--color-deep);
}
.poster-image img, .poster-fallback, .poster-loading {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
}
.poster-image img { object-fit: cover; }
.poster-fallback {
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(var(--poster-angle),
    color-mix(in srgb, var(--color-accent) var(--poster-accent-mix), var(--color-void)),
    color-mix(in srgb, var(--color-ice) 12%, var(--color-void)));
  color: var(--color-ice);
  font-size: clamp(1rem, 4vw, 1.5rem);
  font-weight: 700;
  letter-spacing: 0.04em;
}
/* The decoded poster fades in as the shimmer fades out. The initials stay hidden beneath while
   artwork is expected, so they never show through the crossfade. */
.poster-image img {
  opacity: 0;
  transition: opacity 0.3s ease-out;
}
.poster-image img.is-loaded { opacity: 1; }
.poster-reveal-leave-active { transition: opacity 0.3s ease-out; }
.poster-reveal-leave-to { opacity: 0; }
.poster-fallback.is-covered { visibility: hidden; }
@media (prefers-reduced-motion: reduce) {
  .poster-image img, .poster-reveal-leave-active { transition: none; }
}
</style>

