<template>
  <div class="page-shell operator-console mission-control-view relative">
    <!-- Just the title: the stage slate shows the state, and the readings carry the counts. -->
    <section>
      <div class="flex min-w-0 items-start justify-between gap-3">
        <h1 class="page-title">{{ $t('dashboard.title') }}</h1>
        <a v-if="!inNova" href="https://papi-ux.com/docs/mission-control/" target="_blank" rel="noopener" class="focus-ring inline-flex h-11 w-11 shrink-0 items-center justify-center rounded-lg text-storm hover:text-ice" :aria-label="t('dashboard.docs_link')" :title="t('dashboard.docs_link')" data-tab-docs-link>
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="9.25" /><path d="M9.6 9.3a2.5 2.5 0 0 1 4.8.9c0 1.7-2.4 2.2-2.4 3.8" /><path d="M12 17.2h.01" stroke-width="2.4" /></svg>
        </a>
      </div>
      <!-- Director's own greeting is its exact theme name. -->
      <p v-if="directorGreeting" class="page-subtitle mc-greeting" data-director-greeting><span class="mc-bracket">&lt;</span> {{ directorGreeting }} <span class="mc-bracket">&gt;</span></p>
      <p class="sr-only" role="status" aria-live="polite" aria-atomic="true" data-mission-status>{{ missionStatusLabel }}</p>
    </section>

    <DashboardSpaces v-if="hostPlatform === 'linux' && dashboardSpaces.visible" :model="dashboardSpaces" :loading="spacesLoading" @refresh="refreshSpaces" />

    <div class="mc-shell" :class="{ 'dashboard-live-shell': stageMode === 'live', 'is-preview-expanded': stageMode === 'live' && showPreview && previewExpanded }">
      <!-- The stage: what is playing (or next up), its main action, the route, then the readings.
           Space sessions keep their own section above and no stage. -->
      <MissionStage
        v-if="stageMode !== 'spaces'"
        ref="missionStage"
        :mode="stageMode"
        :data-dashboard-idle-hero="stageMode === 'idle' ? '' : undefined"
        :slate="stageView.slate"
        :timecode="stageView.timecode"
        :kicker="stageView.kicker"
        :title="stageView.title"
        :meta="stageView.meta"
        :note="stageView.note"
        :note-tone="stageView.noteTone"
        :art-src="stageView.artSrc"
        :hero-src="stageView.heroSrc"
        :logo-src="stageView.logoSrc"
        :hide-art="stageMode === 'live' && showPreview"
        :preview-expanded="previewExpanded"
      >
        <template v-if="stageMode === 'live' || stageMode === 'idle'" #reason>
          <!-- Only a blocked preview needs a reason; the stage note already says it starts on request. -->
          <p v-if="stageMode === 'live' && !showPreview && spacesPreviewBlocked" id="dashboard-preview-blocked-reason" class="mc-stage-reason" data-preview-hidden-reason>{{ $t('dashboard_spaces.no_preview') }}</p>
          <p v-if="stageMode === 'idle' && featuredApp && !currentAppAvailable" id="mission-stage-launch-reason" class="mc-stage-reason">Waiting for current app status.</p>
          <p v-if="stageMode === 'idle' && featuredApp && novaLaunchWaiting" id="mission-stage-nova-reason" class="mc-stage-reason" data-nova-launch-reason>{{ t('mission_control.nova_launch_waiting') }}</p>
        </template>

        <template v-if="stageMode !== 'loading'" #actions>
          <template v-if="stageMode === 'live'">
            <button v-if="!showPreview" :disabled="spacesPreviewBlocked || previewStarting" :aria-describedby="spacesPreviewBlocked ? 'dashboard-preview-blocked-reason' : undefined" data-start-preview @click="showPreviewFromStage" class="focus-ring dashboard-action-button dashboard-action-button-primary disabled:cursor-not-allowed disabled:opacity-50">
              {{ $t('dashboard.show_display') }}
            </button>
            <template v-else>
              <button type="button" class="focus-ring dashboard-action-button dashboard-action-button-primary" data-stop-preview @click="hidePreviewFromStage">
                {{ $t('dashboard.hide_display') }}
              </button>
              <button type="button" class="focus-ring dashboard-action-button dashboard-action-button-ghost" :aria-pressed="previewExpanded ? 'true' : 'false'" @click="togglePreviewExpanded">
                {{ previewExpanded ? $t('dashboard.collapse_display') : $t('dashboard.expand_display') }}
              </button>
            </template>
            <button type="button" class="focus-ring dashboard-action-button dashboard-action-button-ghost" data-open-diagnostics @click="openDiagnostics">
              {{ t('mission_control.session_diagnostics') }}
            </button>
          </template>
          <template v-else-if="stageMode === 'idle'">
            <!-- A launch check that needs attention outranks Launch, so a warning is never behind a disclosure. -->
            <router-link v-if="readyCheckDisplay.primaryIssue" :to="readyCheckDisplay.primaryIssue.to" class="focus-ring dashboard-action-button dashboard-action-button-primary no-underline" data-stage-priority-fix>
              {{ $t('dashboard.open_priority_fix') }}
            </router-link>
            <button
              v-if="featuredApp"
              type="button"
              class="focus-ring dashboard-action-button"
              :class="readyCheckDisplay.primaryIssue ? 'dashboard-action-button-ghost' : 'dashboard-action-button-primary'"
              :aria-label="t('mission_control.launch_title', { name: featuredApp.app.name })"
              :aria-disabled="!currentAppAvailable || Boolean(launchingUuid) || novaLaunchWaiting ? 'true' : undefined"
              :aria-describedby="!currentAppAvailable ? 'mission-stage-launch-reason' : novaLaunchWaiting ? 'mission-stage-nova-reason' : undefined"
              data-stage-launch
              @click="launchRecentApp(featuredApp.app)"
            >
              {{ launchingUuid === featuredApp.app.uuid ? $t('dashboard.launching') : $t('dashboard.launch') }}
            </button>
            <!-- The shelf below holds the Library link; the stage offers it only to an empty library. -->
            <router-link v-else to="/apps" class="focus-ring dashboard-action-button no-underline" :class="readyCheckDisplay.primaryIssue ? 'dashboard-action-button-ghost' : 'dashboard-action-button-primary'" data-stage-library>
              {{ $t('navbar.library') }}
            </router-link>
          </template>
          <router-link v-else to="/troubleshooting" class="focus-ring dashboard-action-button dashboard-action-button-primary no-underline" data-stage-troubleshoot>
            {{ $t('navbar.troubleshoot') }}
          </router-link>
        </template>

        <!-- The idle stage leaves its right side to the artwork; the host route is in Diagnostics. -->
        <template v-if="stageMode === 'live' || stageMode === 'unavailable'" #aside>
          <RoutePanel v-if="stageMode === 'live'" :title="t('mission_control.route')" :nodes="liveRouteNodes">
            <template #foot>
              <span class="meta-pill">{{ viewerCountLabel }}</span>
              <button
                v-if="connectedClientUuid"
                type="button"
                :disabled="disconnectingClient"
                @click="disconnectConfirmOpen = true"
                class="focus-ring dashboard-action-button mc-action-danger disabled:cursor-wait disabled:opacity-70"
                data-disconnect-client
              >
                {{ $t('dashboard.disconnect_client') }}
              </button>
            </template>
          </RoutePanel>
          <RoutePanel v-else :title="t('mission_control.route')" :nodes="unavailableRouteNodes" />
        </template>

        <!-- The live display replaces the artwork only when the viewer asks for it. -->
        <template v-if="stageMode === 'live' && showPreview" #preview>
          <div class="dashboard-preview-frame" :class="{ 'has-error': previewError, 'is-expanded': previewExpanded }" data-stage-preview-frame>
            <img
              :src="previewUrl"
              :alt="t('mission_control.preview_alt')"
              class="dashboard-preview-image"
              :class="{ 'opacity-0': !previewLoaded || previewError }"
              @load="handlePreviewLoad"
              @error="handlePreviewError"
            />
            <div v-if="!previewLoaded && !previewError" class="dashboard-preview-overlay" role="status" aria-live="polite">
              <svg class="h-7 w-7 animate-spin text-storm" fill="none" viewBox="0 0 24 24" aria-hidden="true">
                <circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" />
                <path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
              </svg>
              <span>{{ $t('dashboard.preview_capturing') }}</span>
            </div>
            <div v-if="previewError" class="dashboard-preview-overlay dashboard-preview-overlay-error" role="alert">
              <div class="text-sm font-medium text-silver">{{ $t('dashboard.preview_error') }}</div>
              <button @click="retryPreviewNow" class="focus-ring dashboard-action-button dashboard-action-button-secondary">
                {{ $t('dashboard.preview_retry') }}
              </button>
            </div>
            <span v-if="previewLoaded && !previewError" class="dashboard-preview-live-badge">
              <span class="text-success" aria-hidden="true">●</span>
              {{ previewMode === 'mjpeg' ? 'LIVE' : 'PREVIEW' }}
            </span>
            <!-- Stream facts ride the stage as overlays instead of chrome around it. -->
            <div v-if="previewLoaded && !previewError" class="dashboard-preview-hud">
              <div class="dashboard-preview-meta">
                <span v-if="streamFormat" class="data-pill">{{ streamFormat }}</span>
                <span v-if="hdrChipLabel" class="data-pill text-accent-2" :title="stats.hdr_downgrade_message || ''">{{ hdrChipLabel }}</span>
                <span class="data-pill">{{ capturePathLabel }}</span>
              </div>
              <span class="dashboard-preview-hud-status">{{ previewStatusText }}</span>
            </div>
          </div>
        </template>

        <template #signals>
          <div v-if="stageMode === 'loading'" class="mc-signals-loading" aria-hidden="true">
            <Skeleton type="card" v-for="n in 4" :key="n" />
          </div>
          <SignalStrip
            v-else-if="stageMode === 'live'"
            aria-label="Live stream telemetry summary"
            :cells="liveCells"
            charts
            :prefers-reduced-motion="prefersReducedMotion"
            :history="chartHistory"
            :history-revision="historyRevision"
          />
          <SignalStrip v-else-if="stageMode === 'idle'" :aria-label="t('mission_control.idle_signals_label')" :cells="idleCells" />
          <SignalStrip v-else :aria-label="t('mission_control.unavailable_signals_label')" :cells="unavailableCells" />
        </template>
      </MissionStage>

      <!-- The shelf: recent titles as posters. A poster features its title on the stage; Play launches it. -->
      <section v-if="stageMode === 'idle' && recentApps.length" class="mc-shelf" data-dashboard-play-rail aria-labelledby="mc-shelf-title">
        <div class="flex flex-wrap items-center justify-between gap-x-4 gap-y-1">
          <h2 id="mc-shelf-title" class="mc-panel-label"><span class="mc-bracket" aria-hidden="true">&lt;</span> {{ $t('dashboard.recent_games') }} <span class="mc-bracket" aria-hidden="true">&gt;</span></h2>
          <router-link to="/apps" class="focus-ring mc-inline-link">{{ $t('navbar.library') }} <span aria-hidden="true">→</span></router-link>
        </div>
        <p v-if="!currentAppAvailable" role="status" class="mt-2 text-sm text-support">Waiting for current app status.</p>
        <ul class="mc-film" @pointerdown.capture="notePlayPress">
          <li v-for="app in recentApps" :key="app.uuid" class="dashboard-play-tile">
            <div class="mc-film-art">
              <PosterImage class="w-full" :name="app.name" :src="appCoverSrc(app)" aria-hidden="true" />
              <button
                type="button"
                class="focus-ring mc-film-pick"
                :aria-label="t('mission_control.feature_title', { name: app.name })"
                :aria-pressed="featuredApp?.app.uuid === app.uuid ? 'true' : 'false'"
                :data-dashboard-feature-app="app.uuid"
                @click="featureApp(app)"
              ></button>
              <button
                type="button"
                class="focus-ring dashboard-action-button dashboard-action-button-primary mc-film-play"
                :aria-label="t('mission_control.launch_title', { name: app.name })"
                :disabled="launchingUuid === app.uuid || !currentAppAvailable || novaLaunchWaiting"
                :aria-describedby="novaLaunchWaiting ? 'mission-stage-nova-reason' : undefined"
                :data-dashboard-launch-app="app.uuid"
                @click="playClicked(app, $event)"
              >
                <svg viewBox="0 0 16 16" width="12" height="12" fill="currentColor" aria-hidden="true"><path d="M4 2.5v11l9.5-5.5z" /></svg>
                {{ launchingUuid === app.uuid ? $t('dashboard.launching') : $t('dashboard.launch') }}
              </button>
            </div>
            <div class="mc-film-name" :title="app.name">{{ app.name }}</div>
          </li>
        </ul>
      </section>

      <div v-if="stageMode !== 'loading'" class="mc-deck">
        <div class="mc-deck-main">
          <!-- Doctor: the host's own per-second diagnosis, written as a note with its guarded fix. -->
          <section v-if="stageMode === 'live'" class="dashboard-doctor-card mc-panel mc-doctor" :class="doctorPanelClass" data-dashboard-doctor>
            <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
              <div class="min-w-0">
                <h3 class="mc-panel-label"><span class="mc-bracket" aria-hidden="true">&lt;</span> {{ $t('dashboard.doctor') }} <span class="mc-bracket" aria-hidden="true">&gt;</span></h3>
                <div class="mt-2 flex min-w-0 items-start gap-2">
                  <span class="mt-2 inline-block h-2.5 w-2.5 shrink-0 rounded-full" :class="doctorLightClass" aria-hidden="true"></span>
                  <p class="mc-doctor-headline">{{ doctorHeadline }}</p>
                </div>
                <div class="mt-3 flex flex-wrap items-center gap-2" data-dashboard-doctor-meta role="list" aria-label="Doctor confidence and Live Tuning status">
                  <div v-if="doctorConfidenceLabel" class="control-chip" role="listitem">{{ doctorConfidenceLabel }}</div>
                  <div class="meta-pill" :class="autoQuality.toneClass" role="listitem">{{ autoQuality.compactLabel }}</div>
                  <div class="inline-flex" role="listitem">
                  </div>
                </div>
                <p v-if="doctorRecommendation" class="mt-3 max-w-3xl text-sm leading-relaxed text-support">{{ doctorRecommendation }}</p>
                <p v-if="doctorExplanation" class="mt-3 max-w-3xl rounded-lg border border-accent/20 bg-accent/5 px-3 py-2 text-sm leading-relaxed text-silver">
                  {{ doctorExplanation }}
                </p>
              </div>
              <div class="flex shrink-0 flex-wrap items-center gap-2">
                <button
                  v-if="doctorSafeAction && doctorActionExecutable"
                  type="button"
                  class="focus-ring dashboard-action-button dashboard-action-button-secondary disabled:cursor-wait disabled:opacity-70"
                  :disabled="doctorActionPending"
                  @click="requestDoctorSafeAction"
                >
                  {{ doctorActionPending ? $t('dashboard.doctor_action_working') : doctorSafeAction.label }}
                </button>
                <span v-else-if="doctorSafeAction" class="dashboard-doctor-advice" :title="doctorSafeAction.rollback || ''">
                  Suggested: {{ doctorSafeAction.label }}
                </span>
                <button
                  v-if="aiStatus?.enabled && doctor"
                  type="button"
                  class="focus-ring dashboard-action-button dashboard-action-button-ghost disabled:cursor-wait disabled:opacity-70"
                  :disabled="doctorExplainPending"
                  @click="explainDoctorVerdict"
                >
                  {{ doctorExplainPending ? $t('dashboard.doctor_explaining') : $t('dashboard.doctor_explain') }}
                </button>
                <button
                  v-if="doctorActionUndoAvailable"
                  type="button"
                  class="focus-ring dashboard-action-button dashboard-action-button-ghost disabled:cursor-wait disabled:opacity-70"
                  :disabled="doctorActionPending"
                  @click="undoDoctorAction"
                >
                  {{ $t('dashboard.doctor_action_undo') }}
                </button>
              </div>
            </div>
            <p v-if="doctorActionStatus" class="mt-3 text-xs leading-relaxed text-storm" role="status" aria-live="polite">
              {{ doctorActionStatus }}
            </p>
          </section>

          <!-- Without a current report Doctor waits instead of judging old numbers; an applied fix can still be undone. -->
          <section v-else-if="stageMode === 'unavailable'" class="mc-panel mc-doctor is-waiting" data-dashboard-doctor-waiting>
            <h3 class="mc-panel-label"><span class="mc-bracket" aria-hidden="true">&lt;</span> {{ $t('dashboard.doctor') }} <span class="mc-bracket" aria-hidden="true">&gt;</span></h3>
            <p class="mc-doctor-headline mt-2">{{ t('mission_control.doctor_waiting_title') }}</p>
            <p class="mt-2 max-w-3xl text-sm leading-relaxed text-support">{{ t('mission_control.doctor_waiting_body') }}</p>
            <div v-if="doctorActionUndoAvailable" class="mt-3 flex shrink-0 flex-wrap items-center gap-2">
              <button type="button" class="focus-ring dashboard-action-button dashboard-action-button-ghost disabled:cursor-wait disabled:opacity-70" :disabled="doctorActionPending" @click="undoDoctorAction">
                {{ $t('dashboard.doctor_action_undo') }}
              </button>
            </div>
            <p v-if="doctorActionStatus" class="mt-3 text-xs leading-relaxed text-storm" role="status" aria-live="polite">{{ doctorActionStatus }}</p>
          </section>

          <!-- Host vitals stay readable while collapsed, including without colour vision. -->
          <template v-if="stageMode === 'idle'">
            <details class="mc-panel mc-vitals" v-if="gpu" data-dashboard-idle-vitals>
              <summary class="focus-ring flex cursor-pointer flex-wrap items-center justify-between gap-3 text-sm text-silver">
                <span>Host vitals · {{ gpu.name }}</span>
                <span class="flex flex-wrap gap-x-3 gap-y-1 font-mono text-sm text-support" data-dashboard-vitals-summary>
                  <span :class="idleVitals.temperature.tone" data-vitals-temperature>{{ idleVitals.temperature.text }}</span>
                  <span :class="idleVitals.gpu.tone" data-vitals-gpu>{{ idleVitals.gpu.text }}</span>
                  <span :class="idleVitals.encoder.tone" data-vitals-encoder>{{ idleVitals.encoder.text }}</span>
                  <span :class="idleVitals.vram.tone" data-vitals-vram>{{ idleVitals.vram.text }}</span>
                </span>
              </summary>
              <div class="mt-4 grid grid-cols-2 gap-4 place-items-center xl:grid-cols-4">
                <GaugeArc v-if="gpu.temperature_c != null" :value="gpu.temperature_c" :max="100" unit="°C" label="Temp" :size="112"
                          :thresholds="[{ at: 0, color: 'var(--color-success)' }, { at: 70, color: 'var(--color-warning)' }, { at: 85, color: 'var(--color-danger)' }]" />
                <GaugeArc v-if="gpu.utilization_pct != null" :value="gpu.utilization_pct" :max="100" unit="%" label="GPU" :size="112" />
                <GaugeArc v-if="gpu.encoder_pct != null" :value="gpu.encoder_pct" :max="100" unit="%" :label="gpu.vendor === 'nvidia' ? 'NVENC' : 'VCN'" :size="112"
                          :thresholds="[{ at: 0, color: 'var(--color-ice)' }, { at: 60, color: 'var(--color-warning)' }, { at: 85, color: 'var(--color-danger)' }]" />
                <GaugeArc v-if="gpu.vram_used_mb != null && gpu.vram_total_mb != null" :value="gpu.vram_used_mb / 1024" :max="gpu.vram_total_mb / 1024" unit="GB" label="VRAM" :size="112"
                          :thresholds="[{ at: 0, color: 'var(--color-ice)' }, { at: 70, color: 'var(--color-warning)' }, { at: 90, color: 'var(--color-danger)' }]" />
              </div>
            </details>
            <div v-else class="dashboard-degraded-state">
              <div class="min-w-0">
                <div class="text-sm font-semibold text-silver">Host telemetry is warming up</div>
                <div class="mt-1 text-sm leading-relaxed text-support">GPU temperature, encoder load, and VRAM gauges will appear after the host reports system stats.</div>
              </div>
              <router-link to="/troubleshooting" class="focus-ring dashboard-degraded-action">
                Troubleshoot
              </router-link>
            </div>
          </template>

          <!-- One disclosure for the dense detail; opening it is the viewer's choice and survives updates. -->
          <MissionDiagnostics ref="diagnosticsPanel" v-model="diagnosticsOpen" :title="t('mission_control.diagnostics')" :summary="diagnosticsSummary">
            <div class="mc-diag-grid">
              <section v-if="stageMode === 'idle'" class="mc-diag-wide min-w-0" data-diagnostics-host>
                <RoutePanel :title="t('mission_control.host')" :nodes="idleRouteNodes">
                  <template #foot>
                    <template v-if="readinessSources.config === 'ready'">
                      <span class="meta-pill">{{ discoveryEnabled ? t('mission_control.discovery_on') : t('mission_control.discovery_off') }}</span>
                      <span class="meta-pill">{{ pairingEnabled ? t('mission_control.pairing_open') : t('mission_control.pairing_closed') }}</span>
                    </template>
                    <button type="button" class="focus-ring dashboard-action-button dashboard-action-button-ghost" :disabled="refreshingReadiness" @click="refreshLaunchChecks">{{ $t('dashboard.refresh_checks') }}</button>
                  </template>
                </RoutePanel>
              </section>

              <section v-if="stageMode === 'live' || stageMode === 'unavailable'" class="min-w-0" data-diagnostics-stream-status>
                <h4 class="mc-diag-section-title">{{ t('mission_control.diag_stream_status') }}</h4>
                <dl class="mc-kv">
                  <div><dt>{{ t('mission_control.diag_connection') }}</dt><dd>{{ streamStatusIsUnavailable ? t('mission_control.diag_disconnected') : t('mission_control.diag_connected') }}</dd></div>
                  <div><dt>{{ t('mission_control.diag_last_report') }}</dt><dd>{{ reportAgeLabel ? t('mission_control.age_ago', { age: reportAgeLabel }) : t('mission_control.no_report_received') }}</dd></div>
                </dl>
              </section>

              <section v-if="stageMode === 'live'" class="min-w-0">
                <h4 class="mc-diag-section-title">{{ t('mission_control.diag_frame_health') }}</h4>
                <div class="flex flex-wrap gap-1.5" data-dashboard-frame-health>
                  <span class="data-pill" :class="frameHealth.droppedTone">{{ frameHealth.dropped }} dropped</span>
                  <span class="data-pill" :class="frameHealth.duplicateTone">{{ frameHealth.duplicate }} duped</span>
                  <span class="data-pill" :class="frameHealth.intervalErrorTone">{{ frameHealth.intervalError }} target interval error</span>
                </div>
                <p v-if="prefersReducedMotion" class="mt-2 text-sm text-support">Live charts are paused while reduced motion is enabled.</p>
              </section>

              <section v-if="stageMode === 'live'" class="mc-diag-wide min-w-0">
                <StreamPacingReadout :stats="stats || {}" :connected="streamStatsConnected" :observed-at="streamStatsReceivedAt" />
              </section>

              <section v-if="stageMode === 'live'" class="min-w-0">
                <div class="mc-diag-section-head">
                  <h4 class="mc-diag-section-title">{{ $t('dashboard.runtime_path') }}</h4>
                  <router-link to="/troubleshooting" class="focus-ring mc-inline-link">
                    {{ $t('dashboard.runtime_detail_link') }}
                  </router-link>
                </div>
                <dl class="mc-kv">
                  <div><dt>{{ $t('dashboard.capture') }}</dt><dd :class="captureGpuNativeTone">{{ capturePathLabel }}</dd></div>
                  <div><dt>{{ t('mission_control.diag_runtime') }}</dt><dd>{{ runtimeEffectiveMode }}</dd></div>
                </dl>
                <p v-if="runtimePathNote" class="dashboard-rail-footnote mt-2 text-sm" :class="runtimePathNoteTone">{{ runtimePathNote }}</p>
              </section>

              <section v-if="stageMode === 'live' || stageMode === 'unavailable'" class="min-w-0">
                <h4 class="mc-diag-section-title">{{ t('mission_control.diag_host_vitals') }}</h4>
                <dl v-if="gpu" class="mc-kv">
                  <div v-for="row in gpuRows" :key="row.label"><dt>{{ row.label }}</dt><dd>{{ row.value }}</dd></div>
                </dl>
                <p v-else class="text-sm text-support">Host telemetry is warming up. GPU temperature, encoder load, and VRAM gauges will appear after the host reports system stats.</p>
              </section>

              <section v-if="stageMode === 'idle' || stageMode === 'spaces' || stageMode === 'unavailable'" class="mc-diag-wide min-w-0">
                <h4 class="mc-diag-section-title">{{ t('mission_control.diag_launch_checks') }}</h4>
                <div v-if="readyChecksAllPassing" class="ready-check-summary">
                  <div>
                    <div class="text-sm font-semibold text-success">All launch checks are ready</div>
                    <div class="mt-1 text-sm text-support">Pairing, library, discovery, displays, and audio are clear.</div>
                  </div>
                  <span class="rounded-full border border-success/30 bg-success/10 px-2.5 py-1 text-xs font-semibold uppercase tracking-eyebrow text-success">
                    {{ readyChecksPassing }}/{{ readyCheckDisplay.total }} {{ $t('dashboard.ready_checks_pass') }}
                  </span>
                </div>
                <div v-else class="space-y-3">
                  <div :class="readyChecksAttentionCount ? 'ready-check-summary ready-check-summary-attention' : 'ready-check-summary'">
                    <div>
                      <div :class="readyChecksAttentionCount ? 'text-sm font-semibold text-warning-bright' : 'text-sm font-semibold text-storm'">{{ readyChecksAttentionCount ? $t('dashboard.checks_attention', { count: readyChecksAttentionCount }) : $t('dashboard.checks_waiting') }}</div>
                      <div class="mt-1 text-sm text-support">
                        {{ readyChecksPrimaryIssue ? $t('dashboard.checks_start_with', { check: readyChecksPrimaryIssue.label }) : $t('dashboard.check_waiting') }}
                      </div>
                    </div>
                    <router-link
                      v-if="readyChecksPrimaryIssue"
                      :to="readyChecksPrimaryIssue.to"
                      class="focus-ring dashboard-degraded-action"
                    >
                      Open priority fix
                    </router-link>
                  </div>
                  <ul class="mc-checks">
                    <li v-for="check in visibleReadyChecks" :key="check.key">
                      <router-link :to="check.to" class="focus-ring mc-check no-underline" :class="check.cardClass">
                        <span class="mc-check-state" :class="check.badgeClass">{{ check.state }}</span>
                        <span class="mc-check-name">{{ check.label }}</span>
                        <span class="mc-check-detail">{{ check.detail }}</span>
                        <span class="mc-check-link">{{ $t(check.status === 'attention' ? 'dashboard.open_fix' : 'dashboard.review_check') }}</span>
                      </router-link>
                    </li>
                  </ul>
                </div>
              </section>

              <!-- Session history: the host's recorded outcomes, or this browser's richer local list. -->
              <section v-if="(stageMode === 'idle' || stageMode === 'spaces') && !sessions.length && hostHistoryRows.length" class="mc-diag-wide min-w-0" data-session-history-host>
                <div class="mc-diag-section-head">
                  <h4 class="mc-diag-section-title">{{ t('dashboard.session_history') }}</h4>
                  <button type="button" class="focus-ring mc-inline-link" data-clear-session-history @click="clearHistoryConfirmOpen = true">{{ t('dashboard.clear_history') }}</button>
                </div>
                <ul class="mc-history">
                  <li v-for="(s, i) in hostHistoryRows" :key="i">
                    <span class="mc-history-grade" :class="gradeColor(s.quality_grade)">{{ s.quality_grade || '·' }}</span>
                    <span class="mc-history-who">{{ s.key }}</span>
                  </li>
                </ul>
              </section>
              <section v-if="(stageMode === 'idle' || stageMode === 'spaces') && sessions.length" class="mc-diag-wide min-w-0" data-session-history-local>
                <div class="mc-diag-section-head">
                  <h4 class="mc-diag-section-title">{{ t('dashboard.session_history') }}</h4>
                  <button type="button" class="focus-ring mc-inline-link" data-clear-session-history @click="clearHistoryConfirmOpen = true">{{ t('dashboard.clear_history') }}</button>
                </div>
                <ul class="mc-history">
                  <li v-for="(s, i) in sessions.slice(0, 8)" :key="i">
                    <span class="mc-history-grade" :class="gradeColor(s.quality_grade)">{{ s.quality_grade }}</span>
                    <span class="min-w-0">
                      <span class="mc-history-who">{{ s.client_name }}</span>
                      <span class="mc-history-detail">{{ s.codec }} {{ s.width }}x{{ s.height }} · {{ formatDuration(s.duration_s) }}</span>
                    </span>
                    <span class="mc-history-when">{{ s.avg_fps }}fps · {{ (s.avg_bitrate_kbps / 1000).toFixed(1) }}Mbps<br>{{ formatSessionDate(s.started_at) }}</span>
                  </li>
                </ul>
              </section>
            </div>
          </MissionDiagnostics>
        </div>

        <aside v-if="stageMode === 'live' || stageMode === 'idle' || stageMode === 'unavailable'" class="mc-deck-side">
          <section class="mc-panel">
            <QuickControls :compact="stageMode === 'live'" @change="handleQuickControlChange" />
          </section>
        </aside>
      </div>
    </div>

    <ConfirmActionDialog
      v-model="disconnectConfirmOpen"
      :title="t('dashboard.disconnect_client_confirm_title')"
      :message="t('dashboard.disconnect_client_confirm_message', { client: currentClientName })"
      :impact-items="disconnectClientImpactItems"
      :confirm-label="t('dashboard.disconnect_client')"
      :cancel-label="t('_common.cancel')"
      :pending="disconnectingClient"
      :pending-label="t('dashboard.disconnect_client_pending')"
      @confirm="disconnectClient"
    />
    <ConfirmActionDialog
      v-model="clearHistoryConfirmOpen"
      :title="t('dashboard.clear_history_confirm_title')"
      :message="t('dashboard.clear_history_confirm_message')"
      :impact-items="[t('dashboard.clear_history_impact_host'), t('dashboard.clear_history_impact_browser')]"
      :confirm-label="t('dashboard.clear_history')"
      :cancel-label="t('_common.cancel')"
      :pending="clearingHistory"
      :pending-label="t('dashboard.clear_history_pending')"
      @confirm="clearAllSessionHistory"
    />
    <ConfirmActionDialog
      v-model="doctorActionConfirmOpen"
      :title="t('dashboard.doctor_action_confirm_title')"
      :message="doctorSafeAction ? `${doctorSafeAction.label}. ${doctorSafeAction.rollback || ''}` : ''"
      :confirm-label="doctorSafeAction?.label || ''"
      :cancel-label="t('_common.cancel')"
      :pending="doctorActionPending"
      @confirm="runDoctorSafeAction"
    />
  </div>
</template>

<script setup>
import { ref, computed, watch, onMounted, onUnmounted, nextTick } from 'vue'
import { useLiveTuning } from '../composables/useLiveTuning'
import { useStreamStats } from '../composables/useStreamStats'
import { useSystemStats } from '../composables/useSystemStats'
import { useSessionHistory, formatDuration } from '../composables/useSessionHistory'
import { useAiOptimizer } from '../composables/useAiOptimizer'
import { useFavicon } from '../composables/useFavicon'
import Skeleton from '../components/Skeleton.vue'
import DashboardSpaces from '../components/DashboardSpaces.vue'
import { useSpacesSnapshot } from '../composables/useSpacesSnapshot.js'
import { summarizeDashboardSpaces } from '../dashboard-spaces.js'
import GaugeArc from '../components/GaugeArc.vue'
import PosterImage from '../components/PosterImage.vue'
import MissionStage from '../components/MissionStage.vue'
import SignalStrip from '../components/SignalStrip.vue'
import RoutePanel from '../components/RoutePanel.vue'
import MissionDiagnostics from '../components/MissionDiagnostics.vue'
import { getThemeMeta, onThemeChange } from '../theme.js'
import QuickControls from '../components/QuickControls.vue'
import ConfirmActionDialog from '../components/ConfirmActionDialog.vue'
import { useToast } from '../composables/useToast'
import { useI18n } from 'vue-i18n'
import { resolveClientSettingsSync, resolveStreamDisplayMode } from '../client-settings-sync'
import { captureStatusTone } from '../capture-status-tone.js'
import StreamPacingReadout from '../components/StreamPacingReadout.vue'
import { resolveAutoQualityState } from '../auto-quality-state'
import { resolveDoctorActionHttpResponse } from '../doctor-action-http.js'
import { doctorIsWaitingForStream } from '../doctor-observation.js'
import { buildReadyCheckDisplay } from '../dashboard-ready-checks'
import { previewOutputForConfig } from '../dashboard-preview-output.js'
import { readConfigOrNull } from '../config-cache.js'
import { liveClientFamilyLabel, streamClientFamilyLabel } from '../client-family.js'
import { artLaunchHref, embeddedClient, followClientLink } from '../client-embed.js'
import {
  buildLiveSummary,
  buildQualityGrade,
  buildQualityScore,
  dashboardLossPct,
} from '../dashboard-summary'
import {
  appArtworkSrc,
  appCoverSrc,
  dynamicRangeLabel,
  featuredLibraryApp,
  formatReportAge,
  idleSignalCells,
  liveSignalCells,
  missionStageMode,
  reading,
  relativeDay,
  streamFormatLabel,
  streamStatusUnavailable,
  unavailableSignalCells,
} from '../mission-control.js'

const { stats, connected: streamStatsConnected, receivedAt: streamStatsReceivedAt } = useStreamStats(1000)
const { label: liveTuningLabel } = useLiveTuning()
const { gpu, displays, audio, sessionType, readiness: systemReadiness, refresh: refreshSystemStats } = useSystemStats(3000)

// Use the same percentage thresholds as the expanded gauges. State and missing
// telemetry remain readable while collapsed, including without colour vision.
const idleVitals = computed(() => {
  const device = gpu.value || {}
  const reading = (value, label, unit, warning, danger) => {
    if (value == null) return { text: label + ' unknown', tone: '' }
    const level = value >= danger ? 'high' : value >= warning ? 'elevated' : ''
    return { text: Number(value.toFixed(1)) + unit + ' ' + label + (level ? ' · ' + level : ''),
      tone: level === 'high' ? 'text-danger-bright' : level ? 'text-warning-bright' : '' }
  }
  const vramPercent = device.vram_used_mb != null && device.vram_total_mb > 0
    ? device.vram_used_mb / device.vram_total_mb * 100 : null
  return { temperature: reading(device.temperature_c, 'temperature', '°C', 70, 85),
    gpu: reading(device.utilization_pct, 'GPU', '%', 60, 85),
    encoder: reading(device.encoder_pct, 'encoder', '%', 60, 85),
    vram: reading(vramPercent, 'VRAM', '%', 70, 90) }
})
const { sessions, clearHistory, activeStartedAt } = useSessionHistory(stats)

// Apps catalog: recents for the play rail plus the running app for the hero.
let appsRequestRevision = 0
async function refreshApps() {
  const revision = ++appsRequestRevision
  try {
    const res = await fetch('./api/apps', { credentials: 'include' })
    if (res.ok) {
      const data = await res.json()
      if (revision !== appsRequestRevision) return
      readinessSources.value.apps = Array.isArray(data.apps) ? 'ready' : 'unavailable'
      hostUuid.value = typeof data.host_uuid === 'string' ? data.host_uuid : ''
      hostName.value = typeof data.host_name === 'string' ? data.host_name : ''
      currentAppAvailable.value = data.current_app_available !== false && typeof data.current_app === 'string'
      currentAppUuid.value = currentAppAvailable.value ? data.current_app : ''
      // The whole catalog, so the running app (Desktop included) resolves whether or not it is recent.
      allAppsById.value = Object.fromEntries((data.apps || []).filter(a => a.uuid).map(a => [a.uuid, a]))
      const apps = (data.apps || []).filter(a => a.uuid && a.name !== 'Desktop')
      apps.sort((a, b) => (b['last-launched'] || 0) - (a['last-launched'] || 0))
      appCatalogCount.value = apps.length
      recentApps.value = apps.slice(0, 8)
      // A featured pick lasts while its title is on the shelf and differs from the default.
      if (selectedFeaturedUuid.value && (!recentApps.value.some(app => app.uuid === selectedFeaturedUuid.value) ||
        recentApps.value[0]?.uuid === selectedFeaturedUuid.value)) selectedFeaturedUuid.value = ''
    } else if (revision === appsRequestRevision) {
      readinessSources.value.apps = 'unavailable'
      currentAppAvailable.value = false
      currentAppUuid.value = ''
    }
  } catch {
    if (revision !== appsRequestRevision) return
    readinessSources.value.apps = 'unavailable'
    currentAppAvailable.value = false
    currentAppUuid.value = ''
  }
}

// Hero context: what is being played and for how long. current_app arrives
// with the /api/apps payload; duration ticks against the SSE stats clock.
const currentAppUuid = ref('')
const currentAppAvailable = ref(false)
const allAppsById = ref({})
const currentApp = computed(() => (currentAppUuid.value ? allAppsById.value[currentAppUuid.value] || null : null))
const currentAppName = computed(() => currentApp.value?.name || '')

const sessionDurationLabel = computed(() => {
  // Touch stats so the label re-computes on every SSE tick.
  void stats.value
  if (!activeStartedAt.value) return ''
  const totalS = Math.max(0, Math.round((Date.now() - activeStartedAt.value) / 1000))
  if (totalS < 60) return `${totalS}s`
  const minutes = Math.floor(totalS / 60)
  if (minutes < 60) return `${minutes} min`
  return `${Math.floor(minutes / 60)} h ${minutes % 60} min`
})

const { status: aiStatus, fetchStatus: fetchAiStatus, fetchDevices: fetchAiDevices } = useAiOptimizer()

// AI optimization state for current stream
const aiCacheKeys = ref([])
const sessionHistory = ref([])
const recentApps = ref([])
const pairedClients = ref(0)
const appCatalogCount = ref(0)
const version = ref('')
const headlessEnabled = ref(false)
const discoveryEnabled = ref(false)
const pairingEnabled = ref(false)
const readinessSources = ref({ apps: 'pending', clients: 'pending', config: 'pending' })
const readinessConfig = ref(null)
const clientSettingsSync = ref(resolveClientSettingsSync({}))
const { t } = useI18n()
const { toast: showToast } = useToast()
const hostPlatform = ref('')
const { state: spacesState, loaded: spacesLoaded, loading: spacesLoading, loadError: spacesError,
  load: loadSpaces, start: refreshSpaces, stop: stopSpaces } = useSpacesSnapshot({
  shouldPoll: () => hostPlatform.value === 'linux' && (!spacesLoaded.value || spacesState.enabled),
})
const dashboardSpaces = computed(() => summarizeDashboardSpaces(spacesState, {
  loaded: spacesLoaded.value, error: spacesError.value, clients: pairedClientList.value,
}))
const spacesPreviewBlocked = computed(() => !hostPlatform.value ||
  (hostPlatform.value === 'linux' && dashboardSpaces.value.previewBlocked))
watch(hostPlatform, platform => {
  if (platform === 'linux') refreshSpaces()
  else stopSpaces()
})


const autoQuality = computed(() => resolveAutoQualityState(stats.value || {}, clientSettingsSync.value || {}))

// Announced to screen readers; sighted viewers read the same state from the stage slate.
const missionStatusLabel = computed(() => {
  if (streamStatusIsUnavailable.value) return t('mission_control.status_unavailable')
  if (stageMode.value === 'loading') return t('mission_control.connecting')
  if (dashboardSpaces.value.hasActivity) return t('dashboard_spaces.active_title')
  return stats.value?.streaming ? t('dashboard.live') : t('dashboard.standby')
})

const readinessLabel = computed(() => {
  if (stats.value?.streaming) return t('dashboard.readiness_live')
  if (readyCheckDisplay.value.allPassing) return t('dashboard.readiness_ready')
  return t(readyCheckDisplay.value.attention > 0 ? 'dashboard.readiness_setup' : 'dashboard.checks_waiting')
})

const readinessTone = computed(() => {
  if (stats.value?.streaming || readyCheckDisplay.value.allPassing) return 'text-success'
  return readyCheckDisplay.value.attention > 0 ? 'text-warning' : 'text-storm'
})

const readyChecks = computed(() => {
  const items = [
    {
      key: 'clients',
      to: '/pin',
      ok: pairedClients.value > 0,
      label: t('dashboard.check_clients'),
      detail: pairedClients.value > 0
        ? t('dashboard.check_clients_ok', { count: pairedClients.value })
        : t('dashboard.check_clients_missing')
    },
    {
      key: 'library',
      to: '/apps',
      ok: appCatalogCount.value > 0,
      label: t('dashboard.check_library'),
      detail: appCatalogCount.value > 0
        ? t('dashboard.check_library_ok', { count: appCatalogCount.value })
        : t('dashboard.check_library_missing')
    },
    {
      key: 'discovery',
      to: '/config#enable_discovery',
      ok: discoveryEnabled.value,
      label: t('dashboard.check_discovery'),
      detail: discoveryEnabled.value
        ? t('dashboard.check_discovery_ok')
        : t('dashboard.check_discovery_missing')
    },
    {
      key: 'pairing',
      to: '/config#enable_pairing',
      ok: pairingEnabled.value,
      label: t('dashboard.check_pairing'),
      detail: pairingEnabled.value
        ? t('dashboard.check_pairing_ok')
        : t('dashboard.check_pairing_missing')
    },
    {
      key: 'display',
      to: '/config#output_name',
      ok: displays.value.length > 0,
      label: t('dashboard.check_display'),
      detail: displays.value.length > 0
        ? t('dashboard.check_display_ok', { count: displays.value.length })
        : t('dashboard.check_display_missing')
    },
    {
      key: 'audio',
      to: '/config#audio_sink',
      ok: Boolean(audio.value?.sink),
      label: t('dashboard.check_audio'),
      detail: audio.value?.sink
        ? t('dashboard.check_audio_ok')
        : t('dashboard.check_audio_missing')
    }
  ]

  return items.map((item) => {
    const source = item.key === 'library' ? readinessSources.value.apps
      : item.key === 'clients' ? readinessSources.value.clients
        : ['display', 'audio'].includes(item.key) ? systemReadiness.value[item.key === 'display' ? 'displays' : 'audio']
          : readinessSources.value.config
    let status = source === 'ready' ? (item.ok ? 'ready' : 'attention') : 'not_checked'
    const configured = readinessSources.value.config === 'ready'
    if (configured && ((item.key === 'display' && resolveStreamDisplayMode(readinessConfig.value) === 'headless_stream') ||
        (item.key === 'audio' && readinessConfig.value.stream_audio === 'disabled') ||
        (item.key === 'discovery' && !discoveryEnabled.value) ||
        (item.key === 'pairing' && !pairingEnabled.value && readinessSources.value.clients === 'ready' && pairedClients.value > 0))) status = 'not_applicable'
    // Applicability depends on the configured mode, not a guessed default.
    if (['display', 'audio'].includes(item.key) && !configured) status = 'not_checked'
    const attention = status === 'attention'
    const ready = status === 'ready'
    return {
      ...item, status,
      state: t(`dashboard.check_state_${status}`),
      detail: status === 'not_checked' ? t(source === 'unavailable' ? 'dashboard.check_read_failed' : 'dashboard.check_waiting') : item.detail,
      cardClass: attention ? 'border-warning/15 bg-warning/5 hover:border-warning/30' : ready ? 'border-success/15 bg-success/5' : 'border-storm/25 bg-deep/30',
      badgeClass: attention ? 'bg-warning/10 text-warning-bright' : ready ? 'bg-success/10 text-success' : 'bg-storm/10 text-storm',
    }
  })
})

const readyCheckDisplay = computed(() => buildReadyCheckDisplay(readyChecks.value))
const readyChecksPassing = computed(() => readyCheckDisplay.value.passing)
const readyChecksAllPassing = computed(() => readyCheckDisplay.value.allPassing)
const visibleReadyChecks = computed(() => readyCheckDisplay.value.visibleChecks)
const readyChecksAttentionCount = computed(() => readyCheckDisplay.value.attention)
const readyChecksPrimaryIssue = computed(() => readyCheckDisplay.value.primaryIssue)

function handleQuickControlChange({ key, enabled }) {
  switch (key) {
    case 'enable_discovery':
      discoveryEnabled.value = enabled
      break
    case 'enable_pairing':
      pairingEnabled.value = enabled
      break
    case 'headless_mode':
      headlessEnabled.value = enabled
      break
    default:
      break
  }
}

const refreshingReadiness = ref(false)
async function refreshLaunchChecks() {
  if (refreshingReadiness.value) return
  refreshingReadiness.value = true
  readinessSources.value = { apps: 'pending', clients: 'pending', config: 'pending' }
  try {
    await Promise.allSettled([
      refreshApps(), refreshSystemStats(),
      (async () => {
        try {
          const res = await fetch('./api/clients/list', { credentials: 'include' })
          if (!res.ok) throw new Error('clients unavailable')
          const data = await res.json()
          if (!Array.isArray(data.named_certs)) throw new Error('clients unreported')
          pairedClientList.value = data.named_certs
          pairedClients.value = pairedClientList.value.length
          readinessSources.value.clients = 'ready'
        } catch { readinessSources.value.clients = 'unavailable' }
      })(),
      (async () => {
        try {
          const res = await fetch('./api/config', { credentials: 'include' })
          const data = await readConfigOrNull(res)
          if (!data) throw new Error('settings unavailable')
          refreshClientSettingsSync(data)
          version.value = data.version || ''
          headlessEnabled.value = data.headless_mode === 'enabled'
          discoveryEnabled.value = data.enable_discovery !== 'disabled'
          pairingEnabled.value = data.enable_pairing !== 'disabled'
        } catch { readinessSources.value.config = 'unavailable' }
      })(),
    ])
  } finally { refreshingReadiness.value = false }
}

function refreshClientSettingsSync(configPayload) {
  readinessConfig.value = configPayload
  readinessSources.value.config = configPayload ? 'ready' : 'unavailable'
  clientSettingsSync.value = resolveClientSettingsSync(configPayload || {})
  hostPlatform.value = configPayload?.platform || ''
}

const connectedClients = computed(() => {
  if (!stats.value?.streaming) return []

  if (Array.isArray(stats.value.clients) && stats.value.clients.length > 0) {
    return stats.value.clients
  }

  // A host that names no clients still has one viewer; its readings are the stream's own, and a
  // reading the host did not send stays missing rather than becoming 0.
  return [{
    name: stats.value.client_name || t('dashboard.unknown_client'),
    ip: stats.value.client_ip || '--',
    fps: stats.value.fps,
    latency_ms: stats.value.latency_ms,
  }]
})

const viewerCountLabel = computed(() => {
  // The host reports the true session count; client-list length is a fallback.
  const reported = Number(stats.value?.active_sessions)
  const count = Number.isFinite(reported) && reported > 0 ? reported : (connectedClients.value.length || 1)
  return `${count} ${count === 1 ? 'viewer' : 'viewers'}`
})

// First HDR surface in the web UI: state plus downgrade reason on hover.
const hdrChipLabel = computed(() => {
  // Host contract: hdr_effective_mode is sdr_8bit|sdr_10bit|hdr10, and a
  // DOWNGRADED stream reports an sdr_* mode with hdr_downgrade_reason set
  // (the literal "none" means no downgrade). So: downgrade first, then hdr.
  const s = stats.value || {}
  const reason = String(s.hdr_downgrade_reason || 'none')
  if (reason !== 'none') return 'HDR → SDR'
  const mode = String(s.hdr_effective_mode || s.dynamic_range || '').toLowerCase()
  if (!mode || mode.startsWith('sdr')) return ''
  return mode.toUpperCase()
})
const currentClientName = computed(() => connectedClientName.value || connectedClients.value[0]?.name || t('dashboard.unknown_client'))

function titleizeToken(value) {
  if (!value) return '--'
  return String(value)
    .split(/[_-]+/)
    .filter(Boolean)
    .map((part) => {
      const token = part.toLowerCase()
      const labels = {
        av1: 'AV1',
        bgra8: 'BGRA8',
        cpu: 'CPU',
        cuda: 'CUDA',
        dmabuf: 'DMA-BUF',
        drm: 'DRM',
        gpu: 'GPU',
        h264: 'H.264',
        hevc: 'HEVC',
        kms: 'KMS',
        nv12: 'NV12',
        nvenc: 'NVENC',
        p010: 'P010',
        shm: 'SHM',
        vaapi: 'VAAPI',
        yuv420p: 'YUV420P',
      }
      return labels[token] || part.charAt(0).toUpperCase() + part.slice(1)
    })
    .join(' ')
}

function captureReasonMessage(reason) {
  const key = String(reason || '').toLowerCase()
  const messages = {
    gpu_native: 'Capture and encoder conversion are GPU-resident.',
    headless_extcopy_dmabuf: 'True-headless DMA-BUF capture is active; frames stay GPU-resident through the encoder path.',
    windowed_dmabuf_override: 'Windowed private compositor is preserving the GPU-native capture path.',
    headless_shm_fallback: 'Private Stream is using SHM/system-memory capture. The stream can be healthy, including AMD/VAAPI conservative baselines.',
    headless_shm_default: 'Private Stream is using SHM/system-memory capture. The stream can be healthy, including AMD/VAAPI conservative baselines.',
    gpu_native_requested_shm_fallback: 'GPU-native capture was requested, but Wayland capture fell back to SHM/system-memory frames.',
    gpu_native_requested_cpu_capture: 'GPU-native capture was requested, but capture frames are CPU-resident.',
    gpu_native_requested_cpu_encode_upload: 'GPU-native capture was requested, but encoder upload/conversion is CPU-resident.',
    encoder_upload_cpu: 'Capture is GPU-resident, but encoder upload/conversion crosses system memory.',
    cpu_capture: 'The active capture path is CPU-resident.',
    shm_capture: 'The active capture path is CPU-resident.',
    dmabuf_gpu_capture: 'Capture is using DMA-BUF/GPU frames, but the encoder path is not fully GPU-native.',
    no_capture_metadata: 'No capture metadata has been reported yet.',
  }
  return messages[key] || 'The active capture and encoder path is mixed or not fully classified.'
}

function modeLabelFromBool(value) {
  return value ? 'Private Stream' : 'Private Stream (windowed)'
}

function humanizeStreamPathId(id) {
  const map = {
    headless_stream: 'Private Stream',
    windowed_stream: 'Private Stream (GPU-native)',
    host_virtual_display: 'Host Virtual Display',
    desktop_takeover: 'Desktop Takeover',
    desktop_display: 'Mirror Desktop',
    gamescope_stream: 'Gamescope Stream',
    family_isolated: 'Family Mode (isolated)',
    headless_evdi: 'Headless EVDI',
    headless_dongle: 'Headless Dongle',
  }
  return map[id] || titleizeToken(id || '')
}

function streamDisplayModeLabel(statsPayload) {
  // Prefer human label from the host; fall back to path id mapping.
  const label = statsPayload?.stream_display_mode
  if (label && !String(label).includes('_')) return label
  const pathId = statsPayload?.stream_display_mode_id || statsPayload?.stream_path_id || label
  if (pathId) return humanizeStreamPathId(pathId) || modeLabelFromBool(Boolean(statsPayload?.runtime_effective_headless))
  return modeLabelFromBool(Boolean(statsPayload?.runtime_effective_headless))
}



const runtimeEffectiveMode = computed(() => {
  if (!stats.value?.streaming) {
    // Idle: still show configured path when stats expose it.
    const idleLabel = streamDisplayModeLabel(stats.value)
    return idleLabel && idleLabel !== 'Private Stream (windowed)' ? idleLabel : '--'
  }
  return streamDisplayModeLabel(stats.value)
})

const nestedLabwcShmFallbackActive = computed(() => {
  if (!stats.value?.streaming) return false

  const backend = String(stats.value?.runtime_backend || '').toLowerCase()
  const transport = String(stats.value?.capture_transport || '').toLowerCase()

  return backend === 'labwc' &&
    !Boolean(stats.value?.runtime_effective_headless) &&
    transport === 'shm'
})



const capturePathLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return titleizeToken(stats.value?.capture_path || 'unknown')
})

const captureGpuNativeTone = computed(() => {
  const tone = captureStatusTone(stats.value)
  return tone === 'warning' ? 'text-warning' : tone === 'info' ? 'text-info-bright' : 'text-storm'
})

const runtimePathNote = computed(() => {
  if (!stats.value?.streaming) return ''
  const reason = String(stats.value?.capture_path_reason || '').toLowerCase()

  if (nestedLabwcShmFallbackActive.value) {
    return stats.value?.runtime_gpu_native_override_active
      ? 'GPU-native capture was requested, but the nested labwc compositor is using SHM/system-memory capture.'
      : 'The nested labwc compositor is using SHM/system-memory capture. This can be a healthy stream path.'
  }

  if (reason === 'headless_shm_fallback' || reason === 'headless_shm_default') {
    return captureReasonMessage(reason)
  }

  if (reason === 'headless_extcopy_dmabuf') {
    return captureReasonMessage(reason)
  }

  if (stats.value?.capture_cpu_copy) {
    return captureReasonMessage(reason)
  }

  if (stats.value?.capture_gpu_native) {
    return captureReasonMessage(reason)
  }

  if (stats.value?.runtime_gpu_native_override_active) {
    return 'GPU-native capture preference forced a visible compositor path for the active encoder.'
  }

  return ''
})

const runtimePathNoteTone = computed(() => captureGpuNativeTone.value)

const liveSessionTitle = computed(() => (
  connectedClients.value.length > 1
    ? t('dashboard.live_session_multi', { count: connectedClients.value.length })
    : t('dashboard.live_session_single', { client: currentClientName.value })
))


const liveSummary = computed(() => {
  return buildLiveSummary({
    stats: stats.value || {},
    qualityGrade: qualityGrade.value,
    qualityScore: qualityScore.value,
    gradeTone: gradeColor(qualityGrade.value),
  })
})

// Check if a specific client name has AI-optimized settings (for multi-viewer list)
function isClientAiOptimized(clientName) {
  return aiCacheKeys.value.some(key => key.startsWith(clientName + ':'))
}

// Nova or Moonlight / Artemis, as the Devices page names it, so a stream says which client is
// playing and therefore what it can use. The stream's own kind comes first: it is read from that
// device's pairing record, so two devices that share a name cannot hide it.
function liveClientFamily(client) {
  return streamClientFamilyLabel(client?.client_family) ||
    liveClientFamilyLabel(client?.name, pairedClientList.value)
}

function gradeColor(grade) {
  if (grade === 'A') return 'text-success'
  if (grade === 'B') return 'text-ice'
  if (grade === 'C') return 'text-warning'
  if (grade === 'D') return 'text-warning'
  return 'text-danger'
}

function formatSessionDate(ts) {
  const d = new Date(ts)
  const now = new Date()
  if (d.toDateString() === now.toDateString()) {
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
  }
  return d.toLocaleDateString([], { month: 'short', day: 'numeric' }) + ' ' +
         d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
}

// Display preview (polling screenshot endpoint)
const PREVIEW_REFRESH_MS = 2000
const PREVIEW_FAILURE_BACKOFF_MS = 15000
const PREVIEW_MAX_BACKOFF_MS = 60000
const streamingOutput = ref('')
const showPreview = ref(false)
const previewExpanded = ref(false)
const previewLoaded = ref(false)
const previewError = ref(false)
const previewUrl = ref('')
const previewBackoffMs = ref(PREVIEW_REFRESH_MS)
// 'mjpeg' renders the host's multipart stream in the img tag (a genuinely
// live preview, no polling); 'poll' is the JPEG-refresh fallback, also used
// under reduced motion where a self-animating stream is unwanted.
const previewMode = ref('mjpeg')
let previewTimer = null
let previewAttempt = 0
const previewStarting = ref(false)

// Snapshot fields reset and merge together; observe the completed update.
watch(spacesPreviewBlocked, blocked => { if (blocked) stopPreview() })

async function startPreview() {
  if (previewStarting.value || spacesPreviewBlocked.value) return
  const attempt = ++previewAttempt
  previewStarting.value = true
  // Recheck immediately before opening a Linux desktop image. The periodic
  // snapshot also retires an existing preview when a Space becomes active.
  const verified = hostPlatform.value !== 'linux' || await loadSpaces()
  if (attempt !== previewAttempt) return
  previewStarting.value = false
  if (!verified || spacesPreviewBlocked.value) return
  previewLoaded.value = false
  previewError.value = false
  previewBackoffMs.value = PREVIEW_REFRESH_MS
  previewMode.value = prefersReducedMotion.value ? 'poll' : 'mjpeg'
  showPreview.value = true
  refreshPreview()
}

function schedulePreviewRefresh(delayMs) {
  if (previewTimer) {
    clearTimeout(previewTimer)
    previewTimer = null
  }
  if (!showPreview.value || spacesPreviewBlocked.value) return
  previewTimer = setTimeout(refreshPreview, delayMs)
}

function refreshPreview() {
  if (!showPreview.value || spacesPreviewBlocked.value || document.hidden) return
  if (previewTimer) {
    clearTimeout(previewTimer)
    previewTimer = null
  }
  previewError.value = false
  // When streaming, crop to the streaming output; otherwise show full display
  const output = streamingOutput.value ? `&output=${encodeURIComponent(streamingOutput.value)}` : ''
  if (previewMode.value === 'mjpeg') {
    // fps=2 keeps the host's capture loop light (it defaults to 5), and the
    // 30 s re-issue is a staleness watchdog: a cleanly ended multipart stream
    // fires neither load nor error, which would otherwise freeze the frame.
    previewUrl.value = `./api/display/stream?fps=2&t=${Date.now()}${output}`
    // Chromium may never fire img load for multipart streams; clear the
    // spinner once frames have had time to arrive unless an error landed.
    previewTimer = setTimeout(() => {
      if (previewMode.value === 'mjpeg' && showPreview.value && !previewError.value && !document.hidden) {
        previewLoaded.value = true
        schedulePreviewRefresh(30000)
      }
    }, 1500)
  } else {
    previewUrl.value = `./api/display/screenshot?t=${Date.now()}${output}`
  }
}

function handlePreviewLoad() {
  if (!showPreview.value || spacesPreviewBlocked.value || document.hidden) return
  previewLoaded.value = true
  previewError.value = false
  previewBackoffMs.value = PREVIEW_REFRESH_MS
  // The MJPEG stream keeps updating the img on its own; only polling refreshes.
  if (previewMode.value !== 'mjpeg') {
    schedulePreviewRefresh(PREVIEW_REFRESH_MS)
  }
}

function handlePreviewError() {
  if (!showPreview.value || spacesPreviewBlocked.value || document.hidden) return
  previewLoaded.value = false
  if (previewMode.value === 'mjpeg') {
    // Host without the stream endpoint (or a dropped stream): fall back to
    // JPEG polling instead of surfacing an error.
    previewMode.value = 'poll'
    refreshPreview()
    return
  }
  previewError.value = true
  previewBackoffMs.value = Math.min(
    Math.max(PREVIEW_FAILURE_BACKOFF_MS, previewBackoffMs.value * 2),
    PREVIEW_MAX_BACKOFF_MS,
  )
  schedulePreviewRefresh(previewBackoffMs.value)
}

function retryPreviewNow() {
  previewBackoffMs.value = PREVIEW_REFRESH_MS
  previewMode.value = prefersReducedMotion.value ? 'poll' : 'mjpeg'
  refreshPreview()
}

// A backgrounded tab must not keep the host capturing for an MJPEG stream
// nobody is watching; pause the stream and resume on return.
function handlePreviewVisibility() {
  if (!showPreview.value) return
  if (document.hidden) {
    if (previewTimer) { clearTimeout(previewTimer); previewTimer = null }
    previewUrl.value = ''
    previewLoaded.value = false
  } else {
    refreshPreview()
  }
}

function togglePreviewExpanded() {
  if (!showPreview.value) {
    startPreview()
  }
  previewExpanded.value = !previewExpanded.value
}

// Showing or hiding the preview swaps the button the viewer pressed, so focus moves to its
// counterpart instead of falling back to the page.
async function showPreviewFromStage() {
  await startPreview()
  if (!showPreview.value) return
  await nextTick()
  document.querySelector('[data-stop-preview]')?.focus({ preventScroll: true })
}

async function hidePreviewFromStage() {
  stopPreview()
  await nextTick()
  document.querySelector('[data-start-preview]')?.focus({ preventScroll: true })
}

function stopPreview() {
  ++previewAttempt
  previewStarting.value = false
  previewUrl.value = ''
  showPreview.value = false
  previewExpanded.value = false
  previewLoaded.value = false
  previewError.value = false
  previewBackoffMs.value = PREVIEW_REFRESH_MS
  if (previewTimer) { clearTimeout(previewTimer); previewTimer = null }
}

const qualityScore = computed(() => buildQualityScore(stats.value || {}))

const qualityGrade = computed(() => buildQualityGrade(qualityScore.value))


const previewStatusText = computed(() => {
  if (previewError.value) return t('dashboard.preview_unavailable_status')
  if (!previewLoaded.value) return t('dashboard.preview_capturing')
  return previewMode.value === 'mjpeg'
    ? t('dashboard.preview_status_live')
    : t('dashboard.preview_status')
})



// Frame health: dropped/duplicate ratios and mean target-interval error arrive
// on every SSE tick. The interval metric is not statistical network jitter.
const frameHealth = computed(() => {
  const s = stats.value || {}
  const dropped = Number(s.dropped_frame_ratio)
  const duplicate = Number(s.duplicate_frame_ratio)
  const intervalError = Number(s.frame_interval_error_ms ?? s.frame_jitter_ms)
  const pct = (v) => (Number.isFinite(v) ? `${(v * 100).toFixed(1)}%` : '--')
  return {
    dropped: pct(dropped),
    droppedTone: Number.isFinite(dropped) && dropped > 0.02 ? 'text-danger' : Number.isFinite(dropped) && dropped > 0.005 ? 'text-warning' : '',
    duplicate: pct(duplicate),
    duplicateTone: Number.isFinite(duplicate) && duplicate > 0.05 ? 'text-warning' : '',
    intervalError: Number.isFinite(intervalError) ? `${intervalError.toFixed(1)} ms` : '--',
    intervalErrorTone: Number.isFinite(intervalError) && intervalError > 4 ? 'text-warning' : '',
  }
})

// Host-side session log rows, the fallback history source when this browser
// has no local session records yet.
const hostHistoryRows = computed(() => (Array.isArray(sessionHistory.value) ? sessionHistory.value.slice(0, 8) : []))

// Paired-client chips with last-seen times (the host records last_seen_at
// per certificate; epoch seconds, tolerating milliseconds).
const pairedClientList = ref([])

function relativeSeen(value) {
  const raw = Number(value)
  if (!Number.isFinite(raw) || raw <= 0) return ''
  const ms = raw > 1e12 ? raw : raw * 1000
  const deltaS = Math.max(0, (Date.now() - ms) / 1000)
  if (deltaS < 90) return 'seen just now'
  if (deltaS < 5400) return `seen ${Math.round(deltaS / 60)} min ago`
  if (deltaS < 129600) return `seen ${Math.round(deltaS / 3600)} h ago`
  return `seen ${Math.round(deltaS / 86400)} d ago`
}


// Doctor "Explain": same AI flow Troubleshooting uses, loaded on demand so
// the initial bundle stays inside budget.
const doctorExplanation = ref('')
const doctorExplainPending = ref(false)

async function explainDoctorVerdict() {
  if (doctorExplainPending.value) return
  doctorExplainPending.value = true
  doctorExplanation.value = ''
  try {
    const { explainDoctorWithAi } = await import('../ai-doctor-explanation.js')
    const configRes = await fetch('./api/config', { credentials: 'include' })
    const config = (await readConfigOrNull(configRes)) || {}
    const result = await explainDoctorWithAi({
      aiEnabled: config.ai_enabled === true || config.ai_enabled === 'enabled' || config.ai_enabled === 'true',
      config,
      supportBundle: { config, stream_stats: stats.value || {}, deterministic_summary: doctor.value || {} },
      deterministicSummary: doctor.value || {},
    })
    const explanation = result.explanation || {}
    const text = [explanation.likely_cause, (explanation.try_first || [])[0]].filter(Boolean).join(' ')
    if (text) {
      doctorExplanation.value = text
    } else {
      showToast(result.error || t('dashboard.doctor_explain_unavailable'), 'info')
    }
  } catch (e) {
    showToast(t('dashboard.doctor_explain_unavailable') + ` (${e.message})`, 'error')
  } finally {
    doctorExplainPending.value = false
  }
}

// ── Doctor: the host's deterministic per-second diagnosis (SSE `doctor`) ──
const doctor = computed(() => stats.value?.doctor || null)

const doctorHeadline = computed(() => {
  const d = doctor.value
  if (!d) return t('dashboard.doctor_all_clear')
  if (d.traffic_light === 'green' || !d.primary_issue || d.primary_issue === 'none') {
    return d.summary || t('dashboard.doctor_all_clear')
  }
  return d.summary || d.primary_issue
})

// The host emits recommendation as {title, body, ...}, not a string.
const doctorRecommendation = computed(() => {
  const rec = doctor.value?.recommendation
  if (!rec) return ''
  if (typeof rec === 'string') return rec
  return [rec.title, rec.body].filter(Boolean).join('. ')
})

const doctorPanelClass = computed(() => {
  if (doctorIsWaitingForStream(doctor.value, stats.value)) return 'border-l-storm/60 bg-void/20'
  // Host contract: traffic_light is green | amber | red (never yellow).
  // The tone paints the verdict stripe on the card left edge.
  switch (doctor.value?.traffic_light) {
    case 'red': return 'border-l-danger bg-danger/5'
    case 'amber':
    case 'yellow': return 'border-l-warning bg-warning/5'
    case 'green': return 'border-l-success bg-success/5'
    default: return 'border-l-storm/60 bg-void/20'
  }
})

const doctorLightClass = computed(() => {
  if (doctorIsWaitingForStream(doctor.value, stats.value)) return 'bg-storm/60'
  switch (doctor.value?.traffic_light) {
    case 'red': return 'bg-danger'
    case 'amber':
    case 'yellow': return 'bg-warning'
    case 'green': return 'bg-success'
    default: return 'bg-storm/60'
  }
})

const doctorConfidenceLabel = computed(() => {
  const level = doctor.value?.confidence?.level
  return level ? t('dashboard.doctor_confidence', { level }) : ''
})

// The host describes a safe recovery action. Every executable action
// targets this web server's own /api/doctor/action route; the /api/ gate
// below is a defensive filter so a host emitting an endpoint this web
// build cannot reach (version skew) degrades to advice, not a dead button.
const doctorSafeAction = computed(() => {
  // Endpoint-less actions (export diagnostics, safer-next-launch) are real
  // host advice and render as advisory pills; only /api/ ones can execute.
  const action = doctor.value?.safe_recovery_action
  if (!action || action.kind === 'none' || !action.label) return null
  return action
})

const doctorActionExecutable = computed(() => {
  const action = doctorSafeAction.value
  const payload = action?.payload_preview || action?.payload || {}
  return Boolean(
    action?.endpoint?.startsWith('/api/') &&
    typeof payload.app_session_id === 'string' && payload.app_session_id.length > 0 &&
    Number.isSafeInteger(Number(payload.session_generation)) && Number(payload.session_generation) > 0
  )
})

const doctorActionConfirmOpen = ref(false)
const doctorActionPending = ref(false)
const doctorActionResult = ref(null)
let doctorVerificationTimer = null

const doctorActionUndoAvailable = computed(() => Boolean(
  doctorActionResult.value?.undo?.available &&
  doctorActionResult.value?.state !== 'undone'
))

const doctorActionStatus = computed(() => {
  const result = doctorActionResult.value
  if (!result) return ''
  if (result.message) return result.message
  switch (result.state) {
    case 'watching': return t('dashboard.doctor_action_watching')
    case 'resolved': return t('dashboard.doctor_action_resolved')
    case 'stable': return t('dashboard.doctor_action_stable')
    case 'confirmed_pressure': return t('dashboard.doctor_action_confirmed')
    case 'needs_attention': return t('dashboard.doctor_action_needs_attention')
    case 'rollback_unconfirmed': return t('dashboard.doctor_action_rollback_unconfirmed')
    case 'undone': return t('dashboard.doctor_action_undone')
    default: return ''
  }
})

async function postDoctorAction(payload) {
  const response = await fetch('./api/doctor/action', {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify(payload),
  })
  const result = await response.json().catch(() => ({}))
  return resolveDoctorActionHttpResponse(response, result)
}

function requestDoctorSafeAction() {
  if (doctorSafeAction.value?.requires_confirmation) {
    doctorActionConfirmOpen.value = true
    return
  }
  runDoctorSafeAction()
}

async function verifyDoctorAction(verification) {
  if (!verification?.run_id) return
  try {
    const result = await postDoctorAction({
      action_id: verification.action_id || 'verify',
      run_id: verification.run_id,
      app_session_id: verification.app_session_id,
      session_generation: verification.session_generation,
    })
    doctorActionResult.value = result
    if (result.state === 'resolved') {
      showToast(t('dashboard.doctor_action_resolved'), 'success')
    } else if (result.state === 'needs_attention') {
      showToast(t('dashboard.doctor_action_needs_attention'), 'warning')
    } else if (result.state === 'rollback_unconfirmed') {
      showToast(t('dashboard.doctor_action_rollback_unconfirmed'), 'warning')
    }
    if (doctorVerificationTimer) clearTimeout(doctorVerificationTimer)
    if (result.verification?.run_id) {
      const delayMs = Math.max(1, Number(result.verification.delay_seconds) || 8) * 1000
      doctorVerificationTimer = setTimeout(() => verifyDoctorAction(result.verification), delayMs)
    }
  } catch (e) {
    showToast(t('dashboard.doctor_action_error') + e.message, 'error')
  }
}

async function runDoctorSafeAction() {
  const action = doctorSafeAction.value
  doctorActionConfirmOpen.value = false
  if (!action || doctorActionPending.value) return
  doctorActionPending.value = true
  try {
    const payload = { ...(action.payload_preview || action.payload || {}) }
    if (payload.action_id === 'lower_bitrate' || payload.action_id === 'restore_quality') {
      const randomId = globalThis.crypto?.randomUUID?.()
      payload.request_id = randomId
        ? `dashboard-${randomId}`
        : `dashboard-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`
    }
    const result = await postDoctorAction(payload)
    doctorActionResult.value = result
    if (result.state === 'rollback_unconfirmed') {
      showToast(t('dashboard.doctor_action_rollback_unconfirmed'), 'warning')
    } else {
      showToast(result.message || t('dashboard.doctor_action_success') + action.label, 'success')
    }
    if (doctorVerificationTimer) clearTimeout(doctorVerificationTimer)
    if (result.verification?.run_id) {
      const delayMs = Math.max(1, Number(result.verification.delay_seconds) || 8) * 1000
      doctorVerificationTimer = setTimeout(() => verifyDoctorAction(result.verification), delayMs)
    }
  } catch (e) {
    showToast(t('dashboard.doctor_action_error') + e.message, 'error')
  } finally {
    doctorActionPending.value = false
  }
}

async function undoDoctorAction() {
  const undo = doctorActionResult.value?.undo
  if (!undo?.run_id || doctorActionPending.value) return
  doctorActionPending.value = true
  if (doctorVerificationTimer) clearTimeout(doctorVerificationTimer)
  try {
    const result = await postDoctorAction({
      action_id: undo.action_id || 'undo',
      run_id: undo.run_id,
      app_session_id: undo.app_session_id,
      session_generation: undo.session_generation,
    })
    doctorActionResult.value = result
    if (result.state === 'rollback_unconfirmed') {
      showToast(t('dashboard.doctor_action_rollback_unconfirmed'), 'warning')
    } else {
      showToast(t('dashboard.doctor_action_undone'), 'success')
    }
  } catch (e) {
    showToast(t('dashboard.doctor_action_error') + e.message, 'error')
  } finally {
    doctorActionPending.value = false
  }
}

// Launch a recent game straight from the landing page (same endpoint the
// Library uses). Launching is the page's primary action, not destructive.
const launchingUuid = ref('')
// Opened in Nova's Host Console, Play hands the title to Nova, which streams it to the device in
// the player's hands; in a browser the host starts it.
const inNova = embeddedClient() === 'nova'
const hostUuid = ref('')
const hostName = ref('')
// Nova takes a launch only for the host it knows; without this host's id, Play waits rather than
// starting the title on the host behind the player's back.
const novaLaunchWaiting = computed(() => inNova && !hostUuid.value)

async function launchRecentApp(app) {
  if (launchingUuid.value || !currentAppAvailable.value) return
  if (inNova) {
    if (hostUuid.value) followClientLink(artLaunchHref({ hostUuid: hostUuid.value, hostName: hostName.value, app }))
    return
  }
  launchingUuid.value = app.uuid
  try {
    const response = await fetch('./api/apps/launch', {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify({ uuid: app.uuid }),
    })
    const result = response.ok ? await response.json() : { status: false, error: `HTTP ${response.status}` }
    if (result.status === true) {
      showToast(t('dashboard.launched', { name: app.name }), 'success')
      // The stage moves once, to the title that just launched.
      selectedFeaturedUuid.value = ''
      refreshApps()
    } else {
      showToast(t('dashboard.launch_failed') + (result.error || ''), 'error')
    }
  } catch (e) {
    showToast(t('dashboard.launch_failed') + e.message, 'error')
  } finally {
    launchingUuid.value = ''
  }
}

// Connected client disconnect
const connectedClientUuid = ref(null)
// The client the UUID was resolved for, so the confirmation names the device it will disconnect.
const connectedClientName = ref('')
const disconnectConfirmOpen = ref(false)
const clearHistoryConfirmOpen = ref(false)
const clearingHistory = ref(false)

// One action for both history sources: the host's recorded outcomes through
// POST /api/ai/history/clear and this browser's local list.
async function clearAllSessionHistory() {
  clearingHistory.value = true
  try {
    const response = await fetch('./api/ai/history/clear', {
      method: 'POST',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
    })
    const result = await response.json().catch(() => ({}))
    if (!response.ok || result.status === false) throw new Error(result.error || `HTTP ${response.status}`)
    clearHistory()
    sessionHistory.value = []
    clearHistoryConfirmOpen.value = false
    showToast(t('dashboard.clear_history_success'), 'success')
  } catch (error) {
    console.error(error)
    showToast(`${t('dashboard.clear_history_error')} ${error.message || ''}`.trim(), 'error')
  } finally {
    clearingHistory.value = false
  }
}
const disconnectingClient = ref(false)
const disconnectClientImpactItems = computed(() => [
  t('dashboard.disconnect_client_impact_stream'),
  t('dashboard.disconnect_client_impact_reconnect'),
])

// The stream and the client it names. Disconnect belongs to exactly this pair.
const streamClientKey = computed(() => (stats.value?.streaming
  ? `${stats.value.stream_instance_id || ''}\u0000${typeof stats.value.client_name === 'string' ? stats.value.client_name : ''}`
  : ''))
let clientResolveRevision = 0

// GET /api/clients/list answers {named_certs, platform, status}. The stream names its client, and
// the stats payload carries no pairing UUID, so the name must match exactly one valid pairing
// record. Two devices sharing a name, a missing name or a malformed record select nothing, and a
// reply that arrives after the stream or its client changed is discarded rather than retargeted.
async function resolveConnectedClient() {
  const revision = ++clientResolveRevision
  const key = streamClientKey.value
  const name = stats.value?.client_name
  connectedClientUuid.value = null
  connectedClientName.value = ''
  if (!key || typeof name !== 'string' || !name) return
  try {
    const res = await fetch('./api/clients/list', { credentials: 'include' })
    if (!res.ok) return
    const data = await res.json()
    if (revision !== clientResolveRevision || key !== streamClientKey.value) return
    const certs = Array.isArray(data?.named_certs) ? data.named_certs : []
    // Count every record with the name first: a malformed twin is still ambiguity, not a filter.
    const named = certs.filter(cert => cert && typeof cert === 'object' && cert.name === name)
    if (named.length !== 1) return
    const uuid = named[0].uuid
    if (typeof uuid !== 'string' || !uuid.trim()) return
    connectedClientUuid.value = uuid
    connectedClientName.value = name
  } catch {}
}

// Immediate: the stats channel is shared, so this page can open on a stream that is already live
// and whose key never changes afterwards.
watch(streamClientKey, (key) => {
  if (key) {
    resolveConnectedClient()
  } else {
    ++clientResolveRevision
    connectedClientUuid.value = null
    connectedClientName.value = ''
  }
}, { immediate: true })

// A confirmation left open for a client that is no longer the resolved one closes untouched.
watch(connectedClientUuid, (uuid) => {
  if (!uuid && disconnectConfirmOpen.value && !disconnectingClient.value) disconnectConfirmOpen.value = false
})

async function disconnectClient() {
  if (!connectedClientUuid.value) return
  disconnectingClient.value = true
  try {
    const response = await fetch('./api/clients/disconnect', {
      method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ uuid: connectedClientUuid.value })
    })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    connectedClientUuid.value = null
    connectedClientName.value = ''
    disconnectConfirmOpen.value = false
    showToast(t('dashboard.disconnect_client_success'), 'success')
  } catch (e) {
    console.error(e)
    showToast(t('dashboard.disconnect_client_error'), 'error')
  } finally {
    disconnectingClient.value = false
  }
}

function formatAudioName(sink) {
  // "alsa_output.usb-Topping_DX5_II-00.pro-output-0" → "Topping DX5 II"
  const m = sink.match(/usb-(.+?)-\d+\./)
  if (m) return m[1].replace(/_/g, ' ')
  // "alsa_output.pci-0000_01_00.1.hdmi-stereo" → "HDMI Stereo"
  const h = sink.match(/\.([^.]+)$/)
  if (h) return h[1].replace(/-/g, ' ').replace(/\b\w/g, c => c.toUpperCase())
  return sink.length > 30 ? sink.substring(0, 30) + '...' : sink
}


// Dynamic favicon
useFavicon(stats)

const prefersReducedMotion = ref(false)
let reducedMotionQuery = null

// The signal strip owns its sparklines and drops them when this flips.
function updateReducedMotionPreference(event) {
  prefersReducedMotion.value = Boolean(event?.matches)
}

// Track whether we've gotten at least one stats response
const statsLoaded = ref(false)

// Fetch the config-derived state the ready checks and sync badge depend on.
async function fetchSystemInfo() {
  try {
    const configRes = await fetch('./api/config', { credentials: 'include' })
    const config = await readConfigOrNull(configRes)
    if (config) {
      refreshClientSettingsSync(config)
      streamingOutput.value = previewOutputForConfig(config)
      discoveryEnabled.value = config.enable_discovery !== 'disabled'
      pairingEnabled.value = config.enable_pairing !== 'disabled'
    }
  } catch (e) {
    // Non-critical, just leave defaults
  }
}

// Rolling data (60 seconds) for the strip's sparklines.
const MAX_POINTS = 60
const timestamps = ref([])
const qualityHistory = ref([])
const fpsHistory = ref([])
const bitrateHistory = ref([])
const encodeHistory = ref([])
const latencyHistory = ref([])
const lossHistory = ref([])
const historyRevision = ref(0)
const chartHistory = {
  get timestamps() { return timestamps.value },
  series: {
    get quality() { return qualityHistory.value },
    get fps() { return fpsHistory.value },
    get bitrate() { return bitrateHistory.value },
    get encode() { return encodeHistory.value },
    get latency() { return latencyHistory.value },
    get loss() { return lossHistory.value },
  },
}

function destroyCharts() {
  timestamps.value = []
  qualityHistory.value = []
  fpsHistory.value = []
  bitrateHistory.value = []
  encodeHistory.value = []
  latencyHistory.value = []
  lossHistory.value = []
  historyRevision.value++
}

// ── Stream status freshness ──
// useStreamStats reconnects on its own. The page calls the status unavailable only after the
// channel has been disconnected for a grace period, including before any first report, and it
// quotes the age of the last report only when one actually arrived.
const nowMs = ref(Date.now())
let statusClock = null
const disconnectedSince = ref(Date.now())
const lastReportAt = ref(0)
watch(() => streamStatsConnected?.value, (value) => {
  if (value === false) {
    if (!disconnectedSince.value) disconnectedSince.value = Date.now()
  } else {
    disconnectedSince.value = 0
  }
}, { immediate: true })
watch(() => streamStatsReceivedAt?.value, (value) => { if (value > 0) lastReportAt.value = value }, { immediate: true })
const streamStatusIsUnavailable = computed(() => streamStatusUnavailable({
  connected: streamStatsConnected?.value, disconnectedSince: disconnectedSince.value, now: nowMs.value,
}))
const reportAgeLabel = computed(() => (lastReportAt.value > 0 ? formatReportAge(nowMs.value - lastReportAt.value) : ''))
// A tab returning from the background reconnects first; give it the same grace as any other gap.
function handleStatusVisibility() {
  if (!document.hidden && disconnectedSince.value) disconnectedSince.value = Date.now()
}

const stageMode = computed(() => missionStageMode({
  unavailable: streamStatusIsUnavailable.value,
  statsLoaded: statsLoaded.value,
  streaming: Boolean(stats.value?.streaming),
  spacesActive: dashboardSpaces.value.hasActivity,
}))

// ── Director greeting ──
const activeTheme = ref(typeof document !== 'undefined' ? document.documentElement.getAttribute('data-theme') || 'polaris' : 'polaris')
let unsubscribeThemeChange = null
const directorGreeting = computed(() => {
  if (activeTheme.value !== 'director') return ''
  // The exact theme name, "< Congratulations, Director >"; the brackets are drawn as styled text.
  const label = getThemeMeta('director').label || ''
  return label.replace(/^<\s*/, '').replace(/\s*>$/, '')
})

// ── Stage content ──
const hostDisplayName = computed(() => String(readinessConfig.value?.sunshine_name || '').trim() || t('mission_control.this_host'))
// A poster picked from the shelf takes the stage until a launch, a refresh without it, or a stream.
const selectedFeaturedUuid = ref('')
const featuredApp = computed(() => featuredLibraryApp(recentApps.value, selectedFeaturedUuid.value))
const missionStage = ref(null)
watch(stageMode, mode => { if (mode !== 'idle') selectedFeaturedUuid.value = '' })

// Play launches only a press that began on it while it was shown. A tap on a hover-capable screen
// reveals Play as it lands, so its click can arrive on a Play that was hidden and took no hit when
// the finger went down; that tap meant the poster, so it features the title. Keyboard activation
// (detail 0) has no press and launches.
let playPress = ''
function notePlayPress(event) {
  playPress = event.target?.closest?.('[data-dashboard-launch-app]')?.getAttribute('data-dashboard-launch-app') || ''
}
function playClicked(app, event) {
  const pressed = playPress === app.uuid
  playPress = ''
  if (event?.detail && !pressed) featureApp(app)
  else launchRecentApp(app)
}

// Featuring never moves focus; it only brings the stage back into view when it has scrolled away.
function featureApp(app) {
  selectedFeaturedUuid.value = app.uuid === recentApps.value[0]?.uuid ? '' : app.uuid
  const stage = missionStage.value?.$el
  if (stage?.getBoundingClientRect?.().top < 0) {
    stage.scrollIntoView?.({ block: 'start', behavior: prefersReducedMotion.value ? 'auto' : 'smooth' })
  }
}
const streamFormat = computed(() => streamFormatLabel(stats.value || {}))

const liveCells = computed(() => liveSignalCells({
  stats: stats.value || {}, summary: liveSummary.value, grade: qualityGrade.value, score: qualityScore.value, t,
}))
const unavailableCells = computed(() => unavailableSignalCells({ t }))
// The mode new sessions start in: the configured stream path on Linux, Private Stream or Windowed
// elsewhere. Nothing is claimed until the host's settings have been read.
const newSessionModeLabel = computed(() => {
  if (readinessSources.value.config !== 'ready' || !readinessConfig.value) return ''
  if (readinessConfig.value.platform === 'linux') return humanizeStreamPathId(resolveStreamDisplayMode(readinessConfig.value))
  return headlessEnabled.value ? t('dashboard.headless') : t('dashboard.windowed')
})

const idleCells = computed(() => {
  const last = sessions.value[0]
  return idleSignalCells({
    readiness: {
      label: readinessLabel.value, tone: readinessTone.value,
      passing: readyCheckDisplay.value.passing, total: readyCheckDisplay.value.total,
    },
    clientsKnown: readinessSources.value.clients === 'ready',
    pairedClients: pairedClients.value,
    lastSession: last ? {
      duration: Number.isFinite(last.duration_s) ? formatDuration(last.duration_s) : '',
      grade: last.quality_grade || '', tone: last.quality_grade ? gradeColor(last.quality_grade) : '',
      detail: [last.client_name, last.started_at ? formatSessionDate(last.started_at) : ''].filter(Boolean).join(' · '),
    } : null,
    modeLabel: newSessionModeLabel.value,
    gpu: gpu.value,
    liveTuningLabel: liveTuningLabel.value,
    t,
  })
})

// Only real numbers are readings; a value the host did not send is left out, never shown as 0.
const shown = value => Number(value.toFixed(1))

function gpuParts(device) {
  if (!device) return []
  const temperature = reading(device.temperature_c)
  const load = reading(device.utilization_pct)
  const encoder = reading(device.encoder_pct)
  return [
    device.name || '',
    temperature === null ? '' : `${shown(temperature)} °C`,
    load === null ? '' : `GPU ${shown(load)}%`,
    encoder === null ? '' : `encoder ${shown(encoder)}%`,
  ].filter(Boolean)
}

const gpuRows = computed(() => {
  const device = gpu.value
  if (!device) return []
  const missing = t('mission_control.not_reported')
  const temperature = reading(device.temperature_c)
  const load = reading(device.utilization_pct)
  const encoder = reading(device.encoder_pct)
  const used = reading(device.vram_used_mb)
  const total = reading(device.vram_total_mb)
  return [
    { label: 'GPU', value: device.name || missing },
    { label: 'Temperature', value: temperature === null ? missing : `${shown(temperature)} °C` },
    { label: 'GPU load', value: load === null ? missing : `${shown(load)}%` },
    { label: 'Encoder', value: encoder === null ? missing : `${shown(encoder)}%` },
    { label: 'VRAM', value: used === null || total === null ? missing : `${(used / 1024).toFixed(1)} / ${(total / 1024).toFixed(1)} GB` },
  ]
})

const liveRouteNodes = computed(() => {
  const s = stats.value || {}
  const host = {
    key: 'host', role: t('mission_control.route_host'), name: hostDisplayName.value,
    details: [
      gpuParts(gpu.value).join(' · '),
      { text: capturePathLabel.value, pill: true, tone: captureGpuNativeTone.value },
      runtimeEffectiveMode.value !== '--' ? runtimeEffectiveMode.value : '',
    ],
  }
  const format = streamFormat.value
  const bitrate = reading(s.bitrate_kbps)
  const stream = {
    key: 'stream', role: t('mission_control.route_stream'), name: format || t('mission_control.not_reported_title'),
    state: format ? '' : 'unknown',
    details: [[dynamicRangeLabel(s), bitrate === null ? '' : `${(bitrate / 1000).toFixed(1)} Mbps`].filter(Boolean).join(' · ')],
  }
  const clients = connectedClients.value.map((client, index) => {
    const latency = reading(client.latency_ms)
    const fps = reading(client.fps)
    const clientBitrate = reading(client.bitrate_kbps)
    return {
      key: `client-${index}`, role: t('mission_control.route_client'), name: client.name || t('dashboard.unknown_client'),
      badge: isClientAiOptimized(client.name) ? 'AI' : '', state: 'live',
      details: [
        [client.ip && client.ip !== '--' ? client.ip : '', liveClientFamily(client)].filter(Boolean).join(' · '),
        [latency === null ? '' : t('mission_control.round_trip_ms', { ms: latency.toFixed(0) }),
          fps ? `${fps.toFixed(0)} fps` : '',
          clientBitrate ? `${(clientBitrate / 1000).toFixed(1)} Mbps` : ''].filter(Boolean).join(' · '),
        [client.codec ? String(client.codec).toUpperCase() : '', client.width ? `${client.width}×${client.height}` : '',
          client.packet_loss_available && Number.isFinite(client.packet_loss) ? `${client.packet_loss.toFixed(1)}% loss` : ''].filter(Boolean).join(' · '),
      ],
    }
  })
  return [host, stream, ...clients]
})

const idleRouteNodes = computed(() => {
  const hostMode = sessionType.value || (readinessSources.value.config === 'ready' ? (headlessEnabled.value ? t('dashboard.headless') : t('dashboard.windowed')) : '')
  const nodes = [{
    key: 'host', role: t('mission_control.route_host'), name: hostDisplayName.value,
    details: [
      [hostMode, systemReadiness.value.displays === 'ready' ? `${displays.value.length} ${displays.value.length === 1 ? 'display' : 'displays'}` : ''].filter(Boolean).join(' · '),
      gpuParts(gpu.value).join(' · '),
      [audio.value?.sink ? formatAudioName(audio.value.sink) : '', version.value ? `v${version.value}` : ''].filter(Boolean).join(' · '),
    ],
  }]
  if (readinessSources.value.clients !== 'ready') {
    nodes.push({ key: 'paired', role: t('mission_control.route_paired'), name: t('mission_control.not_read_title'), state: 'unknown' })
  } else if (!pairedClientList.value.length) {
    nodes.push({ key: 'paired', role: t('mission_control.route_paired'), name: t('mission_control.no_paired'), state: 'unknown' })
  } else {
    for (const cert of pairedClientList.value.slice(0, 3)) {
      const name = cert.name || cert.uuid || ''
      if (!name) continue
      nodes.push({ key: `paired-${cert.uuid || name}`, role: t('mission_control.route_paired'), name,
        details: [[streamClientFamilyLabel(cert.client_family), relativeSeen(cert.last_seen_at)].filter(Boolean).join(' · ')] })
    }
    const extra = pairedClientList.value.length - 3
    if (extra > 0) nodes.push({ key: 'paired-more', role: t('mission_control.route_paired'), name: t('mission_control.paired_more', { count: extra }) })
  }
  return nodes
})

const unavailableRouteNodes = computed(() => [
  { key: 'host', role: t('mission_control.route_host'), name: hostDisplayName.value, state: 'unknown',
    details: [reportAgeLabel.value ? t('mission_control.no_report_for', { age: reportAgeLabel.value }) : t('mission_control.no_report_received'), t('mission_control.stream_status_unknown')] },
  { key: 'stream', role: t('mission_control.route_stream'), name: t('mission_control.not_reported_title'), state: 'unknown' },
  { key: 'client', role: t('mission_control.route_client'), name: t('mission_control.not_reported_title'), state: 'unknown' },
])

const idleNote = computed(() => {
  const display = readyCheckDisplay.value
  if (display.attention > 0) {
    return [t('dashboard.checks_attention', { count: display.attention }),
      display.primaryIssue ? t('dashboard.checks_start_with', { check: display.primaryIssue.label }) : ''].filter(Boolean).join('. ')
  }
  // All clear needs no note: the readiness reading already says so.
  if (display.allPassing) return ''
  return `${t('dashboard.checks_waiting')}. ${t('dashboard.check_waiting')}`
})

const stageView = computed(() => {
  const mode = stageMode.value
  if (mode === 'loading') {
    return { slate: t('mission_control.connecting'), timecode: '', kicker: '', title: t('dashboard.loading_summary'), meta: [], note: t('dashboard.loading_summary'), noteTone: '', artSrc: '', heroSrc: '', logoSrc: '' }
  }
  if (mode === 'unavailable') {
    const age = reportAgeLabel.value
    return {
      slate: t('mission_control.status_unavailable'),
      timecode: age ? t('mission_control.no_report_age', { age }) : t('mission_control.no_report_yet'),
      kicker: t('mission_control.status_unavailable'),
      title: t('mission_control.waiting_for_host'),
      meta: [age ? t('mission_control.last_report_ago', { age }) : t('mission_control.no_report_received'), t('mission_control.retrying')],
      note: t('mission_control.unavailable_note'), noteTone: '', artSrc: '', heroSrc: '', logoSrc: '',
    }
  }
  if (mode === 'live') {
    const app = currentApp.value
    const artSrc = appCoverSrc(app)
    return {
      slate: dashboardSpaces.value.hasActivity ? t('dashboard_spaces.host_telemetry') : t('mission_control.live'),
      timecode: sessionDurationLabel.value,
      kicker: t('mission_control.now_streaming'),
      title: currentAppName.value || liveSessionTitle.value,
      meta: [app?.source && app.source !== 'manual' ? app.source : '', runtimeEffectiveMode.value !== '--' ? runtimeEffectiveMode.value : '', viewerCountLabel.value],
      note: showPreview.value ? '' : [currentAppName.value ? liveSessionTitle.value : '', artSrc ? t('mission_control.art_note') : t('mission_control.capture_note')].filter(Boolean).join(' '),
      noteTone: '', artSrc, heroSrc: appArtworkSrc(app, 'hero'), logoSrc: appArtworkSrc(app, 'logo'),
    }
  }
  const featured = featuredApp.value
  const display = readyCheckDisplay.value
  return {
    slate: t('mission_control.standby'),
    timecode: '',
    kicker: featured ? (featured.chosen || featured.launchedAt ? t('mission_control.next_up') : t('mission_control.from_library')) : t('dashboard.stream_readiness'),
    title: featured ? featured.app.name : t('dashboard.no_games'),
    meta: featured ? [featured.app.source && featured.app.source !== 'manual' ? featured.app.source : '',
      featured.launchedAt ? t('mission_control.last_launched', { when: relativeDay(featured.launchedAt, nowMs.value) }) : ''] : [],
    note: idleNote.value,
    noteTone: display.attention > 0 ? 'text-warning-bright' : '',
    artSrc: appCoverSrc(featured?.app),
    heroSrc: appArtworkSrc(featured?.app, 'hero'),
    logoSrc: appArtworkSrc(featured?.app, 'logo'),
  }
})

// ── Diagnostics disclosure ──
const diagnosticsOpen = ref(false)
const diagnosticsPanel = ref(null)
const diagnosticsSummary = computed(() => {
  const mode = stageMode.value
  if (mode === 'live') {
    return t('mission_control.diag_live_summary', { dropped: frameHealth.value.dropped, duplicate: frameHealth.value.duplicate, interval: frameHealth.value.intervalError })
  }
  if (mode === 'unavailable') {
    return reportAgeLabel.value ? t('mission_control.diag_unavailable_summary', { age: reportAgeLabel.value }) : t('mission_control.no_report_received')
  }
  const count = sessions.value.length || hostHistoryRows.value.length
  return t('mission_control.diag_idle_summary', { passing: readyCheckDisplay.value.passing, total: readyCheckDisplay.value.total, count })
})

// The stage's Session diagnostics action opens the disclosure and moves there. Nothing else in
// Mission Control moves focus: telemetry updates never do.
async function openDiagnostics() {
  diagnosticsOpen.value = true
  await nextTick()
  const toggle = diagnosticsPanel.value?.toggle
  if (!toggle) return
  toggle.scrollIntoView?.({ block: 'start', behavior: prefersReducedMotion.value ? 'auto' : 'smooth' })
  toggle.focus({ preventScroll: true })
}

watch(stats, (newStats, oldStats) => {
  statsLoaded.value = true

  if (!newStats || !newStats.streaming) {
    if (oldStats?.streaming) {
      stopPreview()
    }
    if (timestamps.value.length) destroyCharts()
    connectedClientUuid.value = null
    return
  }

  // Read the running app when streaming starts (the client UUID follows streamClientKey). The
  // display preview stays opt-in: a new stream never starts a capture on its own; Show display does.
  if (newStats.streaming && (!oldStats || !oldStats.streaming)) {
    refreshApps()
  }

  if (prefersReducedMotion.value) {
    if (timestamps.value.length) destroyCharts()
    return
  }

  const now = Date.now() / 1000

  timestamps.value.push(now)
  qualityHistory.value.push(qualityScore.value || 0)
  fpsHistory.value.push(newStats.fps)
  bitrateHistory.value.push(newStats.bitrate_kbps / 1000)
  encodeHistory.value.push(newStats.encode_time_ms)
  latencyHistory.value.push(newStats.latency_ms)
  // The judged figure the tile shows; a gap until the host has judged any.
  lossHistory.value.push(dashboardLossPct(newStats))

  // Keep rolling window
  while (timestamps.value.length > MAX_POINTS) {
    timestamps.value.shift()
    qualityHistory.value.shift()
    fpsHistory.value.shift()
    bitrateHistory.value.shift()
    encodeHistory.value.shift()
    latencyHistory.value.shift()
    lossHistory.value.shift()
  }
  historyRevision.value++
})

onMounted(async () => {
  if (typeof window !== 'undefined' && typeof window.matchMedia === 'function') {
    reducedMotionQuery = window.matchMedia('(prefers-reduced-motion: reduce)')
    updateReducedMotionPreference(reducedMotionQuery)
    reducedMotionQuery.addEventListener?.('change', updateReducedMotionPreference)
  }
  unsubscribeThemeChange = onThemeChange((theme) => { activeTheme.value = theme })
  activeTheme.value = document.documentElement.getAttribute('data-theme') || 'polaris'
  document.addEventListener('visibilitychange', handlePreviewVisibility)
  document.addEventListener('visibilitychange', handleStatusVisibility)
  statusClock = setInterval(() => { nowMs.value = Date.now() }, 1000)

  fetchSystemInfo()
  fetchAiStatus()
  fetchAiDevices()

  // Fetch AI cache to detect optimized clients
  try {
    const res = await fetch('./api/ai/cache', { credentials: 'include' })
    if (res.ok) {
      const data = await res.json()
      aiCacheKeys.value = Array.isArray(data) ? data.map(e => e.key || '') : Object.keys(data || {})
    }
  } catch {}

  // Fetch session quality history
  try {
    const res = await fetch('./api/ai/history', { credentials: 'include' })
    if (res.ok) {
      sessionHistory.value = await res.json()
    }
  } catch {}

  await refreshLaunchChecks()
})

onUnmounted(() => {
  stopPreview()
  document.removeEventListener('visibilitychange', handlePreviewVisibility)
  document.removeEventListener('visibilitychange', handleStatusVisibility)
  if (statusClock) { clearInterval(statusClock); statusClock = null }
  if (doctorVerificationTimer) clearTimeout(doctorVerificationTimer)
  unsubscribeThemeChange?.()
  unsubscribeThemeChange = null
  reducedMotionQuery?.removeEventListener?.('change', updateReducedMotionPreference)
  reducedMotionQuery = null
})
</script>
