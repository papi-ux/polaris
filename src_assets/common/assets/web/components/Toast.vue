<template>
  <Teleport to="body">
    <div class="fixed bottom-4 right-4 z-50 flex flex-col gap-2 pointer-events-none">
      <TransitionGroup name="toast">
        <div v-for="t in toasts" :key="t.id"
             class="glass rounded-xl shadow-lg max-w-sm pointer-events-auto overflow-hidden"
             :class="{
               'border-green-500/40': t.type === 'success',
               'border-red-500/40': t.type === 'error',
               'border-storm/40': t.type === 'info'
             }">
          <div class="px-4 py-3 flex items-start gap-3">
            <!-- Icon -->
            <div class="shrink-0 mt-0.5">
              <svg v-if="t.type === 'success'" class="w-5 h-5 text-green-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12l2 2 4-4m6 2a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
              <svg v-else-if="t.type === 'error'" class="w-5 h-5 text-red-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 8v4m0 4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
              <svg v-else class="w-5 h-5 text-ice" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
            </div>
            <!-- Content -->
            <div class="flex-1 min-w-0">
              <span class="text-silver text-sm">{{ t.message }}</span>
              <button v-if="t.action" class="ml-2 text-ice hover:text-ice/80 font-medium text-sm whitespace-nowrap" @click="t.action.handler">{{ t.action.label }}</button>
            </div>
          </div>
          <!-- Progress bar -->
          <div v-if="t.duration" class="h-0.5 bg-storm/20">
            <div class="h-full transition-all ease-linear"
                 :class="{
                   'bg-green-500/60': t.type === 'success',
                   'bg-red-500/60': t.type === 'error',
                   'bg-ice/40': t.type === 'info'
                 }"
                 :style="{ width: '100%', animation: `toast-progress ${t.duration}ms linear forwards` }">
            </div>
          </div>
        </div>
      </TransitionGroup>
    </div>
  </Teleport>
</template>

<script setup>
import { useToast } from '../composables/useToast'
const { toasts } = useToast()
</script>

<style>
@reference "../app.css";

.toast-enter-active {
  transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
}
.toast-leave-active {
  transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1);
}
.toast-enter-from {
  opacity: 0;
  transform: translateX(40px) scale(0.95);
}
.toast-leave-to {
  opacity: 0;
  transform: translateX(40px) scale(0.95);
}

@keyframes toast-progress {
  from { width: 100%; }
  to { width: 0%; }
}
</style>
