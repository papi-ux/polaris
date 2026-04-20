<template>
  <Teleport to="body">
    <div
      v-if="modelValue"
      class="fixed inset-0 z-50 flex items-start justify-center px-4 pt-[8vh] sm:px-6"
      @click.self="close"
    >
      <div class="fixed inset-0 bg-void/80 backdrop-blur-sm"></div>
      <div
        ref="dialogRef"
        class="relative w-full max-w-2xl overflow-hidden rounded-2xl border border-storm/40 bg-deep/95 shadow-2xl"
        role="dialog"
        aria-modal="true"
        aria-label="Command palette"
        @keydown.tab.prevent="trapTabKey"
      >
        <div class="border-b border-storm/20 px-4 py-4 sm:px-5">
          <div class="mb-3 flex items-center justify-between gap-3">
            <div>
              <div class="text-[10px] font-semibold uppercase tracking-[0.22em] text-storm">Command Palette</div>
              <div class="mt-1 text-sm text-storm">Jump pages or run a host action.</div>
            </div>
            <div class="flex items-center gap-2">
              <span class="rounded-full border border-storm/20 bg-void/50 px-2.5 py-1 text-[11px] text-storm">{{ resultsLabel }}</span>
              <kbd class="rounded-lg border border-storm/25 bg-void/60 px-2 py-1 text-[11px] text-storm">ESC</kbd>
            </div>
          </div>
          <div class="flex items-center gap-3 rounded-xl border border-storm/25 bg-void/55 px-3 py-2.5 focus-within:border-ice/35 focus-within:ring-1 focus-within:ring-ice/20">
            <svg class="w-5 h-5 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z"/>
          </svg>
          <input
            ref="searchInput"
            v-model="query"
            type="text"
            role="combobox"
            aria-autocomplete="list"
            :aria-controls="listboxId"
            :aria-expanded="filteredActions.length > 0 ? 'true' : 'false'"
            :aria-activedescendant="selectedAction ? `command-palette-option-${selectedAction.id}` : undefined"
            aria-label="Search commands"
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
        <div v-if="filteredActions.length" :id="listboxId" role="listbox" aria-label="Command results" class="max-h-[26rem] overflow-auto px-3 py-3 sm:px-4">
          <div
            v-for="group in groupedActions"
            :key="group.label"
            class="mb-4 last:mb-0"
          >
            <div class="flex items-center justify-between gap-3 px-2 pb-2">
              <div class="text-[10px] font-semibold uppercase tracking-[0.22em] text-storm/85">
                {{ group.label }}
              </div>
              <div class="text-[10px] uppercase tracking-[0.18em] text-storm/65">
                {{ group.actions.length }}
              </div>
            </div>
            <ul class="space-y-1">
              <li
                v-for="action in group.actions"
                :key="action.id"
                :id="`command-palette-option-${action.id}`"
                :aria-selected="selectedAction?.id === action.id"
                class="cursor-pointer rounded-xl border px-3 py-2.5 transition-[background-color,border-color,color,box-shadow] duration-150"
                :class="selectedAction?.id === action.id
                  ? 'border-ice/30 bg-twilight/65 text-ice shadow-[0_12px_30px_rgba(0,0,0,0.2)]'
                  : 'border-transparent text-silver hover:border-storm/25 hover:bg-twilight/40'"
                role="option"
                @mouseenter="selectedIndex = action.globalIndex"
                @click="execute(action)"
              >
                <div class="flex items-center gap-3">
                  <span
                    class="inline-flex h-8 w-8 items-center justify-center rounded-lg border text-sm"
                    :class="action.tone === 'danger'
                      ? 'border-red-500/20 bg-red-500/8 text-red-200'
                      : 'border-storm/20 bg-void/55'"
                  >
                    {{ action.icon }}
                  </span>
                  <div class="min-w-0 flex-1">
                    <div class="text-sm font-medium">{{ action.label }}</div>
                    <div class="mt-0.5 text-xs text-storm">{{ action.description }}</div>
                  </div>
                  <div class="text-right">
                    <div
                      class="rounded-full border px-2 py-1 text-[10px] uppercase tracking-[0.16em]"
                      :class="action.tone === 'danger'
                        ? 'border-red-500/20 bg-red-500/8 text-red-200'
                        : 'border-storm/20 bg-void/50 text-storm'"
                    >
                      {{ action.hint }}
                    </div>
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
          <span class="text-silver">↑</span>/<span class="text-silver">↓</span> move, <span class="text-silver">Enter</span> runs, <span class="text-silver">Esc</span> closes.
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
const dialogRef = ref(null)
const lastFocusedElement = ref(null)
const listboxId = 'command-palette-results'

const actions = computed(() => [
  { id: 'dashboard', group: 'Navigation', icon: '\u{1F4CA}', label: t('command_palette.actions.dashboard'), description: 'Mission Control and live sessions.', hint: '/', tone: 'neutral', action: () => router.push('/') },
  { id: 'apps', group: 'Navigation', icon: '\u{1F3AE}', label: t('command_palette.actions.apps'), description: 'Library and launch targets.', hint: '/apps', tone: 'neutral', action: () => router.push('/apps') },
  { id: 'config', group: 'Navigation', icon: '\u2699\uFE0F', label: t('command_palette.actions.config'), description: 'Host, display, and network settings.', hint: '/config', tone: 'neutral', action: () => router.push('/config') },
  { id: 'pairing', group: 'Navigation', icon: '\u{1F511}', label: t('command_palette.actions.pairing'), description: 'Trust, pairing, and device access.', hint: '/pin', tone: 'neutral', action: () => router.push('/pin') },
  { id: 'logs', group: 'Navigation', icon: '\u{1F4CB}', label: t('command_palette.actions.logs'), description: 'Recovery tools and logs.', hint: '/troubleshooting', tone: 'neutral', action: () => router.push('/troubleshooting') },
  { id: 'audio', group: 'Settings', icon: '\u{1F50A}', label: t('command_palette.actions.audio'), description: 'Audio sink and output routing.', hint: 'Audio', tone: 'neutral', action: () => router.push('/config#audio_sink') },
  { id: 'display', group: 'Settings', icon: '\u{1F5A5}\uFE0F', label: t('command_palette.actions.display'), description: 'Display path and mode control.', hint: 'Display', tone: 'neutral', action: () => router.push('/config#dd_configuration_option') },
  { id: 'ai', group: 'Settings', icon: '\u2728', label: t('command_palette.actions.ai'), description: 'AI provider and optimizer state.', hint: 'AI', tone: 'neutral', action: () => router.push('/config#ai_enabled') },
  { id: 'network', group: 'Settings', icon: '\u{1F310}', label: t('command_palette.actions.network'), description: 'Ports, trust, and exposure.', hint: 'Network', tone: 'neutral', action: () => router.push('/config#trusted_subnets') },
  { id: 'restart', group: 'Host Controls', icon: '\u{1F504}', label: t('command_palette.actions.restart'), description: 'Restart Polaris now.', hint: 'Restart', tone: 'danger', action: async () => {
    await fetch('./api/restart', { method: 'POST' })
    toast(t('command_palette.restart_sent'), 'info')
  }},
  { id: 'quit', group: 'Host Controls', icon: '\u23F9\uFE0F', label: t('command_palette.actions.quit'), description: 'Shut down Polaris.', hint: 'Quit', tone: 'danger', action: async () => {
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
  let globalIndex = 0
  return Array.from(groups.entries()).map(([label, actions]) => {
    const indexedActions = actions.map((action) => ({
      ...action,
      globalIndex: globalIndex++,
    }))
    return { label, actions: indexedActions }
  })
})

const selectedAction = computed(() => filteredActions.value[selectedIndex.value] || null)
const resultsLabel = computed(() => `${filteredActions.value.length} ${filteredActions.value.length === 1 ? 'result' : 'results'}`)

watch(() => props.modelValue, (val) => {
  if (val) {
    lastFocusedElement.value = document.activeElement
    query.value = ''
    selectedIndex.value = 0
    nextTick(() => {
      searchInput.value?.focus()
    })
    return
  }

  nextTick(() => {
    lastFocusedElement.value?.focus?.()
  })
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

function trapTabKey(event) {
  const root = dialogRef.value
  if (!root) return

  const focusable = Array.from(root.querySelectorAll(
    'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'
  )).filter((node) => !node.hasAttribute('hidden') && node.offsetParent !== null)

  if (focusable.length === 0) return

  const first = focusable[0]
  const last = focusable[focusable.length - 1]
  const active = document.activeElement

  if (event.shiftKey && active === first) {
    last.focus()
    return
  }

  if (!event.shiftKey && active === last) {
    first.focus()
    return
  }

  const currentIndex = focusable.indexOf(active)
  if (currentIndex === -1) {
    first.focus()
    return
  }

  const nextIndex = event.shiftKey ? currentIndex - 1 : currentIndex + 1
  focusable[(nextIndex + focusable.length) % focusable.length].focus()
}
</script>
