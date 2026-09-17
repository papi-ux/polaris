<template>
  <div class="app-editor-field app-editor-field-wide" data-app-artwork>
    <div class="settings-field-head">
      <span class="settings-field-label">{{ $t('apps.artwork_title') }}</span>
    </div>
    <p class="text-sm leading-relaxed text-storm" data-app-artwork-state>
      {{ lookupOff ? $t('apps.artwork_off', { name: app.name }) : $t('apps.artwork_on') }}
    </p>
    <div class="mt-2 flex flex-wrap gap-2">
      <Button v-if="!lookupOff" type="button" variant="outline" size="sm" data-app-artwork-remove
              :disabled="disabled || busy" @click="openRemove">
        {{ $t('apps.artwork_remove') }}
      </Button>
      <Button v-else type="button" variant="outline" size="sm" data-app-artwork-find
              :disabled="disabled || busy" :loading="busy" @click="findAgain">
        {{ $t('apps.artwork_find_again') }}
      </Button>
    </div>
    <ConfirmActionDialog
      v-model="removeOpen"
      :title="$t('apps.artwork_remove_title', { name: app.name })"
      :message="$t('apps.artwork_remove_message')"
      :impact-items="[$t('apps.artwork_remove_impact_picked'), $t('apps.artwork_remove_impact_own'), $t('apps.artwork_remove_impact_undo')]"
      :confirm-label="$t('apps.artwork_remove')"
      :cancel-label="$t('_common.cancel')"
      :pending-label="$t('apps.artwork_removing')"
      :pending="busy"
      :error="removeError"
      :eyebrow="$t('apps.artwork_title')"
      :impact-label="$t('apps.artwork_impact_label')"
      @confirm="confirmRemove"
      @cancel="removeError = ''"
    />
  </div>
</template>

<script setup>
import { inject, ref } from 'vue'
import Button from './Button.vue'
import ConfirmActionDialog from './ConfirmActionDialog.vue'
import { useToast } from '../composables/useToast'
import { findAppArtwork, removeAppArtwork } from '../app-artwork.js'

const props = defineProps({
  // The saved entry being edited: its name and uuid.
  app: { type: Object, required: true },
  lookupOff: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
})
const emit = defineEmits(['changed'])
const i18n = inject('i18n')
const t = (key, params) => i18n.t(key, params)
const { toast } = useToast()
const removeOpen = ref(false)
const busy = ref(false)
const removeError = ref('')

function report(result) {
  if (result.lookupOff !== undefined) emit('changed', { uuid: props.app.uuid, lookupOff: result.lookupOff })
}

function openRemove() {
  removeError.value = ''
  removeOpen.value = true
}

async function confirmRemove() {
  if (busy.value) return
  busy.value = true
  removeError.value = ''
  const result = await removeAppArtwork(props.app.uuid)
  busy.value = false
  report(result)
  if (!result.ok) {
    removeError.value = result.error || t('apps.artwork_remove_failed')
    return
  }
  removeOpen.value = false
  toast(t('apps.artwork_removed', { name: props.app.name }), 'success')
}

async function findAgain() {
  if (busy.value) return
  busy.value = true
  const result = await findAppArtwork(props.app.uuid)
  busy.value = false
  report(result)
  if (result.ok) toast(t('apps.artwork_found_again', { name: props.app.name }), 'success')
  else toast(result.error || t('apps.artwork_find_failed'), 'error')
}
</script>
