<script setup>
import { watch } from 'vue'
import Checkbox from '../Checkbox.vue'

// A host admin's stored recording approval for one paired device. It is not a permission bit and
// no preset touches it; no client can use it yet.
const approved = defineModel('approved', { type: Boolean, required: true })
const props = defineProps({
  // Distinguishes the field when several device editors exist.
  idSuffix: { type: String, required: true },
  // A temporary (guest) device cannot hold an approval.
  guest: { type: Boolean, default: false },
})

watch(
  () => props.guest,
  (guest) => {
    if (guest && approved.value) approved.value = false
  },
  { immediate: true },
)
</script>

<template>
  <div class="rounded-xl border border-storm/20 bg-deep/40 p-4" data-recording-approval>
    <Checkbox
      :id="`recording-authorized-${props.idSuffix}`"
      label="pin.recording_authorized"
      desc="pin.recording_authorized_desc"
      v-model="approved"
      :disabled="props.guest"
      default="false"
    />
    <p v-if="props.guest" class="mt-2 text-xs text-storm" data-recording-approval-guest>
      {{ $t('pin.recording_authorized_guest') }}
    </p>
  </div>
</template>
