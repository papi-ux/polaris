<script setup>
import { computed } from 'vue'
import { statusTone } from '../status-tones.js'

const props = defineProps({
  status: {
    type: String,
    required: true,
  },
  label: {
    type: String,
    default: '',
  },
})

const tone = computed(() => statusTone(props.status))
</script>

<template>
  <span class="meta-pill" :class="tone.badge">
    <slot>{{ props.label || tone.label }}</slot>
  </span>
</template>
