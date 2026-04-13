<template>
  <Teleport to="body">
    <div v-if="modelValue" class="fixed inset-0 z-50 flex items-start justify-center pt-[20vh]" @click.self="close">
      <div class="fixed inset-0 bg-void/80 backdrop-blur-sm"></div>
      <div class="relative bg-deep border border-storm rounded-xl shadow-2xl w-full max-w-lg overflow-hidden">
        <div class="flex items-center gap-3 px-4 py-3 border-b border-storm">
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
            @keydown.down.prevent="selectedIndex = Math.min(filteredActions.length - 1, selectedIndex + 1)"
          >
          <kbd class="text-xs text-storm bg-twilight px-2 py-0.5 rounded">ESC</kbd>
        </div>
        <ul v-if="filteredActions.length" class="max-h-80 overflow-auto py-2">
          <li v-for="(action, i) in filteredActions" :key="action.id"
              class="px-4 py-2 flex items-center gap-3 cursor-pointer transition-colors"
              :class="i === selectedIndex ? 'bg-twilight text-ice' : 'text-silver hover:bg-twilight/50'"
              @click="execute(action)">
            <span class="text-sm">{{ action.icon }}</span>
            <span>{{ action.label }}</span>
            <span class="ml-auto text-xs text-storm">{{ action.hint }}</span>
          </li>
        </ul>
        <div v-else class="px-4 py-8 text-center text-storm">
          {{ t('command_palette.empty') }}
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
  { id: 'dashboard', icon: '\u{1F4CA}', label: t('command_palette.actions.dashboard'), hint: '/', action: () => router.push('/') },
  { id: 'apps', icon: '\u{1F3AE}', label: t('command_palette.actions.apps'), hint: '/apps', action: () => router.push('/apps') },
  { id: 'config', icon: '\u2699\uFE0F', label: t('command_palette.actions.config'), hint: '/config', action: () => router.push('/config') },
  { id: 'pairing', icon: '\u{1F511}', label: t('command_palette.actions.pairing'), hint: '/pin', action: () => router.push('/pin') },
  { id: 'logs', icon: '\u{1F4CB}', label: t('command_palette.actions.logs'), hint: '/troubleshooting#logs', action: () => router.push('/troubleshooting') },
  { id: 'audio', icon: '\u{1F50A}', label: t('command_palette.actions.audio'), hint: '/config#audio_sink', action: () => router.push('/config#audio_sink') },
  { id: 'display', icon: '\u{1F5A5}\uFE0F', label: t('command_palette.actions.display'), hint: '/config#dd_configuration_option', action: () => router.push('/config#dd_configuration_option') },
  { id: 'ai', icon: '\u2728', label: t('command_palette.actions.ai'), hint: '/config#ai_enabled', action: () => router.push('/config#ai_enabled') },
  { id: 'network', icon: '\u{1F310}', label: t('command_palette.actions.network'), hint: '/config#trusted_subnets', action: () => router.push('/config#trusted_subnets') },
  { id: 'restart', icon: '\u{1F504}', label: t('command_palette.actions.restart'), hint: 'POST /api/restart', action: async () => {
    await fetch('./api/restart', { method: 'POST' })
    toast(t('command_palette.restart_sent'), 'info')
  }},
  { id: 'quit', icon: '\u23F9\uFE0F', label: t('command_palette.actions.quit'), hint: 'POST /api/quit', action: async () => {
    await fetch('./api/quit', { method: 'POST' })
    toast(t('command_palette.quit_sent'), 'info')
  }},
])

const filteredActions = computed(() => {
  if (!query.value) return actions.value
  const q = query.value.toLowerCase()
  return actions.value.filter(a =>
    a.label.toLowerCase().includes(q) ||
    a.hint.toLowerCase().includes(q)
  )
})

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
