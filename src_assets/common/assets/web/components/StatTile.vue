<script setup>
const props = defineProps({
  label: {
    type: String,
    default: '',
  },
  value: {
    type: String,
    default: '',
  },
  note: {
    type: String,
    default: '',
  },
  tileClass: {
    type: [String, Array, Object],
    default: '',
  },
})
</script>

<template>
  <div class="stat-tile-compact" :class="props.tileClass" data-readonly>
    <slot>
      <slot name="label">
        <div v-if="props.label" class="stat-kicker">{{ props.label }}</div>
      </slot>
      <slot name="value">
        <div v-if="props.value" class="mt-1 text-sm font-medium text-silver">{{ props.value }}</div>
      </slot>
      <slot name="note">
        <div v-if="props.note" class="mt-1 text-[11px] text-storm">{{ props.note }}</div>
      </slot>
    </slot>
  </div>
</template>
