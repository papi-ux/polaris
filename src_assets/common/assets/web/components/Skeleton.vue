<template>
  <div>
    <div v-if="type === 'card'" class="skeleton-shimmer rounded-xl border border-storm/50 p-4 h-24"></div>
    <div v-else-if="type === 'text'" class="skeleton-shimmer rounded h-4 w-3/4"></div>
    <div v-else-if="type === 'chart'" class="skeleton-shimmer rounded-xl border border-storm/50 p-4 h-32"></div>
    <slot v-else>
      <div class="skeleton-shimmer rounded h-4 w-full"></div>
    </slot>
  </div>
</template>

<script setup>
defineProps({
  type: {
    type: String,
    default: 'text'
  }
})
</script>
