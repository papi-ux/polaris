<template>
  <Teleport to="body">
    <div
      v-if="modelValue"
      class="fixed inset-0 z-50 flex items-start justify-center px-4 pt-[10vh] sm:px-6"
      @click.self="close"
    >
      <div class="fixed inset-0 bg-void/80 backdrop-blur-sm"></div>
      <div
        class="relative w-full max-w-2xl overflow-hidden rounded-2xl border border-storm/40 bg-deep/95 shadow-2xl"
        role="dialog"
        aria-modal="true"
        aria-label="Command palette"
      >
        <div class="border-b border-storm/20 px-4 py-4 sm:px-5">
          <div class="mb-3 flex items-center justify-between gap-3">
            <div>
              <div class="text-[10px] font-semibold uppercase tracking-[0.22em] text-storm">Command Palette</div>
              <div class="mt-1 text-sm text-storm">Jump to a page or trigger a host action.</div>
            </div>
            <kbd class="rounded-lg border border-storm/25 bg-void/60 px-2 py-1 text-[11px] text-storm">ESC</kbd>
          </div>
          <div class="flex items-center gap-3 rounded-xl border border-storm/25 bg-void/55 px-3 py-3">
            <svg class="w-5 h-5 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z"/>
          </svg>
          <input
            ref="searchInput"
            v-model="query"
            type="text"
            :placeholder="t('command_palette.placeholder')"
            class="flex-1 bg-transparent text-silver outline-none placeholder-storm"
            @keydown.escape="close"
            @keydown.enter="executeSelected"
            @keydown.up.prevent="selectedIndex = Math.max(0, selectedIndex - 1)"
            @keydown.down.prevent="selectedIndex = Math.max(0, Math.min(filteredActions.length - 1, selectedIndex + 1))"
          >
          <kbd class="rounded-lg border border-storm/20 bg-deep/70 px-2 py-1 text-[11px] text-storm">↵</kbd>
          </div>
        </div>
        <div v-if="filteredActions.length" class="max-h-[26rem] overflow-auto px-3 py-3 sm:px-4">
          <div
            v-for="group in groupedActions"
            :key="group.label"
            class="mb-4 last:mb-0"
          >
            <div class="px-2 pb-2 text-[10px] font-semibold uppercase tracking-[0.22em] text-storm/85">
              {{ group.label }}
            </div>
            <ul role="listbox" class="space-y-1">
              <li
                v-for="action in group.actions"
                :key="action.id"
                :aria-selected="selectedAction?.id === action.id"
                class="cursor-pointer rounded-xl border px-3 py-3 transition-[background-color,border-color,color,box-shadow] duration-150"
                :class="selectedAction?.id === action.id
                  ? 'border-ice/30 bg-twilight/65 text-ice shadow-[0_12px_30px_rgba(0,0,0,0.2)]'
                  : 'border-transparent text-silver hover:border-storm/25 hover:bg-twilight/40'"
                role="option"
                @click="execute(action)"
              >
                <div class="flex items-center gap-3">
                  <span class="inline-flex h-9 w-9 items-center justify-center rounded-xl border border-storm/20 bg-void/55 text-sm">
                    {{ action.icon }}
                  </span>
                  <div class="min-w-0 flex-1">
                    <div class="text-sm font-medium">{{ action.label }}</div>
                    <div class="mt-1 text-xs text-storm">{{ action.description }}</div>
                  </div>
                  <div class="text-right">
                    <div class="text-[11px] text-storm">{{ action.hint }}</div>
                  </div>
                </div>
              </li>
            </ul>
          </div>
        </div>
        <div v-else class="px-4 py-8 text-center text-storm">
          {{ t('command_palette.empty') }}
        </div>
        <div class="border-t border-storm/20 px-4 py-3 text-[11px] text-storm sm:px-5">
          Use <span class="text-silver">↑</span> and <span class="text-silver">↓</span> to move, <span class="text-silver">Enter</span> to run, and <span class="text-silver">Esc</span> to close.
        </div>
      </div>
    </div>
  </Teleport>
</template>

<script setup>
import { ref, computed, watch, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useToast } from './composables/useToast'

const props = defineProps({
  modelValue: {
    type: Boolean,
    default: false
  }
})

const emit = defineEmits(['update:modelValue'])

const router = useRouter()
const { t } = useI18n()
const { toast } = useToast()
const query = ref('')
const selectedIndex = ref(0)
const searchInput = ref(null)

const actions = computed(() => [
  { id: 'dashboard', group: 'Navigation', icon: '\u{1F4CA}', label: t('command_palette.actions.dashboard'), description: 'Open the live control surface and session overview.', hint: '/', action: () => router.push('/') },
  { id: 'apps', group: 'Navigation', icon: '\u{1F3AE}', label: t('command_palette.actions.apps'), description: 'Browse, edit, and import streamable applications.', hint: '/apps', action: () => router.push('/apps') },
  { id: 'config', group: 'Navigation', icon: '\u2699\uFE0F', label: t('command_palette.actions.config'), description: 'Adjust host, network, encoder, and display settings.', hint: '/config', action: () => router.push('/config') },
  { id: 'pairing', group: 'Navigation', icon: '\u{1F511}', label: t('command_palette.actions.pairing'), description: 'Manage pairing, trusted subnets, and connected devices.', hint: '/pin', action: () => router.push('/pin') },
  { id: 'logs', group: 'Navigation', icon: '\u{1F4CB}', label: t('command_palette.actions.logs'), description: 'Open troubleshooting tools and runtime diagnostics.', hint: '/troubleshooting', action: () => router.push('/troubleshooting') },
  { id: 'audio', group: 'Settings', icon: '\u{1F50A}', label: t('command_palette.actions.audio'), description: 'Jump straight to audio sink and output tuning.', hint: 'Audio', action: () => router.push('/config#audio_sink') },
  { id: 'display', group: 'Settings', icon: '\u{1F5A5}\uFE0F', label: t('command_palette.actions.display'), description: 'Jump to display device configuration and mode control.', hint: 'Display', action: () => router.push('/config#dd_configuration_option') },
  { id: 'ai', group: 'Settings', icon: '\u2728', label: t('command_palette.actions.ai'), description: 'Review AI optimization and provider settings.', hint: 'AI', action: () => router.push('/config#ai_enabled') },
  { id: 'network', group: 'Settings', icon: '\u{1F310}', label: t('command_palette.actions.network'), description: 'Review pairing access, trusted subnets, and exposure.', hint: 'Network', action: () => router.push('/config#trusted_subnets') },
  { id: 'restart', group: 'Host Controls', icon: '\u{1F504}', label: t('command_palette.actions.restart'), description: 'Restart Polaris to apply host-level changes.', hint: 'POST /api/restart', action: async () => {
    await fetch('./api/restart', { method: 'POST' })
    toast(t('command_palette.restart_sent'), 'info')
  }},
  { id: 'quit', group: 'Host Controls', icon: '\u23F9\uFE0F', label: t('command_palette.actions.quit'), description: 'Shut down the Polaris host service from the web UI.', hint: 'POST /api/quit', action: async () => {
    await fetch('./api/quit', { method: 'POST' })
    toast(t('command_palette.quit_sent'), 'info')
  }},
])

const filteredActions = computed(() => {
  if (!query.value) return actions.value
  const q = query.value.toLowerCase()
  return actions.value.filter(a =>
    a.label.toLowerCase().includes(q) ||
    a.hint.toLowerCase().includes(q) ||
    a.description.toLowerCase().includes(q) ||
    a.group.toLowerCase().includes(q)
  )
})

const groupedActions = computed(() => {
  const groups = new Map()
  for (const action of filteredActions.value) {
    if (!groups.has(action.group)) groups.set(action.group, [])
    groups.get(action.group).push(action)
  }
  return Array.from(groups.entries()).map(([label, actions]) => ({ label, actions }))
})

const selectedAction = computed(() => filteredActions.value[selectedIndex.value] || null)

watch(() => props.modelValue, (val) => {
  if (val) {
    query.value = ''
    selectedIndex.value = 0
    nextTick(() => {
      searchInput.value?.focus()
    })
  }
})

watch(query, () => {
  selectedIndex.value = 0
})

function close() {
  emit('update:modelValue', false)
}

function execute(action) {
  close()
  action.action()
}

function executeSelected() {
  const action = filteredActions.value[selectedIndex.value]
  if (action) {
    execute(action)
  }
}
</script>
