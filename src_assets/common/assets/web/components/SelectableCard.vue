<script setup>
const props = defineProps({
  selected: {
    type: Boolean,
    default: false,
  },
  disabled: {
    type: Boolean,
    default: false,
  },
  cardClass: {
    type: [String, Array, Object],
    default: '',
  },
})

const emit = defineEmits(['click'])
</script>

<template>
  <button
    type="button"
    class="selectable-card focus-ring"
    :class="props.cardClass"
    :disabled="props.disabled"
    :aria-pressed="props.selected"
    @click="emit('click')"
  >
    <slot></slot>
  </button>
</template>
