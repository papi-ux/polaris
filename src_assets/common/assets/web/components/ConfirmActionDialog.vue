<template>
  <Teleport to="body">
    <div
      v-if="modelValue"
      class="fixed inset-0 z-[80] flex items-center justify-center bg-void/80 px-4 py-6 backdrop-blur-sm"
      @click.self="requestCancel"
    >
      <section
        ref="dialogEl"
        role="dialog"
        aria-modal="true"
        :aria-labelledby="titleId"
        :aria-describedby="descriptionId"
        tabindex="-1"
        class="w-full max-w-lg rounded-2xl border border-storm/30 bg-deep p-5 text-silver shadow-2xl"
        @keydown="handleKeydown"
      >
        <div class="flex items-start justify-between gap-4">
          <div>
            <div class="text-[11px] font-semibold uppercase tracking-[0.2em] text-amber-200">{{ eyebrow }}</div>
            <h2 :id="titleId" class="mt-2 text-xl font-semibold text-silver">{{ title }}</h2>
          </div>
          <span class="rounded-full border border-amber-300/30 bg-amber-300/10 px-2.5 py-1 text-[11px] font-semibold uppercase tracking-[0.16em] text-amber-100">
            {{ impactLabel }}
          </span>
        </div>

        <p :id="descriptionId" class="mt-3 text-sm leading-relaxed text-storm">{{ message }}</p>

        <ul v-if="impactItems.length" class="mt-4 space-y-2 rounded-xl border border-storm/20 bg-void/35 p-3">
          <li v-for="item in impactItems" :key="item" class="flex gap-2 text-sm text-silver">
            <span class="mt-1 h-1.5 w-1.5 shrink-0 rounded-full bg-amber-200"></span>
            <span>{{ item }}</span>
          </li>
        </ul>

        <div v-if="error" class="mt-4 rounded-lg border-l-4 border-red-500 bg-red-500/10 px-3 py-2 text-sm text-red-100">
          {{ error }}
        </div>
        <div v-if="pending" class="mt-4 text-sm text-storm" aria-live="polite">
          {{ pendingLabel }}
        </div>

        <div class="mt-5 flex flex-col-reverse gap-2 sm:flex-row sm:justify-end">
          <button
            type="button"
            data-confirm-cancel
            class="focus-ring rounded-lg border border-storm/30 px-4 py-2 text-sm font-medium text-silver transition-colors hover:border-ice/40 hover:text-ice disabled:cursor-not-allowed disabled:opacity-60"
            :disabled="pending"
            @click="requestCancel"
          >
            {{ cancelLabel }}
          </button>
          <button
            type="button"
            data-confirm-confirm
            class="focus-ring rounded-lg border border-red-400/35 bg-red-500/15 px-4 py-2 text-sm font-semibold text-red-100 transition-colors hover:border-red-300/60 hover:bg-red-500/25 disabled:cursor-wait disabled:opacity-70"
            :disabled="pending"
            @click="$emit('confirm')"
          >
            {{ pending ? pendingLabel : confirmLabel }}
          </button>
        </div>
      </section>
    </div>
  </Teleport>
</template>

<script setup>
import { nextTick, onMounted, ref, watch } from 'vue'

const props = defineProps({
  modelValue: { type: Boolean, default: false },
  title: { type: String, required: true },
  message: { type: String, required: true },
  impactItems: { type: Array, default: () => [] },
  confirmLabel: { type: String, default: 'Confirm' },
  cancelLabel: { type: String, default: 'Cancel' },
  pendingLabel: { type: String, default: 'Working…' },
  pending: { type: Boolean, default: false },
  error: { type: String, default: '' },
  eyebrow: { type: String, default: 'Confirm action' },
  impactLabel: { type: String, default: 'Host impact' },
})

const emit = defineEmits(['update:modelValue', 'confirm', 'cancel'])
const dialogEl = ref(null)
const titleId = `confirm-action-title-${Math.random().toString(36).slice(2)}`
const descriptionId = `confirm-action-desc-${Math.random().toString(36).slice(2)}`
let previousFocus = null

function focusCancel() {
  dialogEl.value?.querySelector('[data-confirm-cancel]')?.focus()
}

function restoreFocus() {
  if (previousFocus instanceof HTMLElement && document.contains(previousFocus)) {
    previousFocus.focus()
  }
  previousFocus = null
}

function requestCancel() {
  if (props.pending) return
  emit('cancel')
  emit('update:modelValue', false)
  nextTick(restoreFocus)
}

function handleKeydown(event) {
  if (event.key === 'Escape') {
    event.preventDefault()
    requestCancel()
  }
}

watch(() => props.modelValue, async (open) => {
  if (open) {
    previousFocus = document.activeElement
    await nextTick()
    focusCancel()
  } else {
    await nextTick()
    restoreFocus()
  }
})

onMounted(async () => {
  if (!props.modelValue) return
  previousFocus = document.activeElement
  await nextTick()
  focusCancel()
})
</script>
