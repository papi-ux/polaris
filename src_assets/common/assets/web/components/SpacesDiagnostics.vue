<template>
  <details class="section-card settings-disclosure" data-spaces-doctor :aria-busy="loading" @toggle="toggle">
    <summary class="settings-disclosure-summary focus-ring cursor-pointer rounded py-2 font-semibold text-silver">
      <span class="flex flex-wrap items-center gap-2">
        <span>{{ t('title') }}</span>
        <StatusBadge role="status" aria-live="polite" :status="loading || !read ? 'info' : report.status" :label="loading ? t('checking') : read ? report.summary : t('unchecked')" />
      </span>
    </summary>
    <div v-if="open" class="mt-3 space-y-4">
      <div class="flex flex-wrap items-start justify-between gap-3">
        <div>
          <p class="section-copy">{{ t('description') }}</p>
          <p v-if="checkedAt && !loading" class="mt-1 text-xs text-storm" data-spaces-checked-at>{{ t('checked_at', { time: checkedAt }) }}</p>
        </div>
        <Button variant="outline" size="sm" :disabled="loading" :loading="loading" data-spaces-doctor-refresh @click="refresh">{{ t('refresh') }}</Button>
      </div>
      <p v-if="loading" class="text-sm text-storm" role="status">{{ t('checking_detail') }}</p>
      <template v-else-if="read">
        <section v-for="group in report.groups" :key="group.id" :data-spaces-doctor-group="group.id">
          <h3 class="text-sm font-semibold text-silver">{{ group.title }}</h3>
          <div class="mt-2 grid gap-3 md:grid-cols-2">
            <article v-for="finding in group.rows" :key="finding.id" class="surface-subtle min-w-0 border p-3"
                     :class="statusTone(finding.status).card" :data-spaces-finding="finding.id" data-readonly>
              <div class="flex flex-wrap items-start justify-between gap-2">
                <h4 class="min-w-0 break-words text-sm font-medium text-silver">{{ finding.title }}</h4>
                <StatusBadge :status="finding.status" :label="t(`status_${finding.status}`)" />
              </div>
              <p class="mt-2 break-words text-sm text-storm">{{ finding.detail }}</p>
            </article>
          </div>
        </section>
      </template>
      <router-link to="/spaces" class="focus-ring inline-block rounded py-2 text-sm text-ice hover:underline" data-spaces-doctor-open>{{ t('open_spaces') }}</router-link>
    </div>
  </details>
</template>

<script setup>
import { computed, inject, onBeforeUnmount, ref } from 'vue'
import Button from './Button.vue'
import StatusBadge from './StatusBadge.vue'
import { statusTone } from '../status-tones.js'
import { validSetup } from '../spaces-setup.js'
import { validSnapshot } from '../spaces-access.js'
import { spacesDiagnostics } from '../spaces-diagnostics.js'

const i18n = inject('i18n')
const t = (key, params) => i18n.t(`spaces_doctor.${key}`, params)
const open = ref(false), loading = ref(false), read = ref(false), checkedAt = ref('')
const evidence = ref({ setup: null, snapshot: null })
const report = computed(() => spacesDiagnostics(evidence.value, i18n.t.bind(i18n)))
let request = null, requestTimer = null, disposed = false

function cancel() {
  clearTimeout(requestTimer)
  requestTimer = null
  request?.abort()
  request = null
  loading.value = false
}

async function refresh() {
  if (disposed || !open.value || loading.value) return
  const current = new AbortController()
  request = current
  loading.value = true
  read.value = false
  checkedAt.value = ''
  // Old green evidence must not survive a refresh that cannot verify this host.
  evidence.value = { setup: null, snapshot: null }
  const timeout = setTimeout(() => current.abort(), 12000)
  requestTimer = timeout
  const load = async (url, valid) => {
    const response = await fetch(url, { credentials: 'include', cache: 'no-store', signal: current.signal })
    if (!response.ok) throw new Error('unavailable')
    const value = await response.json()
    if (!valid(value)) throw new Error('invalid')
    return value
  }
  try {
    const [setup, snapshot] = await Promise.allSettled([
      load('./api/spaces/setup', validSetup), load('./api/multiseat/profiles', validSnapshot),
    ])
    if (disposed || request !== current) return
    evidence.value = {
      setup: !current.signal.aborted && setup.status === 'fulfilled' ? setup.value : null,
      setupError: current.signal.aborted || setup.status === 'rejected' ? 'unavailable' : '',
      snapshot: !current.signal.aborted && snapshot.status === 'fulfilled' ? snapshot.value : null,
      snapshotError: current.signal.aborted || snapshot.status === 'rejected' ? 'unavailable' : '',
    }
    read.value = true
    checkedAt.value = new Date().toLocaleTimeString()
  } finally {
    clearTimeout(timeout)
    if (request === current) {
      request = null
      requestTimer = null
      loading.value = false
    }
  }
}

function toggle(event) {
  const next = event.target.open
  if (next === open.value) return
  open.value = next
  if (open.value) refresh()
  else if (!open.value) cancel()
}

onBeforeUnmount(() => { disposed = true; cancel() })
</script>
