<template>
  <div class="page-shell">
    <section class="page-header system-page-header">
      <div class="page-heading">
        <div class="section-kicker">{{ $t('index.host_health') }}</div>
        <h1 class="page-title">{{ systemHeaderTitle }}</h1>
        <p class="page-subtitle">Version, health, and recent issues.</p>
        <div class="page-meta">
          <span class="meta-pill font-medium" :class="healthBadgeClass">
            {{ healthLabel }}
          </span>
          <template v-if="hasIssueCounts">
            <span v-for="counter in issueCounters" :key="counter.label" class="meta-pill">
              <span class="font-medium text-silver">{{ counter.count }}</span>
              <span class="ml-1">{{ $t(counter.label) }}</span>
            </span>
          </template>
          <span v-else class="meta-pill border-green-500/20 bg-green-500/10 text-green-200">
            {{ $t('index.no_active_issues') }}
          </span>
        </div>
      </div>

      <div class="system-toolbar-notes">
        <article class="header-support-card">
          <div class="header-support-title-row">
            <div class="section-kicker !mb-0">Version</div>
            <button
              type="button"
              class="focus-ring inline-flex h-7 items-center justify-center rounded-lg border border-storm px-2.5 text-[11px] font-medium text-silver transition-[border-color,color,background-color] duration-200 hover:border-ice hover:text-ice"
              @click="copyVersion"
            >
              {{ copiedVersion ? $t('index.copied') : $t('index.copy_version') }}
            </button>
          </div>
          <div class="header-support-value">{{ version?.version || '—' }}</div>
          <div class="header-support-copy">{{ versionHeaderSummary }}</div>
          <a
            v-if="stableBuildAvailable"
            class="focus-ring mt-3 inline-flex h-8 items-center justify-center rounded-lg bg-ice px-3 text-sm font-medium text-void transition-[background-color,box-shadow] duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] no-underline"
            :href="githubVersion.release.html_url"
            target="_blank"
          >
            {{ $t('index.view_release') }}
          </a>
          <a
            v-else-if="notifyPreReleases && preReleaseBuildAvailable"
            class="focus-ring mt-3 inline-flex h-8 items-center justify-center rounded-lg border border-purple-400/30 px-3 text-sm font-medium text-purple-200 transition-[background-color,border-color,color] duration-200 hover:bg-purple-500/10 no-underline"
            :href="preReleaseVersion.release.html_url"
            target="_blank"
          >
            {{ $t('index.view_release') }}
          </a>
        </article>

        <article class="header-support-card">
          <div class="section-kicker">Issues</div>
          <div class="header-support-value">{{ recentIssues.length }}</div>
          <div class="header-support-copy">
            {{ recentIssues.length ? `${recentIssues.length} ${$t('index.visible_now')}` : 'No recent warnings or errors' }}
          </div>
        </article>
      </div>
    </section>

    <section class="section-card">
      <div class="flex flex-col gap-3 lg:flex-row lg:items-end lg:justify-between">
        <div>
          <div class="section-kicker">Host</div>
          <div class="section-title-row">
            <h2 class="section-title">Telemetry</h2>
            <InfoHint size="sm" label="System snapshot details">
              {{ $t('index.system_snapshot_desc') }}
            </InfoHint>
          </div>
        </div>
        <div class="text-xs text-storm">{{ systemLoading ? 'Refreshing…' : 'Live' }}</div>
      </div>

      <div class="mt-5 grid gap-3 md:grid-cols-2 xl:grid-cols-4">
        <article class="rounded-2xl border border-storm/20 bg-deep/40 p-4">
          <div class="text-[10px] font-semibold uppercase tracking-[0.18em] text-storm">{{ $t('index.gpu_health') }}</div>
          <div v-if="gpu" class="mt-3">
            <div class="min-w-0 text-sm font-medium text-silver">{{ gpu.name || $t('index.gpu_active') }}</div>
            <div class="mt-3 flex items-end gap-2 tabular-nums">
              <div class="text-3xl font-semibold text-purple-300">{{ gpu.utilization_pct ?? '--' }}<span class="text-base">%</span></div>
              <div class="pb-1 text-xs text-storm">{{ $t('index.gpu_utilization') }}</div>
            </div>
            <div class="mt-3 flex flex-wrap gap-2 text-xs text-storm tabular-nums">
              <span>{{ gpu.temperature_c ?? '--' }}°C</span>
              <span>{{ gpu.encoder_pct ?? '--' }}% {{ $t('index.encoder_short') }}</span>
              <span>{{ gpu.power_draw_w?.toFixed?.(0) ?? '--' }}W</span>
            </div>
          </div>
          <div v-else class="mt-3 text-sm text-storm">{{ $t('index.gpu_unavailable') }}</div>
        </article>

        <article class="rounded-2xl border border-storm/20 bg-deep/40 p-4">
          <div class="text-[10px] font-semibold uppercase tracking-[0.18em] text-storm">{{ $t('index.display_state') }}</div>
          <div class="mt-3 flex items-end gap-2 tabular-nums">
            <div class="text-3xl font-semibold text-silver">{{ displays.length }}</div>
            <div class="pb-1 text-xs text-storm">{{ $t('index.active_displays') }}</div>
          </div>
          <div class="mt-3 space-y-1 text-sm text-storm">
            <div v-if="displays.length === 0">{{ $t('index.no_display_data') }}</div>
            <div v-for="display in displays.slice(0, 3)" :key="display.name || display.id || display.label" class="truncate text-silver" :title="formatDisplayName(display)">
              {{ formatDisplayName(display) }}
            </div>
          </div>
        </article>

        <article class="rounded-2xl border border-storm/20 bg-deep/40 p-4">
          <div class="text-[10px] font-semibold uppercase tracking-[0.18em] text-storm">{{ $t('index.audio_state') }}</div>
          <div class="mt-3 text-sm font-medium text-silver">
            {{ audio?.sink ? formatAudioName(audio.sink) : $t('index.audio_unavailable') }}
          </div>
          <div class="mt-3 text-xs text-storm">
            {{ audio?.sink ? formatAudioDetail(audio.sink) : $t('index.audio_unavailable_desc') }}
          </div>
        </article>

        <article class="rounded-2xl border border-storm/20 bg-deep/40 p-4">
          <div class="text-[10px] font-semibold uppercase tracking-[0.18em] text-storm">{{ $t('index.session_mode') }}</div>
          <div class="mt-3 text-sm font-medium capitalize text-silver">{{ sessionType || $t('index.session_mode_idle') }}</div>
          <div class="mt-3 text-xs text-storm">
            {{ sessionType ? sessionModeDescription : $t('index.session_mode_idle_desc') }}
          </div>
        </article>
      </div>
    </section>

    <div class="grid gap-4 xl:grid-cols-[minmax(0,1fr)_minmax(0,0.95fr)]">
      <section class="section-card">
        <div class="section-kicker">Actions</div>
        <div class="section-title-row">
          <h2 class="section-title">Shortcuts</h2>
          <InfoHint size="sm" label="Quick action details">
            {{ $t('index.quick_actions_desc') }}
          </InfoHint>
        </div>
        <div class="mt-5 grid gap-3 md:grid-cols-2">
          <router-link
            v-for="action in quickActions"
            :key="action.to"
            class="action-tile"
            :to="action.to"
          >
            <div class="section-title-row">
              <div class="text-sm font-medium text-silver">{{ $t(action.titleKey) }}</div>
              <InfoHint size="sm" align="right" :label="`${$t(action.titleKey)} details`">
                {{ $t(action.descKey) }}
              </InfoHint>
            </div>
          </router-link>
        </div>
      </section>

      <section class="section-card">
        <div class="section-kicker">Clients</div>
        <div class="section-title-row">
          <h2 class="section-title">Supported paths</h2>
          <InfoHint size="sm" label="Compatibility details">
            {{ $t('index.compatibility_desc') }}
          </InfoHint>
        </div>
        <div class="mt-5 grid gap-3 sm:grid-cols-2 xl:grid-cols-1">
          <div
            v-for="client in compatibilityClients"
            :key="client.platform"
            class="surface-subtle p-4"
          >
            <div class="flex items-center justify-between gap-3">
              <div class="text-sm font-medium text-silver">{{ client.platform }}</div>
              <span class="rounded-full border px-2.5 py-1 text-[10px] font-medium uppercase tracking-[0.18em]" :class="client.link ? 'border-ice/30 bg-ice/10 text-ice' : 'border-storm/30 bg-deep/60 text-storm'">
                {{ client.status }}
              </span>
            </div>
            <div class="mt-2 text-sm text-storm">{{ client.name }}</div>
            <a
              v-if="client.link"
              class="mt-3 inline-flex text-sm font-medium text-ice no-underline hover:text-ice/80"
              :href="client.link"
              target="_blank"
            >
              {{ $t('index.view_client') }}
            </a>
          </div>
        </div>
        <div class="mt-4 text-sm italic text-storm">{{ $t('client_card.generic_moonlight_clients_desc') }}</div>
      </section>
    </div>

    <section class="section-card">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
        <div>
          <div class="section-kicker">Resources</div>
          <div class="section-title-row">
            <h2 class="section-title">Links</h2>
            <InfoHint size="sm" label="Resource details">
              {{ $t('index.resources_desc') }}
            </InfoHint>
          </div>
        </div>
        <div class="flex flex-wrap gap-2">
          <a
            v-for="resource in resources"
            :key="resource.href"
            class="focus-ring inline-flex h-9 items-center justify-center rounded-lg border border-storm px-4 text-sm font-medium text-silver transition-[border-color,color,box-shadow,background-color] duration-200 hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] no-underline"
            :href="resource.href"
            target="_blank"
          >
            {{ $t(resource.labelKey) }}
          </a>
        </div>
      </div>
    </section>

    <section class="rounded-2xl border border-storm/20 bg-deep/30 p-4">
      <div class="flex flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
        <div>
          <div class="section-kicker">Legal</div>
          <p class="mt-2 text-sm text-storm">{{ $t('resource_card.legal_desc') }}</p>
        </div>
        <div class="flex flex-wrap gap-2">
          <a
            v-for="document in legalDocs"
            :key="document.href"
            class="focus-ring inline-flex h-9 items-center justify-center rounded-lg bg-red-500/20 px-4 text-sm font-medium text-red-300 transition-[background-color,color] duration-200 hover:bg-red-500/30 no-underline"
            :href="document.href"
            target="_blank"
          >
            {{ $t(document.labelKey) }}
          </a>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
import { useSystemStats } from '../composables/useSystemStats'
import PolarisVersion from '../polaris_version'
import InfoHint from '../components/InfoHint.vue'

const { gpu, displays, audio, sessionType, loading: systemLoading } = useSystemStats(3000)

const version = ref(null)
const githubVersion = ref(null)
const notifyPreReleases = ref(false)
const preReleaseVersion = ref(null)
const loading = ref(true)
const logs = ref(null)
const copiedVersion = ref(false)

const compatibilityClients = [
  { platform: 'Android', name: 'Nova', status: 'Supported', link: 'https://github.com/papi-ux/nova' },
  { platform: 'iOS', name: 'Coming Soon', status: 'Planned', link: '' },
  { platform: 'Desktop', name: 'Coming Soon', status: 'Planned', link: '' }
]

const resources = [
  { href: 'https://github.com/papi-ux/polaris/discussions', labelKey: 'resource_card.github_discussions' },
  { href: 'https://github.com/papi-ux/polaris/wiki', labelKey: 'resource_card.github_wiki' },
  { href: 'https://github.com/papi-ux/polaris/wiki/Stuttering-Clinic', labelKey: 'resource_card.github_stuttering_clinic' }
]

const legalDocs = [
  { href: 'https://github.com/papi-ux/polaris/blob/master/LICENSE', labelKey: 'resource_card.license' },
  { href: 'https://github.com/papi-ux/polaris/blob/master/NOTICE', labelKey: 'resource_card.third_party_notice' }
]

const quickActions = [
  { to: '/', titleKey: 'index.quick_mission_title', descKey: 'index.quick_mission_desc' },
  { to: '/troubleshooting#logs', titleKey: 'index.quick_logs_title', descKey: 'index.quick_logs_desc' },
  { to: '/troubleshooting', titleKey: 'index.quick_troubleshoot_title', descKey: 'index.quick_troubleshoot_desc' },
  { to: '/config', titleKey: 'index.quick_settings_title', descKey: 'index.quick_settings_desc' }
]

const installedVersionNotStable = computed(() => {
  if (!githubVersion.value || !version.value) return false
  return version.value.isGreater(githubVersion.value)
})

const stableBuildAvailable = computed(() => {
  if (!githubVersion.value || !version.value) return false
  return githubVersion.value.isGreater(version.value)
})

const preReleaseBuildAvailable = computed(() => {
  if (!preReleaseVersion.value || !githubVersion.value || !version.value) return false
  return preReleaseVersion.value.isGreater(version.value, true) && preReleaseVersion.value.isGreater(githubVersion.value, true)
})

const buildVersionIsDirty = computed(() => {
  return version.value?.version?.split('.').length === 5 &&
    version.value.version.includes('dirty')
})

const fancyLogs = computed(() => {
  if (!logs.value) return []
  const regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g
  const rawLogLines = logs.value.split(regex).splice(1)
  const logLines = []
  for (let i = 0; i < rawLogLines.length; i += 2) {
    logLines.push({ timestamp: rawLogLines[i], level: rawLogLines[i + 1].split(':')[0], value: rawLogLines[i + 1] })
  }
  return logLines
})

const issueLogs = computed(() => {
  return fancyLogs.value.filter((entry) => ['Fatal', 'Warning', 'Error'].includes(entry.level))
})

function normalizeIssueMessage(value) {
  return value.trim().replace(/^\w+:\s*/, '').replace(/\s+/g, ' ')
}

const groupedIssueLogs = computed(() => {
  const grouped = new Map()

  issueLogs.value
    .slice(-200)
    .reverse()
    .forEach((entry) => {
      const message = normalizeIssueMessage(entry.value)
      const key = `${entry.level}:${message}`
      if (!grouped.has(key)) {
        grouped.set(key, {
          ...entry,
          message,
          count: 1
        })
        return
      }

      grouped.get(key).count += 1
    })

  return Array.from(grouped.values())
})

const fatalCount = computed(() => groupedIssueLogs.value.filter((entry) => entry.level === 'Fatal').length)
const warningCount = computed(() => groupedIssueLogs.value.filter((entry) => entry.level === 'Warning').length)
const errorCount = computed(() => groupedIssueLogs.value.filter((entry) => entry.level === 'Error').length)
const recentIssues = computed(() => groupedIssueLogs.value.slice(0, 3))
const issueCounters = computed(() => [
  buildIssueCounter(fatalCount.value, 'index.fatal_issues', 'fatal'),
  buildIssueCounter(warningCount.value, 'index.warning_issues', 'warning'),
  buildIssueCounter(errorCount.value, 'index.error_issues', 'error')
])
const hasIssueCounts = computed(() => fatalCount.value > 0 || warningCount.value > 0 || errorCount.value > 0)

const healthState = computed(() => {
  if (fatalCount.value > 0) return 'critical'
  if (warningCount.value > 0 || errorCount.value > 0) return 'warning'
  return 'healthy'
})

const healthLabel = computed(() => {
  switch (healthState.value) {
    case 'critical':
      return 'Critical'
    case 'warning':
      return 'Warning'
    default:
      return 'Healthy'
  }
})

const systemHeaderTitle = computed(() => {
  switch (healthState.value) {
    case 'critical':
      return 'Host needs attention'
    case 'warning':
      return 'Host status is mixed'
    default:
      return 'Host is healthy'
  }
})

const healthBadgeClass = computed(() => {
  switch (healthState.value) {
    case 'critical':
      return 'border-red-500/30 bg-red-500/10 text-red-200'
    case 'warning':
      return 'border-amber-300/30 bg-amber-300/10 text-amber-200'
    default:
      return 'border-green-500/30 bg-green-500/10 text-green-200'
  }
})

const versionHeaderSummary = computed(() => {
  if (!version.value) return 'Version unavailable'
  if (buildVersionIsDirty.value) return 'Local dirty build'
  if (installedVersionNotStable.value) return 'Running newer than the latest stable tag'
  if (stableBuildAvailable.value) return 'New stable release available'
  if (notifyPreReleases.value && preReleaseBuildAvailable.value) return 'New prerelease available'
  return 'Current public release'
})

function buildIssueCounter(count, label, tone) {
  if (count === 0) {
    return {
      count,
      label,
      cardClass: 'border border-storm/20 bg-deep/60',
      valueClass: 'text-silver',
      labelClass: 'text-storm'
    }
  }

  if (tone === 'fatal') {
    return {
      count,
      label,
      cardClass: 'border border-red-500/20 bg-red-500/10',
      valueClass: 'text-red-300',
      labelClass: 'text-red-200/70'
    }
  }

  if (tone === 'warning') {
    return {
      count,
      label,
      cardClass: 'border border-amber-300/20 bg-amber-300/10',
      valueClass: 'text-amber-200',
      labelClass: 'text-amber-200/70'
    }
  }

  return {
    count,
    label,
    cardClass: 'border border-blue-400/20 bg-blue-400/10',
    valueClass: 'text-blue-200',
    labelClass: 'text-blue-200/70'
  }
}

function formatAudioName(sink) {
  const matchUsb = sink.match(/usb-(.+?)-\d+\./)
  if (matchUsb) return matchUsb[1].replace(/_/g, ' ')
  const matchTail = sink.match(/\.([^.]+)$/)
  if (matchTail) return matchTail[1].replace(/-/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase())
  return sink.length > 30 ? `${sink.substring(0, 30)}…` : sink
}

function formatAudioDetail(sink) {
  if (!sink) return ''
  if (sink.includes('usb-')) return 'USB output sink active.'
  if (sink.includes('hdmi')) return 'HDMI output sink active.'
  if (sink.includes('analog')) return 'Analog output sink active.'
  return 'Host output sink active.'
}

function formatDisplayName(display) {
  const base = display.friendly_name || display.name || display.id || 'Display'
  if (display.width && display.height) {
    return `${base} · ${display.width}×${display.height}`
  }
  return base
}

const sessionModeDescription = computed(() => {
  const mode = String(sessionType.value || '').toLowerCase()
  if (!mode) return ''
  if (mode.includes('wayland')) return 'Wayland compositor active.'
  if (mode.includes('x11')) return 'X11 desktop session active.'
  if (mode.includes('headless')) return 'Headless Stream runtime active.'
  return 'Live compositor mode reported by the host.'
})

async function copyVersion() {
  if (!version.value?.version || !navigator.clipboard) return
  await navigator.clipboard.writeText(version.value.version)
  copiedVersion.value = true
  window.setTimeout(() => {
    copiedVersion.value = false
  }, 1800)
}

;(async () => {
  try {
    const config = await fetch('./api/config', { credentials: 'include' }).then((r) => r.json())
    notifyPreReleases.value = config.notify_pre_releases
    version.value = new PolarisVersion(null, config.version)
    githubVersion.value = new PolarisVersion(await fetch('https://api.github.com/repos/papi-ux/polaris/releases/latest').then((r) => r.json()), null)
    if (githubVersion.value) {
      preReleaseVersion.value = new PolarisVersion((await fetch('https://api.github.com/repos/papi-ux/polaris/releases').then((r) => r.json())).find((release) => release.prerelease), null)
    }
  } catch {
    // GitHub API may be blocked by CSP — version check is non-critical
  }
  try {
    logs.value = await fetch('./api/logs', { credentials: 'include' }).then((r) => r.text())
  } catch (error) {
    console.error(error)
  }
  loading.value = false
})()
</script>
