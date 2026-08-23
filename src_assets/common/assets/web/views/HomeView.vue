<template>
  <div class="page-shell system-console">
    <header class="system-console-header">
      <div class="page-heading">
        <div class="section-kicker">{{ $t('index.host_health') }}</div>
        <h1 class="page-title">{{ systemHeaderTitle }}</h1>
        <p class="page-subtitle">Live host state, recent issues, and the controls that matter now.</p>
      </div>
      <div class="system-header-actions">
        <button
          type="button"
          class="focus-ring system-button system-button-secondary"
          :disabled="checkingUpdates"
          @click="refreshSystemPage"
        >
          {{ checkingUpdates ? 'Refreshing…' : 'Refresh' }}
        </button>
        <router-link class="focus-ring system-button system-button-primary" to="/troubleshooting">
          Troubleshoot
        </router-link>
      </div>
    </header>

    <section data-system-status-strip class="system-status-strip" aria-label="Host health summary">
      <article class="system-status-item">
        <div class="system-status-heading">
          <span class="section-kicker">Health</span>
          <span class="system-status-dot" :class="healthBadgeClass" aria-hidden="true"></span>
        </div>
        <div class="system-status-value">{{ healthLabel }}</div>
        <div class="system-status-copy">{{ recentIssues.length ? 'Recent host issues are ready to review.' : 'No active host issues.' }}</div>
      </article>

      <article class="system-status-item">
        <div class="system-status-heading">
          <span class="section-kicker">Version</span>
          <button type="button" class="focus-ring system-inline-action" @click="copyVersion">
            {{ copiedVersion ? $t('index.copied') : $t('index.copy_version') }}
          </button>
        </div>
        <div class="system-status-value">{{ version?.version || '—' }}</div>
        <div class="system-status-copy">{{ versionHeaderSummary }}</div>
      </article>

      <article class="system-status-item">
        <div class="system-status-heading">
          <span class="section-kicker">Issues</span>
          <span class="system-status-count">{{ groupedIssueLogs.length }}</span>
        </div>
        <div class="system-status-value">{{ groupedIssueLogs.length ? 'Needs review' : 'Clear' }}</div>
        <div class="system-status-copy">{{ groupedIssueLogs.length ? `${groupedIssueLogs.length} grouped issues · ${recentIssues.length} shown` : 'Latest bounded log scan is clean.' }}</div>
      </article>
    </section>

    <div class="system-ops-grid">
      <section data-system-telemetry class="section-card system-telemetry-panel">
        <div class="system-panel-heading">
          <div>
            <div class="section-kicker">Host now</div>
            <div class="section-title-row">
              <h2 class="section-title">Telemetry</h2>
              <InfoHint size="sm" label="System snapshot details">
                {{ $t('index.system_snapshot_desc') }}
              </InfoHint>
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
              <span class="system-telemetry-state" :class="gpu ? 'system-telemetry-state-success' : 'system-telemetry-state-muted'">{{ gpu ? 'Nominal' : 'Unavailable' }}</span>
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
              <span class="system-telemetry-state" :class="displays.length ? 'system-telemetry-state-success' : 'system-telemetry-state-muted'">{{ displays.length ? 'Active' : 'Idle' }}</span>
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
              <span class="system-telemetry-state" :class="audio?.sink ? 'system-telemetry-state-success' : 'system-telemetry-state-muted'">{{ audio?.sink ? 'Ready' : 'Unavailable' }}</span>
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
              <span class="system-telemetry-state" :class="sessionType ? 'system-telemetry-state-success' : 'system-telemetry-state-muted'">{{ sessionType ? 'Active' : 'Idle' }}</span>
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
              Desktop session environment was repaired automatically.
            </div>
            <div
              v-else-if="displaySession?.status === 'missing_display_environment'"
              data-display-session-health
              class="system-session-health border-warning/30 bg-warning/10 text-warning-bright"
            >
              Desktop preview environment is missing. Restart Polaris from the desktop session or run the user service so it inherits Wayland/X11.
            </div>
          </article>
        </div>
      </section>

      <aside data-system-recent-issues class="section-card system-recent-issues-panel">
        <div class="section-kicker">Recent issues</div>
        <h2 class="section-title">{{ recentIssues.length ? 'Host activity to review' : 'Nothing needs attention' }}</h2>

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
          <div class="system-issues-empty-title">No warnings or errors</div>
          <p>The latest bounded host log scan is clean.</p>
          <span>Latest scan · up to 200 source lines</span>
        </div>

        <div class="system-issue-actions">
         <span v-if="groupedIssueLogs.length > recentIssues.length" class="system-issue-more">+{{ groupedIssueLogs.length - recentIssues.length }} more in logs</span>
          <router-link class="focus-ring system-text-link" to="/troubleshooting#logs">Open logs →</router-link>
          <router-link v-if="recentIssues.length" class="focus-ring system-text-link" to="/troubleshooting">Run guided troubleshooting →</router-link>
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
          <div class="section-kicker">Software</div>
          <h2 class="section-title">Update Center</h2>
          <p><span class="system-update-state-summary">{{ updateCenterState.statusLabel }}</span> · {{ updateCheckError || updateCenterState.primaryActionSummary }}</p>
          <p class="system-update-safety">Polaris never auto-installs updates from this page.</p>
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
            {{ copiedInstallCommand ? 'Copied' : updateCenterState.primaryActionLabel }}
          </button>
          <button
            data-update-center-refresh
            type="button"
            class="focus-ring system-button system-button-secondary"
            :disabled="checkingUpdates"
            @click="refreshUpdateStatus"
          >
            {{ checkingUpdates ? 'Checking…' : 'Check again' }}
          </button>
          <button
            v-if="!updateDetailsForced"
            type="button"
            class="focus-ring system-button system-button-secondary"
            :aria-expanded="String(showUpdateDetails)"
            aria-controls="system-update-details"
            @click="updateDetailsOpen = !updateDetailsOpen"
          >
            {{ showUpdateDetails ? 'Hide details' : 'Update details' }}
          </button>
        </div>
      </div>

      <div v-show="showUpdateDetails" id="system-update-details" data-update-center-details class="system-update-details">
        <div class="system-update-grid">
          <article class="surface-subtle p-4">
            <div class="system-telemetry-label">Installed</div>
            <div class="system-update-value">{{ updateCenterState.currentVersion || version?.version || '—' }}</div>
            <div class="system-update-meta">Running on this host</div>
          </article>
          <article class="surface-subtle p-4">
            <div class="system-telemetry-label">Latest</div>
            <div class="system-update-value text-ice">{{ updateCenterState.latestVersion || '—' }}</div>
            <a v-if="updateCenterState.releaseUrl" class="system-text-link" :href="updateCenterState.releaseUrl" target="_blank">View release notes</a>
          </article>
          <article class="surface-subtle p-4">
            <div class="system-telemetry-label">Package</div>
            <div class="system-update-package">{{ updateCenterState.packageLabel || 'Manual release page' }}</div>
            <div class="system-update-meta break-all">{{ updateCenterState.asset?.name || 'No matching package detected for this host yet.' }}</div>
            <div v-if="updateCenterState.assetDigest" class="system-update-digest">{{ updateCenterState.assetDigest }}</div>
          </article>
        </div>

        <div v-if="updateCenterState.asset || updateCenterState.releaseUrl" class="system-install-row">
          <div v-if="updateCenterState.canCopyInstallCommand" class="system-install-command">
            <div class="system-install-heading">
              <div>
                <div class="font-medium text-silver">Manual install command</div>
                <div class="system-update-meta">Copy, inspect, and run locally when you are ready. Polaris never auto-installs from the web UI.</div>
              </div>
              <button type="button" class="focus-ring system-button system-button-secondary" @click="copyInstallCommand">
                {{ copiedInstallCommand ? 'Copied' : 'Copy command' }}
              </button>
            </div>
            <pre><code>{{ updateCenterState.installCommand }}</code></pre>
          </div>
          <div class="system-install-links">
            <a v-if="updateCenterState.asset" class="focus-ring system-button system-button-primary" :href="updateCenterState.asset.browser_download_url" target="_blank">Download package</a>
            <a v-if="updateCenterState.releaseUrl" class="focus-ring system-button system-button-secondary" :href="updateCenterState.releaseUrl" target="_blank">Open release</a>
          </div>
        </div>
      </div>
    </section>

    <nav class="system-quick-actions" aria-label="System shortcuts">
      <router-link v-for="action in quickActions" :key="action.to" class="focus-ring system-quick-action" :to="action.to">
        <span>{{ action.title }}</span>
        <span aria-hidden="true">→</span>
      </router-link>
    </nav>

    <footer class="system-resource-footer">
      <div>
        <div class="section-kicker">Product & resources</div>
        <p>Nova client support, documentation, community, and project information.</p>
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
import { ref, computed } from 'vue'
import { useSystemStats } from '../composables/useSystemStats'
import PolarisVersion from '../polaris_version'
import { buildUpdateCenterState, updateStatusLightClass } from '../update-center.js'
import InfoHint from '../components/InfoHint.vue'
import { createLogTailState, fetchLogTail } from '../log-tail-state.js'
import { groupRecentIssueLogs } from '../recent-issues.js'
import { resources, legalDocs, sponsor } from '../resource-links.js'

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
  { to: '/apps', title: 'Applications' },
  { to: '/troubleshooting#logs', title: 'Logs' },
  { to: '/config', title: 'Settings' }
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
      return 'border-danger/30 bg-danger/10 text-danger-bright'
    case 'warning':
      return 'border-warning/30 bg-warning/10 text-warning-bright'
    default:
      return 'border-success/30 bg-success/10 text-success-bright'
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
  if (systemLoading.value) return 'Refreshing…'
  return telemetryAvailable.value ? 'Live' : 'Unavailable'
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
  if (mode.includes('wayland')) return 'Wayland compositor active.'
  if (mode.includes('x11')) return 'X11 desktop session active.'
  if (mode.includes('headless')) return 'Headless · Private Stream runtime active.'
  return 'Live compositor mode reported by the host.'
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
