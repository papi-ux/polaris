<template>
  <div class="mx-auto flex min-h-screen max-w-6xl items-center justify-center px-4 py-8 sm:px-6">
    <div class="flex w-full max-w-5xl flex-col gap-6 lg:flex-row">
      <div class="glass rounded-2xl p-8 w-full shadow-2xl animate-[scale-in_0.3s_ease-out]">
        <header class="flex items-center gap-3 mb-6">
          <img src="/images/logo-polaris.svg" class="h-10" alt="Polaris">
          <div>
            <h1 class="text-2xl font-bold text-silver">{{ $t('recovery.title') }}</h1>
            <p class="text-storm text-sm">{{ $t('recovery.subtitle') }}</p>
          </div>
        </header>

        <div class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-4 rounded-lg mb-6">
          <p class="font-semibold mb-1">{{ $t('recovery.browser_reset_unavailable') }}</p>
          <p class="text-sm text-storm">{{ $t('recovery.browser_reset_reason') }}</p>
        </div>

        <div class="mb-6">
          <h2 class="text-lg font-semibold text-silver mb-3">{{ $t('recovery.steps_title') }}</h2>
          <ol class="space-y-2 list-decimal list-inside text-storm">
            <li>{{ $t('recovery.steps.open_terminal') }}</li>
            <li>{{ $t('recovery.steps.run_command') }}</li>
            <li>{{ $t('recovery.steps.return_login') }}</li>
          </ol>
        </div>

        <div class="space-y-4">
          <div
            v-for="variant in commandVariants"
            :key="variant.id"
            class="bg-void/60 border border-storm/60 rounded-xl p-4"
          >
            <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between mb-3">
              <div>
                <h3 class="text-silver font-semibold">{{ variant.title }}</h3>
                <p class="text-sm text-storm">{{ variant.description }}</p>
              </div>
              <button
                type="button"
                class="h-9 px-3 rounded-lg border border-storm/60 text-sm text-silver hover:border-ice hover:text-ice transition-colors"
                @click="copyCommand(variant.id, variant.command)"
              >
                {{ copiedCommandId === variant.id ? $t('recovery.copied') : $t('recovery.copy') }}
              </button>
            </div>
            <pre class="bg-black/30 rounded-lg p-4 overflow-x-auto text-sm text-ice"><code>{{ variant.command }}</code></pre>
          </div>
        </div>

        <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-4 rounded-lg mt-6">
          <p class="font-semibold mb-1">{{ $t('recovery.notes_title') }}</p>
          <ul class="space-y-1 text-sm text-storm list-disc list-inside">
            <li>{{ $t('recovery.notes.full_path') }}</li>
            <li>{{ $t('recovery.notes.source_build') }}</li>
            <li>{{ $t('recovery.notes.keep_safe') }}</li>
          </ul>
        </div>

        <div class="mt-6">
          <router-link
            to="/login"
            class="inline-flex items-center justify-center h-10 px-4 rounded-lg bg-ice text-void font-semibold hover:bg-ice/90 transition-all duration-200 no-underline"
          >
            {{ $t('recovery.back_to_login') }}
          </router-link>
        </div>
      </div>

      <div class="lg:w-80">
        <ResourceCard />
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import ResourceCard from '../ResourceCard.vue'

const { t } = useI18n()
const copiedCommandId = ref('')

const commandVariants = computed(() => [
  {
    id: 'installed',
    title: t('recovery.commands.installed_title'),
    description: t('recovery.commands.installed_desc'),
    command: 'polaris --creds <new-username> <new-password>',
  },
  {
    id: 'source',
    title: t('recovery.commands.source_title'),
    description: t('recovery.commands.source_desc'),
    command: './build/polaris --creds <new-username> <new-password>',
  },
  {
    id: 'appimage',
    title: t('recovery.commands.appimage_title'),
    description: t('recovery.commands.appimage_desc'),
    command: './polaris.AppImage --creds <new-username> <new-password>',
  },
  {
    id: 'flatpak',
    title: t('recovery.commands.flatpak_title'),
    description: t('recovery.commands.flatpak_desc'),
    command: 'flatpak run --command=polaris dev.polaris-stream.app.Polaris --creds <new-username> <new-password>',
  },
])

async function copyCommand(id, command) {
  try {
    await navigator.clipboard.writeText(command)
    copiedCommandId.value = id
    window.setTimeout(() => {
      if (copiedCommandId.value === id) {
        copiedCommandId.value = ''
      }
    }, 1500)
  } catch (e) {
    console.error('Failed to copy recovery command', e)
  }
}
</script>
