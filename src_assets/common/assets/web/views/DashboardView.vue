<template>
  <div class="page-shell operator-console mission-control-view relative">

    <section class="page-header">
      <div class="page-heading">
        <div class="section-title-row">
          <h1 class="page-title">{{ $t('dashboard.title') }}</h1>
        </div>
        <div class="page-subtitle">{{ actionSummary }}</div>
      </div>
      <div class="page-meta" role="status" aria-live="polite" aria-atomic="true">
        <span v-if="stats?.streaming" class="pulse-dot"></span>
        <span class="meta-pill" :class="stats?.streaming ? 'border-success/30 bg-success/10 text-success' : ''">
          {{ stats?.streaming ? $t('dashboard.live') : $t('dashboard.standby') }}
        </span>
      </div>
    </section>

    <!-- Loading skeleton state -->
    <template v-if="!statsLoaded">
      <div class="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <Skeleton type="card" v-for="n in 4" :key="n" />
      </div>
    </template>

    <template v-else-if="stats?.streaming">
      <section class="section-card gradient-border-top gradient-border-top-accent dashboard-live-shell" :class="{ 'is-preview-expanded': showPreview && previewExpanded }">
        <div class="dashboard-live-header">
          <div class="dashboard-live-header-copy">
            <div class="section-kicker">{{ $t('dashboard.live_session') }}</div>
            <div class="section-title-row">
              <h2 class="section-title">{{ liveSessionTitle }}</h2>
              <InfoHint size="sm" :label="$t('dashboard.live_session')">{{ liveSessionSummary }}</InfoHint>
            </div>
          </div>
          <div class="dashboard-live-header-meta">
            <span class="meta-pill">{{ viewerCountLabel }}</span>
            <span class="meta-pill" :class="captureGpuNativeTone">{{ capturePathLabel }}</span>
          </div>
        </div>

        <section class="dashboard-live-summary-grid" role="status" aria-live="polite" aria-atomic="true" aria-label="Live stream telemetry summary">
          <div class="dashboard-live-summary-tile dashboard-live-summary-tile-primary" data-live-summary-metric="Quality">
            <div class="dashboard-live-summary-label">Quality</div>
            <div class="flex items-center gap-2">
              <span class="dashboard-grade-badge" :class="liveSummary.qualityTone">{{ qualityGrade }}</span>
              <span class="dashboard-live-summary-value" :class="liveSummary.qualityTone">{{ qualityScore }}</span>
            </div>
          </div>
          <div class="dashboard-live-summary-tile" data-live-summary-metric="Latency">
            <div class="dashboard-live-summary-label">Latency</div>
            <div class="dashboard-live-summary-value" :class="liveSummary.latencyTone">{{ liveSummary.latency }}</div>
          </div>
          <div class="dashboard-live-summary-tile" data-live-summary-metric="FPS">
            <div class="dashboard-live-summary-label">FPS</div>
            <div class="dashboard-live-summary-value" :class="liveSummary.fpsTone">{{ liveSummary.fps }}</div>
          </div>
          <div class="dashboard-live-summary-tile" data-live-summary-metric="Loss">
            <div class="dashboard-live-summary-label">Loss</div>
            <div class="dashboard-live-summary-value" :class="liveSummary.lossTone">{{ liveSummary.loss }}</div>
          </div>
          <div class="dashboard-live-summary-tile" data-live-summary-metric="Bitrate">
            <div class="dashboard-live-summary-label">Bitrate</div>
            <div class="dashboard-live-summary-value text-silver">{{ liveSummary.bitrate }}</div>
          </div>
        </section>

        <!-- Doctor: the host's own per-second diagnosis replaces the old
             Auto Quality panel, stream-path notices, and priority guidance. -->
        <section class="dashboard-doctor-card" :class="doctorPanelClass" data-dashboard-doctor>
          <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
            <div class="min-w-0">
              <div class="section-kicker">{{ $t('dashboard.doctor') }}</div>
              <div class="mt-2 flex flex-wrap items-center gap-2">
                <span class="inline-block h-2.5 w-2.5 shrink-0 rounded-full" :class="doctorLightClass" aria-hidden="true"></span>
                <h3 class="text-xl font-semibold leading-tight text-silver">{{ doctorHeadline }}</h3>
                <span v-if="doctorConfidenceLabel" class="control-chip">{{ doctorConfidenceLabel }}</span>
                <span class="meta-pill" :class="autoQuality.toneClass">{{ autoQuality.compactLabel }}</span>
                <InfoHint size="sm" label="Auto Quality details">{{ autoQuality.detail }}</InfoHint>
              </div>
              <p v-if="doctorRecommendation" class="mt-2 max-w-3xl text-sm leading-relaxed text-storm">{{ doctorRecommendation }}</p>
              <p v-if="doctorExplanation" class="mt-3 max-w-3xl rounded-lg border border-accent/20 bg-accent/5 px-3 py-2 text-sm leading-relaxed text-silver">
                {{ doctorExplanation }}
              </p>
            </div>
            <div class="flex shrink-0 flex-wrap items-center gap-2 lg:justify-end">
              <button
                v-if="doctorSafeAction && doctorActionExecutable"
                type="button"
                class="focus-ring dashboard-action-button dashboard-action-button-secondary disabled:cursor-wait disabled:opacity-70"
                :disabled="doctorActionPending"
                @click="doctorActionConfirmOpen = true"
              >
                {{ doctorSafeAction.label }}
              </button>
              <span v-else-if="doctorSafeAction" class="data-pill" :title="doctorSafeAction.rollback || ''">
                {{ doctorSafeAction.label }}
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
            </div>
          </div>
        </section>

        <div class="dashboard-live-stage" :class="{ 'is-preview-expanded': showPreview && previewExpanded, 'is-preview-hidden': !showPreview }">
          <div class="dashboard-live-main">
            <section class="dashboard-preview-panel">
              <div class="dashboard-preview-header items-center">
                <div class="flex min-w-0 items-center gap-2">
                  <div class="eyebrow-label">{{ $t('dashboard.display_preview') }}</div>
                  <InfoHint size="sm" :label="$t('dashboard.display_preview')">{{ previewSupportCopy }}</InfoHint>
                </div>
                <div class="dashboard-preview-actions">
                  <button v-if="!showPreview" @click="startPreview" class="focus-ring dashboard-action-button dashboard-action-button-primary">
                    {{ $t('dashboard.show_display') }}
                  </button>
                  <template v-else>
                    <button @click="togglePreviewExpanded" class="focus-ring dashboard-action-button dashboard-action-button-secondary">
                      {{ previewExpanded ? $t('dashboard.collapse_display') : $t('dashboard.expand_display') }}
                    </button>
                    <button @click="stopPreview" class="focus-ring dashboard-action-button dashboard-action-button-ghost">
                      {{ $t('dashboard.hide_display') }}
                    </button>
                  </template>
                </div>
              </div>

              <template v-if="showPreview">
                <div class="dashboard-preview-frame" :class="{ 'has-error': previewError }">
                  <img
                    :src="previewUrl"
                    alt=""
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
                    <span class="text-success">●</span>
                    {{ previewMode === 'mjpeg' ? 'LIVE' : 'PREVIEW' }}
                  </span>
                  <!-- Stream facts ride the stage as overlays instead of chrome around it. -->
                  <div v-if="previewLoaded && !previewError" class="dashboard-preview-hud">
                    <div class="dashboard-preview-meta">
                      <span class="data-pill">{{ stats.width }}×{{ stats.height }}</span>
                      <span class="data-pill">{{ stats.codec?.toUpperCase() || '--' }}</span>
                      <span v-if="hdrChipLabel" class="data-pill text-accent-2" :title="stats.hdr_downgrade_message || ''">{{ hdrChipLabel }}</span>
                      <span class="data-pill">{{ capturePathLabel }}</span>
                    </div>
                    <span class="dashboard-preview-hud-status">{{ previewStatusText }}</span>
                  </div>
                </div>
              </template>
              <div v-else class="flex items-center justify-between gap-3 py-1">
                <span class="text-xs text-storm">{{ $t('dashboard.preview_hidden_title') }}</span>
                <div class="dashboard-preview-meta">
                  <span class="data-pill">{{ viewerCountLabel }}</span>
                  <span class="data-pill">{{ qualitySummaryLabel }}</span>
                </div>
              </div>
            </section>

          </div>

          <div class="dashboard-live-side">
            <section class="surface-subtle p-4 dashboard-context-card">
              <div class="flex items-center justify-between gap-3">
                <div class="eyebrow-label">Session context</div>
                <span class="meta-pill" :class="runtimeModeTone">{{ runtimeEffectiveMode }}</span>
              </div>
              <div v-if="gpu" class="mt-2 font-mono text-[11px] tabular-nums text-storm">
                {{ gpu.name }} · {{ gpu.temperature_c ?? '--' }}°C · {{ gpu.utilization_pct ?? 0 }}% · {{ gpu.encoder_pct ?? 0 }}% enc · {{ gpu.vram_used_mb ? (gpu.vram_used_mb / 1024).toFixed(1) : '--' }}G
              </div>
              <div class="dashboard-context-section">
                <div class="dashboard-context-header">
                  <span>{{ $t('dashboard.connected_clients') }}</span>
                  <button
                    v-if="connectedClientUuid"
                    :disabled="disconnectingClient"
                    @click="disconnectConfirmOpen = true"
                    class="focus-ring dashboard-action-button dashboard-action-button-danger disabled:cursor-wait disabled:opacity-70"
                  >
                    {{ $t('dashboard.disconnect_client') }}
                  </button>
                </div>
                <div class="space-y-2">
                  <div
                    v-for="(client, index) in connectedClients"
                    :key="`${client.name}-${client.ip}-${index}`"
                    class="dashboard-client-row"
                  >
                    <div class="min-w-0">
                      <div class="truncate text-sm font-medium text-silver">
                        {{ client.name }}
                        <span v-if="isClientAiOptimized(client.name)" class="ml-1 inline-flex items-center gap-0.5 rounded-full bg-accent/15 px-1.5 py-0.5 text-[9px] font-medium text-accent">AI</span>
                      </div>
                      <div class="mt-1 text-[11px] text-storm">{{ client.ip || '--' }}</div>
                    </div>
                    <div class="text-right text-[11px] text-storm tabular-nums">
                      <div>{{ client.latency_ms?.toFixed(0) || '--' }} ms</div>
                      <div v-if="client.fps" class="mt-0.5">
                        {{ client.fps.toFixed(0) }} fps<template v-if="client.bitrate_kbps"> · {{ (client.bitrate_kbps / 1000).toFixed(1) }} Mbps</template>
                      </div>
                      <div v-if="client.codec || client.width" class="mt-0.5">
                        <template v-if="client.codec">{{ client.codec.toUpperCase() }}</template><template v-if="client.width"> · {{ client.width }}×{{ client.height }}</template><template v-if="Number.isFinite(client.packet_loss)"> · {{ client.packet_loss.toFixed(1) }}%</template>
                      </div>
                    </div>
                  </div>
                </div>
              </div>
              <div class="dashboard-context-section">
                <div class="dashboard-context-header">
                  <span>{{ $t('dashboard.runtime_path') }}</span>
                  <router-link to="/troubleshooting" class="focus-ring text-[11px] font-semibold text-ice no-underline hover:text-ice/80">
                    {{ $t('dashboard.runtime_detail_link') }}
                  </router-link>
                </div>
                <div class="dashboard-runtime-pill-grid">
                  <div class="dashboard-runtime-pill">
                    <span class="dashboard-runtime-label">{{ $t('dashboard.capture') }}</span>
                    <span class="text-sm font-medium" :class="captureGpuNativeTone">{{ capturePathLabel }}</span>
                  </div>
                </div>
                <div v-if="runtimePathNote" class="dashboard-rail-footnote" :class="runtimePathNoteTone">
                  {{ runtimePathNote }}
                </div>
              </div>
            </section>

            <section class="card p-4">
              <QuickControls compact @change="handleQuickControlChange" />
            </section>
          </div>
        </div>

        <details class="dashboard-secondary-group" open>
          <summary class="dashboard-secondary-group-summary">
            <span>{{ $t('dashboard.telemetry_title') }}</span>
            <span>{{ stats.fps?.toFixed(1) || '--' }} fps · {{ (stats.bitrate_kbps / 1000).toFixed(1) }} Mbps</span>
          </summary>
          <section class="dashboard-telemetry-card">
          <div class="flex flex-wrap items-center justify-between gap-3">
            <InfoHint size="sm" :label="$t('dashboard.telemetry')">{{ $t('dashboard.telemetry_desc') }}</InfoHint>
            <!-- Frame health: the numbers that explain "feels stuttery"
                 when FPS looks fine; the strip already covers the rest. -->
            <div class="flex flex-wrap gap-2 text-[11px] text-silver" data-dashboard-frame-health>
              <span class="data-pill" :class="frameHealth.droppedTone">{{ frameHealth.dropped }} dropped</span>
              <span class="data-pill" :class="frameHealth.duplicateTone">{{ frameHealth.duplicate }} duped</span>
              <span class="data-pill" :class="frameHealth.jitterTone">{{ frameHealth.jitter }} jitter</span>
            </div>
          </div>

          <div v-if="!prefersReducedMotion" class="dashboard-telemetry-grid mt-4">
            <div class="card p-2.5">
              <div class="text-[10px] font-semibold uppercase tracking-wider text-success/80">FPS</div>
              <div ref="fpsChartEl" class="h-24 w-full"></div>
            </div>
            <div class="card p-2.5">
              <div class="text-[10px] font-semibold uppercase tracking-wider text-info/80">Bitrate</div>
              <div ref="bitrateChartEl" class="h-24 w-full"></div>
            </div>
            <div class="card p-2.5">
              <div class="text-[10px] font-semibold uppercase tracking-wider text-silver/60">Encode</div>
              <div ref="encodeChartEl" class="h-24 w-full"></div>
            </div>
            <div class="card p-2.5">
              <div class="text-[10px] font-semibold uppercase tracking-wider text-warning/80">Latency</div>
              <div ref="latencyChartEl" class="h-24 w-full"></div>
            </div>
            <div class="card p-2.5">
              <div class="text-[10px] font-semibold uppercase tracking-wider text-accent/80">GPU Load</div>
              <div ref="gpuChartEl" class="h-24 w-full"></div>
            </div>
            <div class="card p-2.5">
              <div class="text-[10px] font-semibold uppercase tracking-wider text-danger/80">Packet Loss</div>
              <div ref="lossChartEl" class="h-24 w-full"></div>
            </div>
          </div>
          <div v-else class="dashboard-empty-state mt-4">
            Live charts are paused while reduced motion is enabled; the summary tiles above keep updating without the extra canvas work.
          </div>

          <div v-if="gpu" class="mt-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <div class="dashboard-metric-tile">
              <div class="dashboard-metric-label">GPU Temp</div>
              <div class="dashboard-metric-value" :class="gpu.temperature_c != null ? (gpu.temperature_c > 80 ? 'text-danger' : gpu.temperature_c > 65 ? 'text-warning' : 'text-success') : 'text-storm'">
                {{ gpu.temperature_c != null ? gpu.temperature_c + '°' : '--' }}
              </div>
              <div class="mt-1 text-xs text-storm">{{ gpu.power_draw_w?.toFixed(0) || '--' }}W draw</div>
            </div>
            <div class="dashboard-metric-tile">
              <div class="dashboard-metric-label">GPU Load</div>
              <div class="dashboard-metric-value text-accent">{{ gpu.utilization_pct != null ? gpu.utilization_pct + '%' : '--' }}</div>
              <div class="mt-1 text-xs text-storm">{{ gpu.clock_mhz || gpu.clock_gpu_mhz || '--' }} MHz</div>
            </div>
            <div class="dashboard-metric-tile" v-if="gpu.encoder_pct != null">
              <div class="dashboard-metric-label">Encoder</div>
              <div class="dashboard-metric-value text-info">{{ gpu.encoder_pct }}%</div>
              <div class="mt-1 text-xs text-storm">{{ gpu.vendor === 'nvidia' ? 'NVENC' : 'VCN' }} workload</div>
            </div>
            <div class="dashboard-metric-tile">
              <div class="dashboard-metric-label">VRAM</div>
              <div class="dashboard-metric-value text-silver">{{ gpu.vram_used_mb != null ? (gpu.vram_used_mb / 1024).toFixed(1) + ' GB' : '--' }}</div>
              <div class="mt-1 text-xs text-storm">{{ gpu.vram_total_mb != null ? '/ ' + (gpu.vram_total_mb / 1024).toFixed(0) + ' GB' : '' }}</div>
            </div>
          </div>
          </section>
        </details>
      </section>
    </template>

    <!-- ═══ IDLE LAYOUT ═══ -->
    <template v-else>
      <!-- Status hero: one band answering "can I stream right now, and if
           not, what fixes it" (absorbs the old strip, triptych, and quad). -->
      <section class="section-card gradient-border-top gradient-border-top-accent" data-dashboard-idle-hero>
        <div class="flex flex-col gap-4 xl:flex-row xl:items-start xl:justify-between">
          <div class="min-w-0">
            <div class="section-kicker">{{ $t('dashboard.stream_readiness') }}</div>
            <h2 class="mt-2 text-2xl font-semibold leading-tight" :class="readinessTone">{{ readinessLabel }}</h2>
            <p class="mt-2 max-w-3xl text-sm leading-relaxed text-storm">{{ nextStep.title }} · {{ nextStep.desc }}</p>
            <div class="mt-4 flex flex-wrap gap-2">
              <template v-if="pairedClientChips.length">
                <span v-for="chip in pairedClientChips" :key="chip.name" class="data-pill">{{ chip.label }}</span>
              </template>
              <span v-else class="data-pill">{{ pairedClients }} {{ $t('dashboard.clients_paired') }}</span>
              <span class="data-pill" :class="headlessEnabled ? 'text-accent' : ''">{{ headlessEnabled ? $t('dashboard.headless') : $t('dashboard.windowed') }}</span>
              <span class="data-pill" v-if="gpu">{{ gpu.temperature_c || '--' }}°C · {{ gpu.utilization_pct || 0 }}% · {{ gpu.power_draw_w?.toFixed(0) || '--' }}W</span>
              <span class="data-pill" :class="aiStatus?.enabled ? 'text-accent' : ''">{{ aiStatus?.enabled ? 'Auto Quality' : 'Manual' }} · {{ sessionHistory.length }} {{ $t('dashboard.sessions') }}</span>
            </div>
          </div>
          <div class="flex shrink-0 flex-col items-end gap-3">
            <span
              class="inline-flex items-center gap-2 rounded-full border px-3 py-1 font-mono text-[10px] font-semibold uppercase tracking-eyebrow"
              :class="readyCheckDisplay.allPassing ? 'border-success/35 text-success' : 'border-warning/35 text-warning'"
            >
              <span class="pulse-dot" :class="readyCheckDisplay.allPassing ? '' : '!bg-warning'"></span>
              {{ readyCheckDisplay.allPassing ? 'READY' : 'ATTENTION' }}
            </span>
            <router-link
              v-if="readyCheckDisplay.primaryIssue"
              :to="readyCheckDisplay.primaryIssue.to"
              class="focus-ring dashboard-action-button dashboard-action-button-primary shrink-0 no-underline"
            >
              {{ $t('dashboard.open_priority_fix') }}
            </router-link>
          </div>
        </div>
      </section>

      <!-- GPU Gauges + Quick Controls -->
      <div class="grid grid-cols-1 items-start gap-4 xl:grid-cols-[minmax(0,1fr)_320px]">
        <div class="section-card space-y-5">
          <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
            <div>
              <div class="section-kicker">{{ $t('dashboard.stream_readiness') }}</div>
              <h2 class="section-title">{{ headlessEnabled ? $t('dashboard.headless') : $t('dashboard.windowed') }} {{ $t('dashboard.mode') }}</h2>
            </div>
            <span class="meta-pill">
              {{ readyChecksPassing }}/{{ readyChecks.length }} {{ $t('dashboard.ready_checks_pass') }}
            </span>
          </div>

          <div class="surface-subtle p-4" v-if="gpu">
            <div class="mb-3 flex items-center justify-between">
              <div class="eyebrow-label text-silver/80">{{ gpu.name }}</div>
              <div class="text-[11px] text-storm">{{ gpu.power_draw_w?.toFixed(0) || '--' }}W · {{ gpu.clock_gpu_mhz || gpu.clock_mhz || '--' }} MHz</div>
            </div>
            <div class="grid grid-cols-2 gap-4 place-items-center xl:grid-cols-4">
              <GaugeArc v-if="gpu.temperature_c != null" :value="gpu.temperature_c" :max="100" unit="°C" label="Temp" :size="112"
                        :thresholds="[{ at: 0, color: 'var(--color-success)' }, { at: 70, color: 'var(--color-warning)' }, { at: 85, color: 'var(--color-danger)' }]" />
              <GaugeArc v-if="gpu.utilization_pct != null" :value="gpu.utilization_pct" :max="100" unit="%" label="GPU" :size="112" />
              <GaugeArc v-if="gpu.encoder_pct != null" :value="gpu.encoder_pct" :max="100" unit="%" :label="gpu.vendor === 'nvidia' ? 'NVENC' : 'VCN'" :size="112"
                        :thresholds="[{ at: 0, color: 'var(--color-ice)' }, { at: 60, color: 'var(--color-warning)' }, { at: 85, color: 'var(--color-danger)' }]" />
              <GaugeArc v-if="gpu.vram_used_mb != null && gpu.vram_total_mb != null" :value="gpu.vram_used_mb / 1024" :max="gpu.vram_total_mb / 1024" unit="GB" label="VRAM" :size="112"
                        :thresholds="[{ at: 0, color: 'var(--color-ice)' }, { at: 70, color: 'var(--color-warning)' }, { at: 90, color: 'var(--color-danger)' }]" />
            </div>
          </div>
          <div v-else class="dashboard-degraded-state">
            <div class="min-w-0">
              <div class="text-sm font-semibold text-silver">Host telemetry is warming up</div>
              <div class="mt-1 text-xs leading-relaxed text-storm">GPU temperature, encoder load, and VRAM gauges will appear after the host reports system stats.</div>
            </div>
            <router-link to="/troubleshooting" class="dashboard-degraded-action">
              Troubleshoot
            </router-link>
          </div>

          <div>
            <div class="flex items-start justify-between gap-3">
              <div>
                <div class="section-kicker">{{ $t('dashboard.ready_checks') }}</div>
                <div class="mt-2 flex items-center gap-2">
                  <div class="text-sm font-medium text-silver">{{ readyChecksPassing }}/{{ readyChecks.length }} {{ $t('dashboard.ready_checks_pass') }}</div>
                  <InfoHint size="sm" :label="$t('dashboard.ready_checks')">{{ $t('dashboard.ready_checks_desc') }}</InfoHint>
                </div>
              </div>
              <span class="rounded-full border px-2.5 py-1 text-[10px] font-medium"
                    :class="readyChecksPassing === readyChecks.length ? 'border-success/30 bg-success/10 text-success' : 'border-warning/30 bg-warning/10 text-warning-bright'">
                {{ readyChecksPassing }}/{{ readyChecks.length }} {{ $t('dashboard.ready_checks_pass') }}
              </span>
            </div>
            <div v-if="readyChecksAllPassing" class="ready-check-summary mt-4">
              <div>
                <div class="text-sm font-semibold text-success">All launch checks are ready</div>
                <div class="mt-1 text-xs text-storm">Pairing, library, discovery, displays, and audio are clear.</div>
              </div>
              <span class="rounded-full border border-success/30 bg-success/10 px-2.5 py-1 text-[10px] font-semibold uppercase tracking-eyebrow text-success">
                {{ readyChecksPassing }}/{{ readyChecks.length }} {{ $t('dashboard.ready_checks_pass') }}
              </span>
            </div>
            <div v-else class="mt-4 space-y-3">
              <div class="ready-check-summary ready-check-summary-attention">
                <div>
                  <div class="text-sm font-semibold text-warning-bright">{{ readyChecksAttentionCount }} launch checks need attention</div>
                  <div class="mt-1 text-xs text-storm">
                    Start with {{ readyChecksPrimaryIssue?.label || 'the first missing check' }} to get this host stream-ready.
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
              <div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
                <router-link
                  v-for="check in visibleReadyChecks"
                  :key="check.key"
                  :to="check.to"
                  class="focus-ring rounded-xl border px-3 py-3 no-underline transition-[border-color,background-color,transform] duration-200 hover:-translate-y-0.5"
                  :class="check.cardClass"
                >
                  <div class="flex items-center justify-between gap-3">
                    <div class="text-sm font-medium text-silver">{{ check.label }}</div>
                    <span class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow" :class="check.badgeClass">
                      {{ check.state }}
                    </span>
                  </div>
                  <div class="mt-2 text-[11px] text-storm">{{ check.detail }}</div>
                  <div class="mt-3 text-[11px] font-medium text-ice/80">{{ $t('dashboard.open_fix') }}</div>
                </router-link>
              </div>
            </div>
          </div>
        </div>
        <div class="section-card">
          <QuickControls @change="handleQuickControlChange" />
        </div>
      </div>

      <!-- Recent Games: the landing page is a remote control, so rows launch. -->
      <div class="section-card" data-dashboard-play-rail>
        <div class="flex items-center justify-between gap-3 mb-3">
          <div>
            <div class="section-kicker">{{ $t('dashboard.recent_games') }}</div>
            <div class="mt-2 text-sm text-storm">{{ recentApps.length ? $t('dashboard.recent_ready', { count: recentApps.length }) : $t('dashboard.no_games') }}</div>
          </div>
          <router-link to="/apps" class="focus-ring inline-flex h-8 items-center gap-1.5 rounded-lg border border-storm px-3 text-xs font-medium text-silver transition-[border-color,color,background-color] duration-200 hover:border-ice hover:text-ice no-underline">
            {{ $t('navbar.library') }}
          </router-link>
        </div>
        <div v-if="recentApps.length" class="grid grid-cols-2 gap-2 sm:grid-cols-3 xl:grid-cols-5">
          <div v-for="app in recentApps" :key="app.uuid" class="dashboard-play-tile">
            <div class="dashboard-play-cover">
              <img v-if="app['image-path']" :src="'./api/covers/image?name=' + encodeURIComponent(app.name)" class="h-full w-full object-cover" loading="lazy" @error="$event.target.style.display='none'" />
            </div>
            <div class="mt-2 min-w-0">
              <div class="truncate text-xs font-semibold text-silver">{{ app.name }}</div>
              <div class="truncate font-mono text-[9px] uppercase tracking-eyebrow text-storm" v-if="app.source && app.source !== 'manual'">{{ app.source }}</div>
            </div>
            <button
              type="button"
              class="focus-ring dashboard-play-launch"
              :disabled="launchingUuid === app.uuid"
              @click="launchRecentApp(app)"
            >
              {{ launchingUuid === app.uuid ? $t('dashboard.launching') : $t('dashboard.launch') }}
            </button>
          </div>
        </div>
        <div v-else class="text-sm text-storm py-6 text-center">{{ $t('dashboard.no_games') }}</div>
        <div class="mt-4 flex flex-wrap items-center gap-2 border-t border-storm/15 pt-3 text-[11px] text-silver">
          <span class="eyebrow-label mr-1">{{ $t('dashboard.host_context') }}</span>
          <span class="data-pill">
            {{ sessionType ? sessionType : (headlessEnabled ? $t('dashboard.headless') : $t('dashboard.windowed')) }}
          </span>
          <span class="data-pill">
            {{ displays.length }} {{ displays.length === 1 ? 'display' : 'displays' }}
          </span>
          <span class="data-pill" v-if="audio?.sink">
            {{ formatAudioName(audio.sink) }}
          </span>
          <span class="data-pill">
            v{{ version }}
          </span>
        </div>
      </div>
    </template>

    <!-- Session History (idle): one list; this browser's rich local history,
         with the host's session log as the fallback source. -->
    <div v-if="statsLoaded && !stats?.streaming && !sessions.length && hostHistoryRows.length" class="card p-4">
      <div class="eyebrow-label mb-3">Session History</div>
      <div class="space-y-2">
        <div v-for="(s, i) in hostHistoryRows" :key="i" class="flex items-center gap-3 rounded-xl border border-storm/15 bg-void/35 px-3 py-2">
          <span class="inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-full font-mono text-[10px] font-bold" :class="{
            'bg-success/20 text-success': s.quality_grade === 'A',
            'bg-info/20 text-info': s.quality_grade === 'B',
            'bg-warning/20 text-warning': s.quality_grade === 'C' || s.quality_grade === 'D',
            'bg-danger/20 text-danger': s.quality_grade === 'F'
          }">{{ s.quality_grade || '·' }}</span>
          <div class="min-w-0 flex-1 truncate text-sm text-silver">{{ s.key }}</div>
        </div>
      </div>
    </div>
    <div v-if="statsLoaded && !stats?.streaming && sessions.length" class="card p-4">
      <div class="flex items-center justify-between mb-3">
        <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider">Session History</div>
        <button @click="clearHistory" class="text-[10px] text-storm hover:text-ice transition-colors">Clear</button>
      </div>
      <div class="space-y-0">
        <div v-for="(s, i) in sessions.slice(0, 8)" :key="i"
             class="flex items-center gap-3 py-2 text-sm" :class="i > 0 ? 'border-t border-storm/15' : ''">
          <div class="text-base font-bold w-6 text-center" :class="gradeColor(s.quality_grade)">{{ s.quality_grade }}</div>
          <div class="flex-1 min-w-0">
            <div class="text-silver font-medium text-sm">{{ s.client_name }}</div>
            <div class="text-[10px] text-storm">{{ s.codec }} {{ s.width }}x{{ s.height }} · {{ formatDuration(s.duration_s) }}</div>
          </div>
          <div class="text-right text-[10px] text-storm shrink-0">
            <div>{{ s.avg_fps }}fps · {{ (s.avg_bitrate_kbps / 1000).toFixed(1) }}Mbps</div>
            <div>{{ formatSessionDate(s.started_at) }}</div>
          </div>
        </div>
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
import { useStreamStats } from '../composables/useStreamStats'
import { useSystemStats } from '../composables/useSystemStats'
import { useSessionHistory, formatDuration } from '../composables/useSessionHistory'
import { useAiOptimizer } from '../composables/useAiOptimizer'
import { useFavicon } from '../composables/useFavicon'
import Skeleton from '../components/Skeleton.vue'
import GaugeArc from '../components/GaugeArc.vue'
import { onThemeTokensChange, readThemeTokens, withAlpha } from '../theme-bridge.js'
import QuickControls from '../components/QuickControls.vue'
import InfoHint from '../components/InfoHint.vue'
import ConfirmActionDialog from '../components/ConfirmActionDialog.vue'
import { useToast } from '../composables/useToast'
import { useI18n } from 'vue-i18n'
import { resolveClientSettingsSync } from '../client-settings-sync'
import { resolveAutoQualityState } from '../auto-quality-state'
import { buildReadyCheckDisplay } from '../dashboard-ready-checks'
import {
  buildLiveSummary,
  buildQualityGrade,
  buildQualityScore,
} from '../dashboard-summary'

const { stats } = useStreamStats(1000)
const { gpu, displays, audio, sessionType } = useSystemStats(3000)
const { sessions, clearHistory } = useSessionHistory(stats)
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
const clientSettingsSync = ref(resolveClientSettingsSync({}))
const { t } = useI18n()
const { toast: showToast } = useToast()

const autoQuality = computed(() => resolveAutoQualityState(stats.value || {}, clientSettingsSync.value || {}))

const actionSummary = computed(() => {
  if (!statsLoaded.value) return t('dashboard.loading_summary')
  if (stats.value?.streaming) return t('dashboard.streaming_summary')
  return t('dashboard.idle_summary', { count: pairedClients.value })
})

const readinessLabel = computed(() => {
  if (stats.value?.streaming) return t('dashboard.readiness_live')
  if (pairedClients.value > 0) return t('dashboard.readiness_ready')
  return t('dashboard.readiness_setup')
})

const readinessTone = computed(() => {
  if (stats.value?.streaming || pairedClients.value > 0) return 'text-success'
  return 'text-warning'
})

const nextStep = computed(() => {
  if (stats.value?.streaming) return { title: t('dashboard.next_step_monitor'), desc: t('dashboard.next_step_monitor_desc') }
  if (!pairedClients.value) return { title: t('dashboard.next_step_pair'), desc: t('dashboard.next_step_pair_desc') }
  return { title: t('dashboard.next_step_launch'), desc: t('dashboard.next_step_launch_desc') }
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

  return items.map((item) => ({
    ...item,
    state: item.ok ? t('dashboard.check_state_ready') : t('dashboard.check_state_attention'),
    cardClass: item.ok
      ? 'border-success/15 bg-success/5 hover:border-success/30 hover:bg-success/8'
      : 'border-warning/15 bg-warning/5 hover:border-warning/30 hover:bg-warning/8',
    badgeClass: item.ok ? 'bg-success/10 text-success' : 'bg-warning/10 text-warning-bright'
  }))
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

function refreshClientSettingsSync(configPayload) {
  clientSettingsSync.value = resolveClientSettingsSync(configPayload || {})
}

const connectedClients = computed(() => {
  if (!stats.value?.streaming) return []

  if (Array.isArray(stats.value.clients) && stats.value.clients.length > 0) {
    return stats.value.clients
  }

  return [{
    name: stats.value.client_name || t('dashboard.unknown_client'),
    ip: stats.value.client_ip || '--',
    fps: stats.value.fps || 0,
    latency_ms: stats.value.latency_ms || 0,
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
const currentClientName = computed(() => connectedClients.value[0]?.name || t('dashboard.unknown_client'))

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

const runtimeBackendLabel = computed(() => {
  const backend = String(stats.value?.runtime_backend || '').trim()
  if (!backend || backend === 'none' || backend === 'unknown') {
    // Last-resort while idle: derive from configured path id if present.
    const pathId = stats.value?.stream_path_id || stats.value?.stream_display_mode_id
    if (pathId === 'headless_stream' || pathId === 'windowed_stream' || pathId === 'family_isolated') return 'Labwc'
    if (pathId === 'gamescope_stream') return 'Gamescope'
    if (pathId === 'host_virtual_display' || pathId === 'headless_evdi') return 'Virtual display'
    if (pathId === 'desktop_display') return 'Portal / host'
    return 'Host'
  }
  if (backend === 'labwc') return 'Labwc'
  if (backend === 'gamescope') return 'Gamescope'
  if (backend === 'portal') return 'Portal'
  if (backend === 'virtual_display') return 'Virtual display'
  return titleizeToken(backend)
})

const runtimeEffectiveMode = computed(() => {
  if (!stats.value?.streaming) {
    // Idle: still show configured path when stats expose it.
    const idleLabel = streamDisplayModeLabel(stats.value)
    return idleLabel && idleLabel !== 'Private Stream (windowed)' ? idleLabel : '--'
  }
  return streamDisplayModeLabel(stats.value)
})

const runtimeModeTone = computed(() => {
  if (!stats.value?.streaming) return 'bg-storm/20 text-storm'
  const mode = String(runtimeEffectiveMode.value || '').toLowerCase()
  if (mode.includes('private stream')) return 'bg-accent/15 text-accent'
  if (mode.includes('private stream (windowed)') || mode.includes('host virtual')) return 'bg-warning/15 text-warning'
  return 'bg-storm/20 text-storm'
})

const nestedLabwcShmFallbackActive = computed(() => {
  if (!stats.value?.streaming) return false

  const backend = String(stats.value?.runtime_backend || '').toLowerCase()
  const transport = String(stats.value?.capture_transport || '').toLowerCase()

  return backend === 'labwc' &&
    !Boolean(stats.value?.runtime_effective_headless) &&
    transport === 'shm'
})

const captureTransportLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return titleizeToken(stats.value?.capture_transport || 'unknown')
})

const capturePathLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return titleizeToken(stats.value?.capture_path || 'unknown')
})

const captureGpuNativeTone = computed(() => {
  if (!stats.value?.streaming) return 'text-storm'
  return stats.value?.capture_gpu_native ? 'text-success' : 'text-warning'
})

const runtimePathNote = computed(() => {
  if (!stats.value?.streaming) return ''
  const reason = String(stats.value?.capture_path_reason || '').toLowerCase()

  if (nestedLabwcShmFallbackActive.value) {
    return 'Nested labwc fallback is active: Polaris is capturing the windowed compositor through SHM instead of the GPU-native fast path.'
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

const runtimePathNoteTone = computed(() => {
  if (!runtimePathNote.value) return 'text-storm'
  if (nestedLabwcShmFallbackActive.value) return 'text-warning'
  if (stats.value?.capture_cpu_copy) return 'text-warning'
  if (stats.value?.capture_gpu_native) return 'text-success'
  return 'text-warning'
})

const liveSessionTitle = computed(() => (
  connectedClients.value.length > 1
    ? t('dashboard.live_session_multi', { count: connectedClients.value.length })
    : t('dashboard.live_session_single', { client: currentClientName.value })
))

const liveSessionSummary = computed(() => {
  if (runtimePathNote.value) return runtimePathNote.value
  return t('dashboard.live_session_summary', {
    backend: runtimeBackendLabel.value,
    mode: String(runtimeEffectiveMode.value || '').toLowerCase(),
    transport: captureTransportLabel.value,
  })
})

const qualitySummaryLabel = computed(() => t('dashboard.quality_summary', {
  grade: qualityGrade.value,
  score: qualityScore.value,
}))

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

function startPreview() {
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
  if (!showPreview.value) return
  previewTimer = setTimeout(refreshPreview, delayMs)
}

function refreshPreview() {
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
      if (previewMode.value === 'mjpeg' && showPreview.value && !previewError.value) {
        previewLoaded.value = true
        schedulePreviewRefresh(30000)
      }
    }, 1500)
  } else {
    previewUrl.value = `./api/display/screenshot?t=${Date.now()}${output}`
  }
}

function handlePreviewLoad() {
  previewLoaded.value = true
  previewError.value = false
  previewBackoffMs.value = PREVIEW_REFRESH_MS
  // The MJPEG stream keeps updating the img on its own; only polling refreshes.
  if (previewMode.value !== 'mjpeg') {
    schedulePreviewRefresh(PREVIEW_REFRESH_MS)
  }
}

function handlePreviewError() {
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

function stopPreview() {
  showPreview.value = false
  previewExpanded.value = false
  previewLoaded.value = false
  previewError.value = false
  previewBackoffMs.value = PREVIEW_REFRESH_MS
  if (previewTimer) { clearTimeout(previewTimer); previewTimer = null }
}

const qualityScore = computed(() => buildQualityScore(stats.value || {}))

const qualityGrade = computed(() => buildQualityGrade(qualityScore.value))

const previewSupportCopy = computed(() => (
  previewExpanded.value
    ? t('dashboard.preview_support_expanded')
    : t('dashboard.preview_support')
))

const previewStatusText = computed(() => {
  if (previewError.value) return t('dashboard.preview_unavailable_status')
  if (!previewLoaded.value) return t('dashboard.preview_capturing')
  return t('dashboard.preview_status')
})



// Frame health: dropped/duplicate ratios and jitter arrive on every SSE tick
// but were never surfaced; they explain stutter that FPS alone hides.
const frameHealth = computed(() => {
  const s = stats.value || {}
  const dropped = Number(s.dropped_frame_ratio)
  const duplicate = Number(s.duplicate_frame_ratio)
  const jitter = Number(s.frame_jitter_ms)
  const pct = (v) => (Number.isFinite(v) ? `${(v * 100).toFixed(1)}%` : '--')
  return {
    dropped: pct(dropped),
    droppedTone: Number.isFinite(dropped) && dropped > 0.02 ? 'text-danger' : Number.isFinite(dropped) && dropped > 0.005 ? 'text-warning' : '',
    duplicate: pct(duplicate),
    duplicateTone: Number.isFinite(duplicate) && duplicate > 0.05 ? 'text-warning' : '',
    jitter: Number.isFinite(jitter) ? `${jitter.toFixed(1)} ms` : '--',
    jitterTone: Number.isFinite(jitter) && jitter > 4 ? 'text-warning' : '',
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

const pairedClientChips = computed(() => {
  const chips = pairedClientList.value.slice(0, 3).map((cert) => {
    const seen = relativeSeen(cert.last_seen_at)
    return { name: cert.name || cert.uuid, label: seen ? `${cert.name} · ${seen}` : cert.name }
  }).filter((chip) => chip.name)
  const extra = pairedClientList.value.length - chips.length
  if (extra > 0) chips.push({ name: '+extra', label: `+${extra} more` })
  return chips
})

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
    const config = configRes.ok ? await configRes.json() : {}
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

// The host describes a safe recovery action. Only /api/ endpoints are
// reachable from this web server (the host also emits game-stream-server
// endpoints like /polaris/v1/client-settings, which would 404 here), so
// anything else renders as advice without an execute button.
const doctorSafeAction = computed(() => {
  // Endpoint-less actions (export diagnostics, safer-next-launch) are real
  // host advice and render as advisory pills; only /api/ ones can execute.
  const action = doctor.value?.safe_recovery_action
  if (!action || action.kind === 'none' || !action.label) return null
  return action
})

const doctorActionExecutable = computed(() => Boolean(doctorSafeAction.value?.endpoint?.startsWith('/api/')))

const doctorActionConfirmOpen = ref(false)
const doctorActionPending = ref(false)

async function runDoctorSafeAction() {
  const action = doctorSafeAction.value
  doctorActionConfirmOpen.value = false
  if (!action || doctorActionPending.value) return
  doctorActionPending.value = true
  try {
    const response = await fetch(`.${action.endpoint.startsWith('/') ? '' : '/'}${action.endpoint}`, {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: action.method || 'POST',
      body: JSON.stringify(action.payload_preview || action.payload || {}),
    })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    showToast(t('dashboard.doctor_action_success') + action.label, 'success')
  } catch (e) {
    showToast(t('dashboard.doctor_action_error') + e.message, 'error')
  } finally {
    doctorActionPending.value = false
  }
}

// Launch a recent game straight from the landing page (same endpoint the
// Library uses). Launching is the page's primary action, not destructive.
const launchingUuid = ref('')

async function launchRecentApp(app) {
  if (launchingUuid.value) return
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
const disconnectConfirmOpen = ref(false)
const disconnectingClient = ref(false)
const disconnectClientImpactItems = computed(() => [
  t('dashboard.disconnect_client_impact_stream'),
  t('dashboard.disconnect_client_impact_reconnect'),
])

async function resolveConnectedClient() {
  if (!stats.value?.streaming) return
  try {
    const res = await fetch('./api/clients/list', { credentials: 'include' })
    if (res.ok) {
      const clients = await res.json()
      const connected = clients.find(c => c.name === stats.value.client_name)
      if (connected) connectedClientUuid.value = connected.uuid
    }
  } catch {}
}

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

// Lazy-load uPlot
let uPlotLib = null

async function loadUPlot() {
  if (!uPlotLib) {
    const mod = await import('uplot')
    await import('uplot/dist/uPlot.min.css')
    uPlotLib = mod.default
  }
  return uPlotLib
}

const prefersReducedMotion = ref(false)
let reducedMotionQuery = null

function updateReducedMotionPreference(event) {
  prefersReducedMotion.value = Boolean(event?.matches)
}

// Track whether we've gotten at least one stats response
const statsLoaded = ref(false)

// Fetch the config-derived state the ready checks and sync badge depend on.
async function fetchSystemInfo() {
  try {
    const configRes = await fetch('./api/config', { credentials: 'include' })
    if (configRes.ok) {
      const config = await configRes.json()
      refreshClientSettingsSync(config)
      streamingOutput.value = config.linux_streaming_output || config.output_name || ''
      discoveryEnabled.value = config.enable_discovery !== 'disabled'
      pairingEnabled.value = config.enable_pairing !== 'disabled'
    }
  } catch (e) {
    // Non-critical, just leave defaults
  }
}

// Chart refs
const fpsChartEl = ref(null)
const bitrateChartEl = ref(null)
const encodeChartEl = ref(null)
const latencyChartEl = ref(null)
const gpuChartEl = ref(null)
const lossChartEl = ref(null)

// Chart instances
let fpsChart = null
let bitrateChart = null
let encodeChart = null
let latencyChart = null
let gpuChart = null
let lossChart = null

// Rolling data (60 seconds)
const MAX_POINTS = 60
const timestamps = ref([])
const fpsHistory = ref([])
const bitrateHistory = ref([])
const encodeHistory = ref([])
const latencyHistory = ref([])
const gpuHistory = ref([])
const lossHistory = ref([])

// Chart colors resolve from the active theme's tokens at build time; charts
// are rebuilt on theme swaps because canvas cannot follow CSS variables.
function makeChartOpts(title, suffix, tokenName = 'ice') {
  const tokens = readThemeTokens(['ice', 'twilight', 'storm', tokenName])
  const color = tokens[tokenName] || tokens.ice
  return {
    width: 300,
    height: 96,
    cursor: { show: false },
    legend: { show: false },
    axes: [
      {
        stroke: tokens.twilight,
        grid: { stroke: withAlpha(tokens.twilight, 0.125), width: 1 },
        ticks: { show: false },
        font: '9px sans-serif',
        values: () => [],
      },
      {
        stroke: tokens.storm,
        grid: { stroke: withAlpha(tokens.twilight, 0.125), width: 1 },
        ticks: { stroke: tokens.twilight, width: 1 },
        font: '9px sans-serif',
        size: 35,
      },
    ],
    series: [
      {},
      {
        stroke: color,
        width: 1.5,
        fill: withAlpha(color, 0.06),
      },
    ],
  }
}

function initChart(el, opts) {
  if (!el || !uPlotLib) return null
  const actualWidth = el.clientWidth || 300
  opts.width = actualWidth
  const data = [[], []]
  return new uPlotLib(opts, data, el)
}

function updateChartData(chart, ts, values) {
  if (!chart) return
  chart.setData([ts, values])
}

function resizeCharts() {
  const charts = [
    { chart: fpsChart, el: fpsChartEl.value },
    { chart: bitrateChart, el: bitrateChartEl.value },
    { chart: encodeChart, el: encodeChartEl.value },
    { chart: latencyChart, el: latencyChartEl.value },
    { chart: gpuChart, el: gpuChartEl.value },
    { chart: lossChart, el: lossChartEl.value },
  ]
  for (const { chart, el } of charts) {
    if (chart && el) {
      chart.setSize({ width: el.clientWidth, height: 96 })
    }
  }
}

let resizeObserver = null

async function setupCharts() {
  await loadUPlot()
  await nextTick()
  if (fpsChartEl.value && !fpsChart) {
    fpsChart = initChart(fpsChartEl.value, makeChartOpts('FPS', 'fps', 'success'))
  }
  if (bitrateChartEl.value && !bitrateChart) {
    bitrateChart = initChart(bitrateChartEl.value, makeChartOpts('Bitrate', 'Mbps', 'info'))
  }
  if (latencyChartEl.value && !latencyChart) {
    latencyChart = initChart(latencyChartEl.value, makeChartOpts('Latency', 'ms', 'warning'))
  }
  if (gpuChartEl.value && !gpuChart) {
    gpuChart = initChart(gpuChartEl.value, makeChartOpts('GPU', '%', 'accent'))
  }
  if (lossChartEl.value && !lossChart) {
    lossChart = initChart(lossChartEl.value, makeChartOpts('Loss', '%', 'danger'))
  }
  if (encodeChartEl.value && !encodeChart) {
    encodeChart = initChart(encodeChartEl.value, makeChartOpts('Encode', 'ms'))
  }
}

function destroyChartInstances() {
  if (fpsChart) { fpsChart.destroy(); fpsChart = null }
  if (bitrateChart) { bitrateChart.destroy(); bitrateChart = null }
  if (encodeChart) { encodeChart.destroy(); encodeChart = null }
  if (latencyChart) { latencyChart.destroy(); latencyChart = null }
  if (gpuChart) { gpuChart.destroy(); gpuChart = null }
  if (lossChart) { lossChart.destroy(); lossChart = null }
}

function destroyCharts() {
  destroyChartInstances()
  timestamps.value = []
  fpsHistory.value = []
  bitrateHistory.value = []
  encodeHistory.value = []
  latencyHistory.value = []
  gpuHistory.value = []
  lossHistory.value = []
}

// Rebuild live charts with the new theme's colors, keeping their history.
async function refreshChartTheme() {
  const hadCharts = Boolean(fpsChart || bitrateChart || encodeChart || latencyChart || gpuChart || lossChart)
  if (!hadCharts) return
  destroyChartInstances()
  await setupCharts()
  const ts = [...timestamps.value]
  updateChartData(fpsChart, ts, [...fpsHistory.value])
  updateChartData(bitrateChart, ts, [...bitrateHistory.value])
  updateChartData(encodeChart, ts, [...encodeHistory.value])
  updateChartData(latencyChart, ts, [...latencyHistory.value])
  updateChartData(gpuChart, ts, [...gpuHistory.value])
  updateChartData(lossChart, ts, [...lossHistory.value])
}

watch(stats, (newStats, oldStats) => {
  statsLoaded.value = true

  if (!newStats || !newStats.streaming) {
    if (oldStats?.streaming) {
      stopPreview()
    }
    destroyCharts()
    connectedClientUuid.value = null
    return
  }

  // Resolve client UUID and auto-show preview when streaming starts
  if (newStats.streaming && (!oldStats || !oldStats.streaming)) {
    resolveConnectedClient()
    if (!showPreview.value && !prefersReducedMotion.value) startPreview()
  }

  if (prefersReducedMotion.value) {
    destroyCharts()
    return
  }

  const now = Date.now() / 1000

  timestamps.value.push(now)
  fpsHistory.value.push(newStats.fps)
  bitrateHistory.value.push(newStats.bitrate_kbps / 1000)
  encodeHistory.value.push(newStats.encode_time_ms)
  latencyHistory.value.push(newStats.latency_ms)
  gpuHistory.value.push(gpu.value?.utilization_pct || 0)
  lossHistory.value.push(newStats.packet_loss || 0)

  // Keep rolling window
  while (timestamps.value.length > MAX_POINTS) {
    timestamps.value.shift()
    fpsHistory.value.shift()
    bitrateHistory.value.shift()
    encodeHistory.value.shift()
    latencyHistory.value.shift()
    gpuHistory.value.shift()
    lossHistory.value.shift()
  }

  // Initialize charts if needed
  if (!fpsChart || !bitrateChart || !encodeChart || !latencyChart || !gpuChart || !lossChart) {
    setupCharts()
  }

  // Update chart data
  nextTick(() => {
    const ts = [...timestamps.value]
    updateChartData(fpsChart, ts, [...fpsHistory.value])
    updateChartData(bitrateChart, ts, [...bitrateHistory.value])
    updateChartData(encodeChart, ts, [...encodeHistory.value])
    updateChartData(latencyChart, ts, [...latencyHistory.value])
    updateChartData(gpuChart, ts, [...gpuHistory.value])
    updateChartData(lossChart, ts, [...lossHistory.value])
  })
})

watch(prefersReducedMotion, (isReduced) => {
  if (isReduced) {
    destroyCharts()
  } else if (stats.value?.streaming) {
    setupCharts()
  }
})

let unsubscribeThemeTokens = null

onMounted(async () => {
  if (typeof window !== 'undefined' && typeof window.matchMedia === 'function') {
    reducedMotionQuery = window.matchMedia('(prefers-reduced-motion: reduce)')
    updateReducedMotionPreference(reducedMotionQuery)
    reducedMotionQuery.addEventListener?.('change', updateReducedMotionPreference)
  }
  unsubscribeThemeTokens = onThemeTokensChange(() => { refreshChartTheme() })
  document.addEventListener('visibilitychange', handlePreviewVisibility)

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

  // Fetch recent games for quick launch
  try {
    const res = await fetch('./api/apps', { credentials: 'include' })
    if (res.ok) {
      const data = await res.json()
      const apps = (data.apps || []).filter(a => a.uuid && a.name !== 'Desktop')
      // Sort by last-launched (most recent first), take top 5
      apps.sort((a, b) => (b['last-launched'] || 0) - (a['last-launched'] || 0))
      appCatalogCount.value = apps.length
      recentApps.value = apps.slice(0, 5)
    }
  } catch {}

  // Fetch paired clients count + version
  try {
    const res = await fetch('./api/clients/list', { credentials: 'include' })
    if (res.ok) {
      const data = await res.json()
      pairedClientList.value = data.named_certs || []
      pairedClients.value = pairedClientList.value.length
    }
  } catch {}
  try {
    const res = await fetch('./api/config', { credentials: 'include' })
    if (res.ok) {
      const data = await res.json()
      refreshClientSettingsSync(data)
      version.value = data.version || '0.0.0'
      headlessEnabled.value = data.headless_mode === 'enabled'
      discoveryEnabled.value = data.enable_discovery !== 'disabled'
      pairingEnabled.value = data.enable_pairing !== 'disabled'
    }
  } catch {}

  resizeObserver = new ResizeObserver(() => {
    resizeCharts()
  })
  // Observe the parent container for resize
  const parent = fpsChartEl.value?.parentElement?.parentElement
  if (parent) {
    resizeObserver.observe(parent)
  }
})

onUnmounted(() => {
  stopPreview()
  document.removeEventListener('visibilitychange', handlePreviewVisibility)
  destroyCharts()
  if (unsubscribeThemeTokens) {
    unsubscribeThemeTokens()
    unsubscribeThemeTokens = null
  }
  reducedMotionQuery?.removeEventListener?.('change', updateReducedMotionPreference)
  reducedMotionQuery = null
  if (resizeObserver) {
    resizeObserver.disconnect()
    resizeObserver = null
  }
})
</script>
