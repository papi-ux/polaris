<template>
  <div>
    <p class="mb-4 text-sm leading-relaxed text-storm">{{ $t('welcome.artwork_intro') }}</p>
    <div class="space-y-3">
      <a
        href="https://www.steamgriddb.com/profile/preferences/api"
        target="_blank"
        rel="noopener"
        class="inline-flex min-h-10 max-w-full items-center justify-center rounded-xl border border-storm/25 bg-deep/50 px-4 py-2 text-center text-sm font-medium text-silver transition-[background-color,border-color] duration-200 hover:border-ice/40 hover:bg-deep/70"
      >
        {{ $t('welcome.artwork_get_key') }}
      </a>

      <div v-if="hasStoredKey" class="surface-subtle flex flex-wrap items-center justify-between gap-3 p-4">
        <div class="text-sm text-silver">{{ $t('welcome.artwork_stored') }}</div>
        <button
          type="button"
          class="rounded-full border border-storm/25 px-3 py-1.5 text-xs font-medium text-silver transition-colors hover:border-ice/40 hover:text-ice disabled:opacity-60"
          :disabled="busy !== ''"
          @click="checkStoredKey"
        >
          {{ $t('welcome.artwork_check_stored') }}
        </button>
      </div>

      <div class="surface-subtle p-4">
        <label for="welcomeSteamGridDbKey" class="mb-1 block text-sm font-medium text-storm">{{ $t('welcome.artwork_key_label') }}</label>
        <input
          id="welcomeSteamGridDbKey"
          v-model="typedKey"
          type="password"
          autocomplete="off"
          class="settings-input font-mono text-sm"
          :placeholder="$t('welcome.artwork_key_placeholder')"
        />
        <div class="mt-3 flex flex-wrap items-center gap-3">
          <button
            type="button"
            class="inline-flex h-10 items-center justify-center rounded-xl bg-ice px-4 text-sm font-semibold text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 disabled:cursor-not-allowed disabled:opacity-60"
            :disabled="busy !== '' || !typedKey.trim()"
            @click="checkAndSave"
          >
            {{ busy === 'checking' ? $t('welcome.artwork_checking') : busy === 'saving' ? $t('welcome.artwork_saving') : $t('welcome.artwork_check_and_save') }}
          </button>
          <button type="button" class="text-sm text-storm underline-offset-4 hover:underline" @click="$emit('skip')">
            {{ $t('welcome.skip_for_now') }}
          </button>
        </div>
      </div>

      <div
        v-if="outcome"
        class="rounded-2xl border px-4 py-3 text-sm"
        :class="outcome.ok ? 'border-success/25 bg-success/10 text-success-bright' : 'border-danger/25 bg-danger/10 text-danger-bright'"
        role="status"
      >
        <div>{{ outcome.text || $t(outcome.textKey, outcome.params || {}) }}</div>
        <div v-if="outcome.hintKey" class="mt-1 text-xs opacity-80">{{ $t(outcome.hintKey) }}</div>
      </div>

      <p class="text-xs text-storm">{{ $t('welcome.artwork_later') }}</p>
    </div>
  </div>
</template>

<script setup>
import { computed, ref } from 'vue'

const props = defineProps({
  configData: { type: Object, default: null },
  patchConfig: { type: Function, required: true },
})
const emit = defineEmits(['skip', 'saved'])

const typedKey = ref('')
const busy = ref('')
const outcome = ref(null)
const savedThisSession = ref(false)

const hasStoredKey = computed(() => savedThisSession.value || !!props.configData?.has_steamgriddb_api_key)

// The host checks the key against SteamGridDB without storing it, so the
// answer is real even though the running host only reads a saved key after
// a restart.
async function checkKey(body) {
  const response = await fetch('./api/covers/key/check', {
    method: 'POST',
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  let payload = null
  try {
    payload = await response.json()
  } catch {
    payload = null
  }
  return payload || { status: false, code: 'steamgriddb_unreachable', error: '' }
}

function hintKeyFor(code) {
  if (code === 'steamgriddb_unauthorized') return 'welcome.artwork_hint_unauthorized'
  if (code === 'steamgriddb_unreachable') return 'welcome.artwork_hint_unreachable'
  if (code === 'steamgriddb_rate_limited') return 'welcome.artwork_hint_rate_limited'
  return ''
}

function failure(result) {
  return {
    ok: false,
    text: result?.error || '',
    textKey: 'welcome.artwork_hint_unreachable',
    hintKey: hintKeyFor(result?.code),
  }
}

async function checkStoredKey() {
  busy.value = 'checking'
  outcome.value = null
  try {
    const result = await checkKey({ use_stored: true })
    outcome.value = result.status ? { ok: true, textKey: 'welcome.artwork_stored_ok' } : failure(result)
  } catch {
    outcome.value = failure(null)
  } finally {
    busy.value = ''
  }
}

async function checkAndSave() {
  const key = typedKey.value.trim()
  if (!key) return
  busy.value = 'checking'
  outcome.value = null
  try {
    const result = await checkKey({ steamgriddb_api_key: key })
    if (!result.status) {
      outcome.value = failure(result)
      return
    }
    busy.value = 'saving'
    const saved = await props.patchConfig({ steamgriddb_api_key: key })
    if (!saved.ok) {
      outcome.value = { ok: false, textKey: 'welcome.artwork_save_failed', params: { error: saved.error || '' } }
      return
    }
    typedKey.value = ''
    savedThisSession.value = true
    outcome.value = { ok: true, textKey: 'welcome.artwork_ok', params: { matches: result.matches ?? 0 }, hintKey: 'welcome.artwork_saved' }
    emit('saved')
  } catch {
    outcome.value = failure(null)
  } finally {
    busy.value = ''
  }
}
</script>
