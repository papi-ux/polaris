<template>
  <div class="page-shell system-console">
    <header class="system-console-header">
      <div class="page-heading">
        <div class="section-kicker">{{ $t('index.host_health') }}</div>
        <h1 class="page-title">{{ systemHeaderTitle }}</h1>
        <p class="page-subtitle">{{ $t('index.page_subtitle') }}</p>
      </div>
      <div class="system-header-actions">
        <button
          type="button"
          class="focus-ring system-button system-button-secondary"
          :disabled="checkingUpdates"
          @click="refreshSystemPage"
        >
          {{ checkingUpdates ? $t('index.refreshing') : $t('index.refresh') }}
        </button>
        <router-link class="focus-ring system-button system-button-primary" to="/troubleshooting">
          {{ $t('index.troubleshoot') }}
        </router-link>
      </div>
    </header>

    <section data-system-status-strip class="system-status-strip" :aria-label="$t('index.host_health_summary')">
      <article class="system-status-item">
        <div class="system-status-heading">
          <span class="section-kicker">{{ $t('index.health') }}</span>
          <span class="system-status-dot" :class="healthBadgeClass" aria-hidden="true"></span>
        </div>
        <div class="system-status-value">{{ healthLabel }}</div>
        <div class="system-status-copy">{{ recentIssues.length ? $t('index.health_copy_issues') : $t('index.health_copy_clear') }}</div>
      </article>

      <article class="system-status-item">
        <div class="system-status-heading">
          <span class="section-kicker">{{ $t('index.version') }}</span>
          <button type="button" class="focus-ring system-inline-action" @click="copyVersion">
            {{ copiedVersion ? $t('index.copied') : $t('index.copy_version') }}
          </button>
        </div>
        <div class="system-status-value">{{ version?.version || '—' }}</div>
        <div class="system-status-copy">{{ versionHeaderSummary }}</div>
      </article>

      <article class="system-status-item">
        <div class="system-status-heading">
          <span class="section-kicker">{{ $t('index.issues') }}</span>
          <span class="system-status-count">{{ groupedIssueLogs.length }}</span>
        </div>
        <div class="system-status-value">{{ groupedIssueLogs.length ? $t('index.issues_needs_review') : $t('index.issues_clear') }}</div>
        <div class="system-status-copy">{{ groupedIssueLogs.length ? $t('index.issues_copy_grouped', { grouped: groupedIssueLogs.length, shown: recentIssues.length }) : $t('index.issues_copy_clean') }}</div>
      </article>
    </section>

    <div class="system-ops-grid">
      <section data-system-telemetry class="section-card system-telemetry-panel">
        <div class="system-panel-heading">
          <div>
            <div class="section-kicker">{{ $t('index.host_now') }}</div>
            <div class="section-title-row">
              <h2 class="section-title">{{ $t('index.telemetry') }}</h2>
            </div>
          </div>
          <span class="system-telemetry-live meta-pill" :class="telemetryLiveClass">
            {{ telemetryLiveLabel }}
          </span>
        </div>

        <div class="system-telemetry-grid">
          <article class="system-telemetry-item">
            <div class="system-telemetry-heading">
              <span class="system-telemetry-label">{{ $t('index.gpu_health') }}</span>
              <span class="system-telemetry-state" :class="gpu ? 'system-telemetry-state-success' : 'system-telemetry-state-muted'">{{ gpu ? $t('index.state_nominal') : $t('index.state_unavailable') }}</span>
            </div>
            <template v-if="gpu">
              <div class="system-telemetry-value text-accent">{{ gpu.utilization_pct ?? '--' }}<span>%</span></div>
              <div class="system-telemetry-copy">{{ gpu.name || $t('index.gpu_active') }}</div>
              <div class="system-telemetry-meta">
                {{ gpu.temperature_c ?? '--' }}°C · {{ gpu.encoder_pct ?? '--' }}% {{ $t('index.encoder_short') }} · {{ gpu.power_draw_w?.toFixed?.(0) ?? '--' }}W
              </div>
            </template>
            <div v-else class="system-telemetry-copy">{{ $t('index.gpu_unavailable') }}</div>
          </article>

          <article class="system-telemetry-item">
            <div class="system-telemetry-heading">
              <span class="system-telemetry-label">{{ $t('index.display_state') }}</span>
              <span class="system-telemetry-state" :class="displays.length ? 'system-telemetry-state-success' : 'system-telemetry-state-muted'">{{ displays.length ? $t('index.state_active') : $t('index.state_idle') }}</span>
            </div>
            <div class="system-telemetry-value">{{ displays.length }}</div>
            <div class="system-telemetry-copy">{{ $t('index.active_displays') }}</div>
            <div class="system-telemetry-meta">
              <template v-if="displays.length">
                {{ displays.slice(0, 2).map(formatDisplayName).join(' · ') }}
              </template>
              <template v-else>{{ $t('index.no_display_data') }}</template>
            </div>
          </article>

          <article class="system-telemetry-item">
            <div class="system-telemetry-heading">
              <span class="system-telemetry-label">{{ $t('index.audio_state') }}</span>
              <span class="system-telemetry-state" :class="audio?.sink ? 'system-telemetry-state-success' : 'system-telemetry-state-muted'">{{ audio?.sink ? $t('index.state_ready') : $t('index.state_unavailable') }}</span>
            </div>
            <div class="system-telemetry-value system-telemetry-value-text">
              {{ audio?.sink ? formatAudioName(audio.sink) : $t('index.audio_unavailable') }}
            </div>
            <div class="system-telemetry-meta">
              {{ audio?.sink ? formatAudioDetail(audio.sink) : $t('index.audio_unavailable_desc') }}
            </div>
          </article>

          <article class="system-telemetry-item">
            <div class="system-telemetry-heading">
              <span class="system-telemetry-label">{{ $t('index.session_mode') }}</span>
              <span class="system-telemetry-state" :class="sessionType ? 'system-telemetry-state-success' : 'system-telemetry-state-muted'">{{ sessionType ? $t('index.state_active') : $t('index.state_idle') }}</span>
            </div>
            <div class="system-telemetry-value system-telemetry-value-text">
              {{ sessionType || $t('index.session_mode_idle') }}
            </div>
            <div class="system-telemetry-meta">
              {{ sessionType ? sessionModeDescription : $t('index.session_mode_idle_desc') }}
            </div>
            <div
              v-if="displaySession?.environment_repaired"
              data-display-session-health
              class="system-session-health border-success/30 bg-success/10 text-success-bright"
            >
              {{ $t('index.session_repaired') }}
            </div>
            <div
              v-else-if="displaySession?.status === 'missing_display_environment'"
              data-display-session-health
              class="system-session-health border-warning/30 bg-warning/10 text-warning-bright"
            >
              {{ $t('index.session_missing_env') }}
            </div>
          </article>
        </div>
      </section>

      <aside data-system-recent-issues class="section-card system-recent-issues-panel">
        <div class="section-kicker">{{ $t('index.recent_issues') }}</div>
        <h2 class="section-title">{{ recentIssues.length ? $t('index.recent_issues_review') : $t('index.recent_issues_clear') }}</h2>

        <div v-if="recentIssues.length" class="system-issue-list">
          <article
            v-for="issue in recentIssues"
            :key="`${issue.level}:${issue.message}`"
            class="system-issue-item"
            :class="issueSeverityClass(issue.level)"
          >
            <div class="system-issue-heading">
              <span>{{ issue.level }}</span>
              <span v-if="issue.count > 1">×{{ issue.count }}</span>
            </div>
            <div class="system-issue-message">{{ issue.message }}</div>
            <time class="system-issue-time" :datetime="issue.timestamp || undefined">{{ issue.timestamp }}</time>
          </article>
        </div>

        <div v-else class="system-issues-empty">
          <div class="system-issues-check" aria-hidden="true">✓</div>
          <div class="system-issues-empty-title">{{ $t('index.no_warnings_title') }}</div>
          <p>{{ $t('index.no_warnings_copy') }}</p>
          <span>{{ $t('index.latest_scan_note') }}</span>
        </div>

        <div class="system-issue-actions">
         <span v-if="groupedIssueLogs.length > recentIssues.length" class="system-issue-more">{{ $t('index.more_in_logs', { count: groupedIssueLogs.length - recentIssues.length }) }}</span>
          <router-link class="focus-ring system-text-link" to="/troubleshooting#logs">{{ $t('index.open_logs') }}</router-link>
          <router-link v-if="recentIssues.length" class="focus-ring system-text-link" to="/troubleshooting">{{ $t('index.run_guided') }}</router-link>
        </div>
      </aside>
    </div>

    <section id="update-center" ref="updateCenterSection" data-system-update-summary class="section-card system-update-section">
      <div class="system-update-summary">
        <span
          data-update-status-light
          class="system-update-status-light"
          :class="updateCenterStatusLightClass"
          :aria-label="updateCenterState.statusLightLabel"
          role="status"
        ></span>
        <div class="system-update-copy">
          <div class="section-kicker">{{ $t('index.software') }}</div>
          <h2 class="section-title">{{ $t('index.update_center') }}</h2>
          <p><span class="system-update-state-summary">{{ updateCenterState.statusLabel }}</span> · {{ updateCheckError || updateCenterState.primaryActionSummary }}</p>
          <p class="system-update-safety">
            {{ $t('index.never_auto_installs') }}
            <a href="https://papi-ux.com/docs/repositories/#after-install-or-upgrade" target="_blank" rel="noopener" class="focus-ring system-text-link">{{ $t('index.updates_docs_link') }}</a>
          </p>
        </div>
        <div class="system-update-actions">
          <button
            data-update-center-cta
            type="button"
            class="focus-ring system-button"
            :class="updateCenterCtaClass"
            :disabled="updateCenterState.primaryActionKind === 'none'"
            @click="handlePrimaryUpdateAction"
          >
            {{ copiedInstallCommand ? $t('index.copied') : updateCenterState.primaryActionLabel }}
          </button>
          <button
            data-update-center-refresh
            type="button"
            class="focus-ring system-button system-button-secondary"
            :disabled="checkingUpdates"
            @click="refreshUpdateStatus"
          >
            {{ checkingUpdates ? $t('index.checking') : $t('index.check_again') }}
          </button>
          <button
            v-if="!updateDetailsForced"
            type="button"
            class="focus-ring system-button system-button-secondary"
            :aria-expanded="String(showUpdateDetails)"
            aria-controls="system-update-details"
            @click="updateDetailsOpen = !updateDetailsOpen"
          >
            {{ showUpdateDetails ? $t('index.hide_details') : $t('index.update_details') }}
          </button>
        </div>
      </div>

      <div v-show="showUpdateDetails" id="system-update-details" data-update-center-details class="system-update-details">
        <div class="system-update-grid">
          <article class="surface-subtle p-4">
            <div class="system-telemetry-label">{{ $t('index.installed') }}</div>
            <div class="system-update-value">{{ updateCenterState.currentVersion || version?.version || '—' }}</div>
            <div class="system-update-meta">{{ $t('index.running_on_host') }}</div>
          </article>
          <article class="surface-subtle p-4">
            <div class="system-telemetry-label">{{ $t('index.latest') }}</div>
            <div class="system-update-value text-ice">{{ updateCenterState.latestVersion || '—' }}</div>
            <a v-if="updateCenterState.releaseUrl" class="system-text-link" :href="updateCenterState.releaseUrl" target="_blank" rel="noopener">{{ $t('index.view_release_notes') }}</a>
          </article>
          <article class="surface-subtle p-4">
            <div class="system-telemetry-label">{{ $t('index.package') }}</div>
            <div class="system-update-package">{{ updateCenterState.packageLabel || $t('index.manual_release_page') }}</div>
            <div class="system-update-meta break-all">{{ updateCenterState.asset?.name || $t('index.no_matching_package') }}</div>
            <div v-if="updateCenterState.assetDigest" class="system-update-digest">{{ updateCenterState.assetDigest }}</div>
          </article>
        </div>

        <div v-if="updateCenterState.asset || updateCenterState.releaseUrl" class="system-install-row">
          <div v-if="updateCenterState.canCopyInstallCommand" class="system-install-command">
            <div class="system-install-heading">
              <div>
                <div class="font-medium text-silver">{{ $t('index.manual_install_command') }}</div>
                <div class="system-update-meta">{{ $t('index.manual_install_copy') }}</div>
              </div>
              <button type="button" class="focus-ring system-button system-button-secondary" @click="copyInstallCommand">
                {{ copiedInstallCommand ? $t('index.copied') : $t('index.copy_command') }}
              </button>
            </div>
            <pre><code>{{ updateCenterState.installCommand }}</code></pre>
          </div>
          <div class="system-install-links">
            <a v-if="updateCenterState.asset" class="focus-ring system-button system-button-primary" :href="updateCenterState.asset.browser_download_url" target="_blank" rel="noopener">{{ $t('index.download_package') }}</a>
            <a v-if="updateCenterState.releaseUrl" class="focus-ring system-button system-button-secondary" :href="updateCenterState.releaseUrl" target="_blank" rel="noopener">{{ $t('index.open_release') }}</a>
          </div>
        </div>
      </div>
    </section>

    <nav class="system-quick-actions" :aria-label="$t('index.system_shortcuts')">
      <router-link v-for="action in quickActions" :key="action.to" class="focus-ring system-quick-action" :to="action.to">
        <span>{{ $t(action.titleKey) }}</span>
        <span aria-hidden="true">→</span>
      </router-link>
    </nav>

    <footer class="system-resource-footer">
      <div>
        <div class="section-kicker">{{ $t('index.product_resources') }}</div>
        <p>{{ $t('index.product_resources_copy') }}</p>
      </div>
      <div class="system-resource-links">
        <a class="focus-ring system-footer-link" href="https://github.com/papi-ux/nova" target="_blank">Nova</a>
        <a v-for="link in resources" :key="link.href" class="focus-ring system-footer-link" :href="link.href" target="_blank">
          {{ $t(link.labelKey) }}
        </a>
        <a
          class="focus-ring system-footer-link text-xs"
          :href="sponsor.href"
          target="_blank"
          rel="noopener noreferrer"
          :aria-label="$t(sponsor.ariaLabelKey)"
        >
          <span aria-hidden="true" class="text-danger/80">♥</span>
          {{ $t(sponsor.labelKey) }}
        </a>
        <a v-for="doc in legalDocs" :key="doc.href" class="focus-ring system-footer-link" :href="doc.href" target="_blank">
          {{ $t(doc.labelKey) }}
        </a>
      </div>
    </footer>
  </div>
</template>

<script setup>
import { ref, computed, inject } from 'vue'
import { useSystemStats } from '../composables/useSystemStats'
import PolarisVersion from '../polaris_version'
import { buildUpdateCenterState, updateStatusLightClass } from '../update-center.js'
import { createLogTailState, fetchLogTail } from '../log-tail-state.js'
import { groupRecentIssueLogs } from '../recent-issues.js'
import { resources, legalDocs, sponsor } from '../resource-links.js'

const i18n = inject('i18n')

const { gpu, displays, audio, sessionType, displaySession, loading: systemLoading } = useSystemStats(3000)

const version = ref(null)
const githubVersion = ref(null)
const notifyPreReleases = ref(false)
const preReleaseVersion = ref(null)
const logs = ref(null)
const copiedVersion = ref(false)
const copiedInstallCommand = ref(false)
const updateHost = ref({ platform: '', distro: {} })
const checkingUpdates = ref(false)
const updateCheckError = ref('')
const updateCenterSection = ref(null)
const updateDetailsOpen = ref(false)

const quickActions = [
  { to: '/apps', titleKey: 'index.quick_applications_title' },
  { to: '/troubleshooting#logs', titleKey: 'index.quick_logs_title' },
  { to: '/config', titleKey: 'index.quick_settings_title' }
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
  return version.value?.version?.split('.').length === 5 && version.value.version.includes('dirty')
})

// A commit-suffixed version such as 1.4.0.5fd06cc2 is a local build of that
// release, not the packaged one. It must not read as the current public release.
const buildVersionIsLocal = computed(() => {
  const parts = String(version.value?.version || '').split('.')
  return parts.length >= 4 && /^[0-9a-f]{7,}$/i.test(parts[3] || '')
})

const groupedIssueLogs = computed(() => groupRecentIssueLogs(logs.value, { maxSourceLines: 200 }))
const fatalCount = computed(() => groupedIssueLogs.value.filter((entry) => entry.level === 'Fatal').length)
const warningCount = computed(() => groupedIssueLogs.value.filter((entry) => entry.level === 'Warning').length)
const errorCount = computed(() => groupedIssueLogs.value.filter((entry) => entry.level === 'Error').length)
const recentIssues = computed(() => groupedIssueLogs.value.slice(0, 3))

const healthState = computed(() => {
  if (fatalCount.value > 0) return 'critical'
  if (warningCount.value > 0 || errorCount.value > 0) return 'warning'
  return 'healthy'
})

const healthLabel = computed(() => {
  switch (healthState.value) {
    case 'critical':
      return i18n.t('index.health_critical')
    case 'warning':
      return i18n.t('index.health_warning')
    default:
      return i18n.t('index.health_healthy')
  }
})

const systemHeaderTitle = computed(() => {
  switch (healthState.value) {
    case 'critical':
      return i18n.t('index.header_attention')
    case 'warning':
      return i18n.t('index.header_mixed')
    default:
      return i18n.t('index.header_healthy')
  }
})

const healthBadgeClass = computed(() => {
  switch (healthState.value) {
    case 'critical':
      return 'border-danger/30 bg-danger/10 text-danger-bright'
    case 'warning':
      return 'border-warning/30 bg-warning/10 text-warning-bright'
    default:
      return 'border-success/30 bg-success/10 text-success-bright'
  }
})

const versionHeaderSummary = computed(() => {
  if (!version.value) return i18n.t('index.version_unavailable')
  if (buildVersionIsDirty.value) return i18n.t('index.version_dirty_build')
  if (installedVersionNotStable.value) return i18n.t('index.version_newer_than_stable')
  if (stableBuildAvailable.value) return i18n.t('index.version_new_stable_available')
  if (notifyPreReleases.value && preReleaseBuildAvailable.value) return i18n.t('index.version_new_prerelease_available')
  if (buildVersionIsLocal.value) return i18n.t('index.version_local_build')
  return i18n.t('index.version_current')
})

const updateCenterState = computed(() => buildUpdateCenterState({
  currentVersion: version.value?.version || '',
  latestRelease: githubVersion.value?.release || null,
  prereleaseRelease: preReleaseVersion.value?.release || null,
  includePrereleases: notifyPreReleases.value,
  host: updateHost.value,
}))

const updateCenterStatusLightClass = computed(() => updateStatusLightClass(updateCenterState.value.statusTone))

const updateCenterCtaClass = computed(() => {
  if (updateCenterState.value.status === 'update_available') {
    return 'system-button-primary'
  }
  if (updateCenterState.value.primaryActionKind === 'none') {
    return 'system-button-disabled'
  }
  return 'system-button-secondary'
})

const telemetryAvailable = computed(() => {
  return Boolean(gpu.value || displays.value.length || audio.value?.sink || sessionType.value)
})

const telemetryLiveLabel = computed(() => {
  if (systemLoading.value) return i18n.t('index.refreshing')
  return telemetryAvailable.value ? i18n.t('index.telemetry_live') : i18n.t('index.telemetry_unavailable')
})

const telemetryLiveClass = computed(() => {
  if (systemLoading.value) return 'border-ice/30 bg-ice/10 text-ice'
  return telemetryAvailable.value
    ? 'border-success/30 bg-success/10 text-success-bright'
    : 'border-storm/30 bg-storm/10 text-silver'
})

const updateDetailsForced = computed(() => {
  return updateCenterState.value.status === 'update_available' || Boolean(updateCheckError.value)
})

const showUpdateDetails = computed(() => {
  return updateDetailsOpen.value || updateDetailsForced.value
})

const sessionModeDescription = computed(() => {
  const mode = String(sessionType.value || '').toLowerCase()
  if (!mode) return ''
  if (mode.includes('wayland')) return i18n.t('index.session_wayland')
  if (mode.includes('x11')) return i18n.t('index.session_x11')
  if (mode.includes('headless')) return i18n.t('index.session_headless')
  return i18n.t('index.session_live')
})

function issueSeverityClass(level) {
  if (level === 'Fatal') return 'system-issue-fatal'
  if (level === 'Warning') return 'system-issue-warning'
  return 'system-issue-error'
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
  if (display.width && display.height) return `${base} · ${display.width}×${display.height}`
  return base
}

async function copyVersion() {
  if (!version.value?.version || !navigator.clipboard) return
  await navigator.clipboard.writeText(version.value.version)
  copiedVersion.value = true
  window.setTimeout(() => {
    copiedVersion.value = false
  }, 1800)
}

async function copyInstallCommand() {
  if (!updateCenterState.value.installCommand || !navigator.clipboard) return false
  await navigator.clipboard.writeText(updateCenterState.value.installCommand)
  copiedInstallCommand.value = true
  window.setTimeout(() => {
    copiedInstallCommand.value = false
  }, 1800)
  return true
}

function scrollToUpdateCenter() {
  updateDetailsOpen.value = true
  updateCenterSection.value?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}

async function handlePrimaryUpdateAction() {
  const action = updateCenterState.value.primaryActionKind
  if (action === 'refresh_update_status') {
    await refreshUpdateStatus()
    return
  }
  if (action === 'open_release' && updateCenterState.value.releaseUrl) {
    window.open(updateCenterState.value.releaseUrl, '_blank', 'noopener')
    return
  }
  scrollToUpdateCenter()
  if (action === 'copy_install_command') await copyInstallCommand()
}

async function refreshUpdateStatus() {
  checkingUpdates.value = true
  updateCheckError.value = ''
  try {
    const config = await fetch('./api/config', { credentials: 'include' }).then((response) => response.json())
    const hostStatus = await fetch('./api/update-status', { credentials: 'include' }).then((response) => response.json()).catch(() => null)
    updateHost.value = hostStatus || { platform: config.platform || '', distro: {} }
    notifyPreReleases.value = config.notify_pre_releases
    version.value = new PolarisVersion(null, hostStatus?.version || config.version)

    try {
      githubVersion.value = new PolarisVersion(await fetch('https://api.github.com/repos/papi-ux/polaris/releases/latest').then((response) => response.json()), null)
      const releases = await fetch('https://api.github.com/repos/papi-ux/polaris/releases').then((response) => response.json())
      const preRelease = releases.find((release) => release.prerelease)
      preReleaseVersion.value = preRelease ? new PolarisVersion(preRelease, null) : null
    } catch (error) {
      githubVersion.value = null
      preReleaseVersion.value = null
      updateCheckError.value = 'Release check unavailable'
      console.error(error)
    }
  } catch (error) {
    updateCheckError.value = 'Host update status unavailable'
    console.error(error)
  } finally {
    checkingUpdates.value = false
  }
}

async function fetchLogs() {
  try {
    logs.value = (await fetchLogTail(createLogTailState())).text
  } catch (error) {
    console.error(error)
  }
}

async function refreshSystemPage() {
  await Promise.all([
    refreshUpdateStatus(),
    fetchLogs(),
  ])
}

;(async () => {
  await refreshSystemPage()
})()
</script>
