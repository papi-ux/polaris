<template>
  <div class="page-shell relative">

    <section class="page-header">
      <div class="page-heading">
        <h1 class="page-title">{{ $t('dashboard.title') }}</h1>
        <div class="page-subtitle">{{ actionSummary }}</div>
      </div>
      <div class="page-meta">
        <span v-if="stats?.streaming" class="pulse-dot"></span>
        <span class="meta-pill" :class="stats?.streaming ? 'border-green-500/30 bg-green-500/10 text-green-300' : ''">
          {{ stats?.streaming ? $t('dashboard.live') : $t('dashboard.standby') }}
        </span>
      </div>
    </section>

    <div v-if="statsLoaded && !stats?.streaming" class="grid grid-cols-1 gap-3 lg:grid-cols-3">
      <div class="surface-subtle p-4">
        <div class="eyebrow-label">{{ $t('dashboard.primary_focus') }}</div>
        <div class="mt-2 text-base font-semibold text-silver">{{ primaryFocus.title }}</div>
        <div class="mt-2 text-sm leading-relaxed text-storm">{{ primaryFocus.desc }}</div>
      </div>
      <div class="surface-subtle p-4">
        <div class="eyebrow-label">{{ $t('dashboard.stream_readiness') }}</div>
        <div class="mt-2 text-base font-semibold" :class="readinessTone">{{ readinessLabel }}</div>
        <div class="mt-2 text-sm leading-relaxed text-storm">{{ readinessDetail }}</div>
      </div>
      <div class="surface-subtle p-4">
        <div class="eyebrow-label">{{ $t('dashboard.next_step') }}</div>
        <div class="mt-2 text-base font-semibold text-silver">{{ nextStep.title }}</div>
        <div class="mt-2 text-sm leading-relaxed text-storm">{{ nextStep.desc }}</div>
      </div>
    </div>

    <!-- Loading skeleton state -->
    <template v-if="!statsLoaded">
      <div class="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <Skeleton type="card" v-for="n in 4" :key="n" />
      </div>
    </template>

    <template v-else-if="stats?.streaming">
      <section class="section-card dashboard-live-shell" :class="{ 'is-preview-expanded': showPreview && previewExpanded }">
        <div class="dashboard-live-header">
          <div class="dashboard-live-header-copy">
            <div class="section-kicker">{{ $t('dashboard.live_session') }}</div>
            <h2 class="section-title">{{ liveSessionTitle }}</h2>
            <p class="section-copy">{{ liveSessionSummary }}</p>
          </div>
          <div class="dashboard-live-header-meta">
            <span class="meta-pill">{{ viewerCountLabel }}</span>
            <span class="meta-pill">{{ qualitySummaryLabel }}</span>
            <span class="meta-pill" :class="runtimeModeTone">{{ runtimeEffectiveMode }}</span>
            <span class="meta-pill" :class="captureGpuNativeTone">{{ capturePathLabel }}</span>
          </div>
        </div>

        <div v-if="streamPathNotices.length" class="grid gap-2 md:grid-cols-2">
          <div
            v-for="notice in streamPathNotices"
            :key="notice.key"
            class="rounded-lg border px-3 py-2"
            :class="notice.surfaceClass"
          >
            <div class="text-[11px] font-semibold uppercase tracking-[0.16em]" :class="notice.titleClass">
              {{ notice.title }}
            </div>
            <div class="mt-1 text-sm leading-relaxed text-silver">{{ notice.message }}</div>
          </div>
        </div>

        <div class="dashboard-live-stage" :class="{ 'is-preview-expanded': showPreview && previewExpanded, 'is-preview-hidden': !showPreview }">
          <div class="dashboard-live-main">
            <section class="dashboard-preview-panel">
              <div class="dashboard-preview-header">
                <div class="min-w-0">
                  <div class="eyebrow-label">{{ $t('dashboard.display_preview') }}</div>
                  <div class="mt-2 text-base font-semibold text-silver">{{ previewHeadline }}</div>
                  <div class="mt-2 text-sm leading-relaxed text-storm">{{ previewSupportCopy }}</div>
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
                  <div v-if="!previewLoaded && !previewError" class="dashboard-preview-overlay">
                    <svg class="h-7 w-7 animate-spin text-storm" fill="none" viewBox="0 0 24 24" aria-hidden="true">
                      <circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" />
                      <path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
                    </svg>
                    <span>{{ $t('dashboard.preview_capturing') }}</span>
                  </div>
                  <div v-if="previewError" class="dashboard-preview-overlay dashboard-preview-overlay-error">
                    <div class="text-sm font-medium text-silver">{{ $t('dashboard.preview_error') }}</div>
                    <button @click="retryPreviewNow" class="focus-ring dashboard-action-button dashboard-action-button-secondary">
                      {{ $t('dashboard.preview_retry') }}
                    </button>
                  </div>
                </div>

                <div class="dashboard-preview-footer">
                  <div class="dashboard-preview-meta">
                    <span class="data-pill">{{ stats.width }}×{{ stats.height }}</span>
                    <span class="data-pill">{{ stats.codec?.toUpperCase() || '--' }}</span>
                    <span class="data-pill">{{ runtimeBackendLabel }}</span>
                    <span class="data-pill">{{ capturePathLabel }}</span>
                  </div>
                  <div class="text-xs text-storm">{{ previewStatusText }}</div>
                </div>
              </template>
              <div v-else class="dashboard-preview-empty">
                <div>
                  <div class="text-sm font-medium text-silver">{{ $t('dashboard.preview_hidden_title') }}</div>
                  <div class="mt-2 text-sm leading-relaxed text-storm">{{ $t('dashboard.preview_hidden_desc') }}</div>
                </div>
                <div class="dashboard-preview-meta">
                  <span class="data-pill">{{ viewerCountLabel }}</span>
                  <span class="data-pill">{{ qualitySummaryLabel }}</span>
                </div>
              </div>
            </section>

            <div class="dashboard-live-support-grid">
              <section class="surface-subtle p-4 dashboard-support-card">
                <div class="flex items-start justify-between gap-3">
                  <div>
                    <div class="eyebrow-label">{{ $t('dashboard.recommendations') }}</div>
                    <div class="mt-2 text-base font-semibold text-silver">Priority guidance for the live stream.</div>
                  </div>
                  <span class="meta-pill">{{ primaryRecommendations.length || 0 }} live cues</span>
                </div>
                <div v-if="primaryRecommendations.length" class="mt-4 space-y-2">
                  <div v-for="(rec, i) in primaryRecommendations" :key="i" class="glass rounded-lg px-3 py-2 text-xs text-silver">
                    <div class="flex items-start gap-2">
                      <svg class="mt-0.5 h-3.5 w-3.5 shrink-0" :class="rec.color" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
                      </svg>
                      <span>{{ rec.message }}</span>
                    </div>
                  </div>
                </div>
                <div v-else class="dashboard-empty-state">
                  {{ $t('dashboard.recommendations_empty') }}
                </div>
                <div class="dashboard-support-subsection">
                  <div class="dashboard-support-subtitle">{{ $t('dashboard.ai_optimization') }}</div>
                  <div v-if="aiOptimization" class="space-y-3">
                    <div class="text-sm leading-relaxed text-silver">{{ aiOptimizationSummary }}</div>
                    <div class="flex flex-wrap gap-2">
                      <span class="data-pill">{{ aiOptimization.source }}</span>
                      <span v-if="aiOptimization.display_mode" class="data-pill">{{ aiOptimization.display_mode }}</span>
                      <span v-if="aiOptimization.target_bitrate_kbps" class="data-pill">{{ (aiOptimization.target_bitrate_kbps / 1000).toFixed(0) }} Mbps</span>
                    </div>
                  </div>
                  <div v-else class="text-sm text-storm">
                    {{ $t('dashboard.ai_optimization_empty') }}
                  </div>
                </div>
              </section>

              <section class="surface-subtle p-4 dashboard-support-card">
                <div class="flex items-start justify-between gap-3">
                  <div>
                    <div class="eyebrow-label">Session tools</div>
                    <div class="mt-2 text-base font-semibold text-silver">Capture what matters and keep recent context nearby.</div>
                  </div>
                  <span class="meta-pill" :class="recording.active ? 'border-red-500/35 bg-red-500/10 text-red-300' : ''">
                    {{ recording.active ? $t('dashboard.recording_active') : $t('dashboard.recording_idle') }}
                  </span>
                </div>
                <div class="dashboard-support-subsection">
                  <div class="dashboard-support-subtitle">{{ $t('dashboard.recording') }}</div>
                  <div class="text-sm text-storm">Keep a clip or save an instant replay without leaving Mission Control.</div>
                </div>
                <div class="mt-4 flex flex-wrap items-center gap-2">
                  <button
                    v-if="!recording.active"
                    @click="startRecording"
                    class="focus-ring dashboard-action-button dashboard-action-button-danger"
                  >
                    {{ $t('dashboard.record') }}
                  </button>
                  <button
                    v-if="recording.active"
                    @click="stopRecording"
                    class="focus-ring dashboard-action-button dashboard-action-button-secondary"
                  >
                    {{ $t('dashboard.stop_recording') }}
                  </button>
                  <button
                    @click="saveReplay"
                    class="focus-ring dashboard-action-button dashboard-action-button-ghost"
                  >
                    {{ $t('dashboard.save_replay') }}
                  </button>
                </div>
                <div v-if="recording.file" class="mt-3 text-xs break-all text-storm">{{ recording.file }}</div>
                <div class="dashboard-support-subsection">
                  <div class="dashboard-support-subtitle">{{ $t('dashboard.session_history') }}</div>
                  <div v-if="sessionHistory.length" class="mt-3 space-y-2">
                    <div v-for="(s, i) in sessionHistory.slice(0, 4)" :key="i" class="dashboard-list-row">
                      <span
                        class="inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-full text-[10px] font-bold"
                        :class="{
                          'bg-green-500/20 text-green-400': s.quality_grade === 'A',
                          'bg-blue-500/20 text-blue-400': s.quality_grade === 'B',
                          'bg-yellow-500/20 text-yellow-400': s.quality_grade === 'C',
                          'bg-orange-500/20 text-orange-400': s.quality_grade === 'D',
                          'bg-red-500/20 text-red-400': s.quality_grade === 'F'
                        }"
                      >
                        {{ s.quality_grade }}
                      </span>
                      <div class="min-w-0 flex-1 text-[11px] text-storm">
                        <div class="truncate text-sm text-silver">{{ s.key }}</div>
                      </div>
                    </div>
                  </div>
                  <div v-else class="dashboard-empty-state mt-3">
                    {{ $t('dashboard.session_history_empty') }}
                  </div>
                </div>
              </section>
            </div>

            <section class="dashboard-telemetry-card">
              <div class="flex flex-col gap-3 lg:flex-row lg:items-end lg:justify-between">
                <div>
                  <div class="section-kicker">{{ $t('dashboard.telemetry') }}</div>
                  <h3 class="section-title">{{ $t('dashboard.telemetry_title') }}</h3>
                  <p class="section-copy">{{ $t('dashboard.telemetry_desc') }}</p>
                </div>
                <div class="flex flex-wrap gap-2 text-[11px] text-silver">
                  <span class="data-pill">{{ stats.fps?.toFixed(1) || '--' }} fps</span>
                  <span class="data-pill">{{ (stats.bitrate_kbps / 1000).toFixed(1) }} Mbps</span>
                  <span class="data-pill">{{ stats.latency_ms?.toFixed(0) || '--' }} ms</span>
                  <span class="data-pill">{{ stats.packet_loss?.toFixed(1) || '--' }}% loss</span>
                </div>
              </div>

              <div class="dashboard-telemetry-grid mt-4">
                <div class="card p-3">
                  <div class="text-[10px] font-semibold uppercase tracking-wider text-green-400/80">FPS</div>
                  <div ref="fpsChartEl" class="h-24 w-full"></div>
                </div>
                <div class="card p-3">
                  <div class="text-[10px] font-semibold uppercase tracking-wider text-sky-400/80">Bitrate</div>
                  <div ref="bitrateChartEl" class="h-24 w-full"></div>
                </div>
                <div class="card p-3">
                  <div class="text-[10px] font-semibold uppercase tracking-wider text-silver/60">Encode</div>
                  <div ref="encodeChartEl" class="h-24 w-full"></div>
                </div>
                <div class="card p-3">
                  <div class="text-[10px] font-semibold uppercase tracking-wider text-amber-400/80">Latency</div>
                  <div ref="latencyChartEl" class="h-24 w-full"></div>
                </div>
                <div class="card p-3">
                  <div class="text-[10px] font-semibold uppercase tracking-wider text-fuchsia-300/80">GPU Load</div>
                  <div ref="gpuChartEl" class="h-24 w-full"></div>
                </div>
                <div class="card p-3">
                  <div class="text-[10px] font-semibold uppercase tracking-wider text-red-400/80">Packet Loss</div>
                  <div ref="lossChartEl" class="h-24 w-full"></div>
                </div>
              </div>

              <div v-if="gpu" class="mt-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
                <div class="dashboard-metric-tile">
                  <div class="dashboard-metric-label">GPU Temp</div>
                  <div class="dashboard-metric-value" :class="gpu.temperature_c > 80 ? 'text-red-400' : gpu.temperature_c > 65 ? 'text-yellow-400' : 'text-green-400'">
                    {{ gpu.temperature_c }}°
                  </div>
                  <div class="mt-1 text-xs text-storm">{{ gpu.power_draw_w?.toFixed(0) || '--' }}W draw</div>
                </div>
                <div class="dashboard-metric-tile">
                  <div class="dashboard-metric-label">GPU Load</div>
                  <div class="dashboard-metric-value text-fuchsia-300">{{ gpu.utilization_pct }}%</div>
                  <div class="mt-1 text-xs text-storm">{{ gpu.clock_mhz || gpu.clock_gpu_mhz || '--' }} MHz</div>
                </div>
                <div class="dashboard-metric-tile">
                  <div class="dashboard-metric-label">Encoder</div>
                  <div class="dashboard-metric-value text-sky-300">{{ gpu.encoder_pct }}%</div>
                  <div class="mt-1 text-xs text-storm">NVENC workload</div>
                </div>
                <div class="dashboard-metric-tile">
                  <div class="dashboard-metric-label">VRAM</div>
                  <div class="dashboard-metric-value text-silver">{{ (gpu.vram_used_mb / 1024).toFixed(1) }} GB</div>
                  <div class="mt-1 text-xs text-storm">/ {{ (gpu.vram_total_mb / 1024).toFixed(0) }} GB</div>
                </div>
              </div>
            </section>
          </div>

          <div class="dashboard-live-side">
            <section class="surface-subtle p-4">
              <div class="flex items-center justify-between gap-3">
                <div class="eyebrow-label">{{ $t('dashboard.signal_snapshot') }}</div>
                <span class="inline-flex h-8 min-w-8 items-center justify-center rounded-full px-3 text-sm font-semibold" :class="qualityBadgeClass">
                  {{ qualityGrade }}
                </span>
              </div>
              <div class="dashboard-metric-grid">
                <div v-for="metric in primaryStreamMetrics" :key="metric.label" class="dashboard-metric-tile">
                  <div class="dashboard-metric-label">{{ metric.label }}</div>
                  <div class="dashboard-metric-value" :class="metric.color">{{ metric.value }}</div>
                </div>
              </div>
              <div class="dashboard-rail-footnote">
                {{ stats.codec?.toUpperCase() || '--' }} · {{ stats.width }}×{{ stats.height }}
              </div>
            </section>

            <section class="surface-subtle p-4 dashboard-context-card">
              <div class="flex items-center justify-between gap-3">
                <div class="eyebrow-label">Session context</div>
                <span class="meta-pill" :class="runtimeModeTone">{{ runtimeEffectiveMode }}</span>
              </div>
              <div class="dashboard-context-section">
                <div class="dashboard-context-header">
                  <span>{{ $t('dashboard.connected_clients') }}</span>
                  <button
                    v-if="connectedClientUuid"
                    @click="disconnectClient"
                    class="focus-ring dashboard-action-button dashboard-action-button-danger"
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
                    <div class="text-right text-[11px] text-storm">
                      <div>{{ client.latency_ms?.toFixed(0) || '--' }} ms</div>
                    </div>
                  </div>
                </div>
              </div>
              <div class="dashboard-context-section">
                <div class="dashboard-context-header">
                  <span>{{ $t('dashboard.runtime_path') }}</span>
                </div>
                <div class="dashboard-runtime-pill-grid">
                  <div v-for="row in runtimeSummaryRows" :key="row.label" class="dashboard-runtime-pill">
                    <span class="dashboard-runtime-label">{{ row.label }}</span>
                    <span class="text-sm font-medium" :class="row.tone">{{ row.value }}</span>
                  </div>
                </div>
                <div v-if="runtimePathNote" class="dashboard-rail-footnote" :class="runtimePathNoteTone">
                  {{ runtimePathNote }}
                </div>
              </div>
            </section>

            <section class="card p-4">
              <div class="flex items-center justify-between gap-3">
                <div>
                  <div class="eyebrow-label">{{ $t('dashboard.quick_controls') }}</div>
                  <div class="mt-2 text-sm leading-relaxed text-storm">{{ clientSettingsSyncCopy }}</div>
                </div>
                <div class="flex shrink-0 items-center gap-2">
                  <span class="meta-pill whitespace-nowrap" :class="clientSettingsSyncTone">{{ clientSettingsSyncLabel }}</span>
                  <router-link to="/config" class="text-[11px] font-medium text-ice no-underline hover:text-ice/80">
                    Settings
                  </router-link>
                </div>
              </div>
              <div class="mt-4">
                <QuickControls compact @change="handleQuickControlChange" />
              </div>
            </section>
          </div>
        </div>
      </section>
    </template>

    <!-- ═══ IDLE LAYOUT ═══ -->
    <template v-else>
      <!-- At-a-glance status cards -->
      <div class="grid grid-cols-2 gap-3 lg:grid-cols-4">
        <div class="surface-subtle p-4">
          <div class="eyebrow-label mb-1">{{ $t('dashboard.status') }}</div>
          <div class="text-lg font-bold text-green-400">{{ $t('dashboard.ready') }}</div>
          <div class="mt-1 text-xs text-storm">{{ pairedClients }} {{ $t('dashboard.clients_paired') }}</div>
        </div>
        <div class="surface-subtle p-4">
          <div class="eyebrow-label mb-1">{{ $t('dashboard.mode') }}</div>
          <div class="text-lg font-bold" :class="headlessEnabled ? 'text-accent' : 'text-silver'">{{ headlessEnabled ? $t('dashboard.headless') : $t('dashboard.windowed') }}</div>
          <div class="mt-1 text-xs text-storm">{{ headlessEnabled ? $t('dashboard.headless_desc') : $t('dashboard.windowed_desc') }}</div>
        </div>
        <div class="surface-subtle p-4">
          <div class="eyebrow-label mb-1">GPU</div>
          <div class="text-lg font-bold" :class="gpu?.temperature_c > 65 ? 'text-yellow-400' : 'text-green-400'">{{ gpu?.temperature_c || '--' }}°C</div>
          <div class="mt-1 text-xs text-storm">{{ gpu?.utilization_pct || 0 }}% load · {{ gpu?.power_draw_w?.toFixed(0) || '--' }}W</div>
        </div>
        <div class="surface-subtle p-4">
          <div class="eyebrow-label mb-1">AI</div>
          <div class="text-lg font-bold" :class="aiStatus?.enabled ? 'text-accent' : 'text-storm'">{{ aiStatus?.enabled ? $t('dashboard.active') : $t('dashboard.off') }}</div>
          <div class="mt-1 text-xs text-storm">{{ aiStatus?.cache_count || 0 }} {{ $t('dashboard.cached') }} · {{ sessionHistory.length }} {{ $t('dashboard.sessions') }}</div>
        </div>
      </div>

      <!-- GPU Gauges + Quick Controls -->
      <div class="grid grid-cols-1 items-start gap-4 xl:grid-cols-[minmax(0,1fr)_320px]">
        <div class="section-card space-y-5">
          <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
            <div>
              <div class="section-kicker">{{ $t('dashboard.stream_readiness') }}</div>
              <h2 class="section-title">{{ headlessEnabled ? $t('dashboard.headless') : $t('dashboard.windowed') }} {{ $t('dashboard.mode') }}</h2>
              <p class="section-copy">{{ headlessEnabled ? $t('dashboard.primary_ready_desc') : $t('dashboard.windowed_desc') }}</p>
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
              <GaugeArc :value="gpu.temperature_c" :max="100" unit="°C" label="Temp" :size="112"
                        :thresholds="[{ at: 0, color: '#22c55e' }, { at: 70, color: '#eab308' }, { at: 85, color: '#ef4444' }]" />
              <GaugeArc :value="gpu.utilization_pct" :max="100" unit="%" label="GPU" :size="112" />
              <GaugeArc :value="gpu.encoder_pct" :max="100" unit="%" label="NVENC" :size="112"
                        :thresholds="[{ at: 0, color: '#c8d6e5' }, { at: 60, color: '#eab308' }, { at: 85, color: '#ef4444' }]" />
              <GaugeArc :value="gpu.vram_used_mb / 1024" :max="gpu.vram_total_mb / 1024" unit="GB" label="VRAM" :size="112"
                        :thresholds="[{ at: 0, color: '#c8d6e5' }, { at: 70, color: '#eab308' }, { at: 90, color: '#ef4444' }]" />
            </div>
          </div>

          <div>
            <div class="flex items-start justify-between gap-3">
              <div>
                <div class="section-kicker">{{ $t('dashboard.ready_checks') }}</div>
                <div class="mt-2 text-sm text-storm">{{ $t('dashboard.ready_checks_desc') }}</div>
              </div>
              <span class="rounded-full border px-2.5 py-1 text-[10px] font-medium"
                    :class="readyChecksPassing === readyChecks.length ? 'border-green-500/30 bg-green-500/10 text-green-300' : 'border-amber-300/30 bg-amber-300/10 text-amber-200'">
                {{ readyChecksPassing }}/{{ readyChecks.length }} {{ $t('dashboard.ready_checks_pass') }}
              </span>
            </div>
            <div class="mt-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
              <router-link
                v-for="check in readyChecks"
                :key="check.key"
                :to="check.to"
                class="focus-ring rounded-xl border px-3 py-3 no-underline transition-[border-color,background-color,transform] duration-200 hover:-translate-y-0.5"
                :class="check.cardClass"
              >
                <div class="flex items-center justify-between gap-3">
                  <div class="text-sm font-medium text-silver">{{ check.label }}</div>
                  <span class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.18em]" :class="check.badgeClass">
                    {{ check.state }}
                  </span>
                </div>
                <div class="mt-2 text-[11px] text-storm">{{ check.detail }}</div>
                <div class="mt-3 text-[11px] font-medium text-ice/80">{{ $t('dashboard.open_fix') }}</div>
              </router-link>
            </div>
          </div>
        </div>
        <div class="section-card">
          <QuickControls @change="handleQuickControlChange" />
        </div>
      </div>

      <!-- Recent Games + Launch Deck -->
      <div class="grid grid-cols-1 gap-4 lg:grid-cols-2">
        <div class="section-card">
          <div class="flex items-center justify-between gap-3 mb-3">
            <div>
              <div class="section-kicker">{{ $t('dashboard.recent_games') }}</div>
              <div class="mt-2 text-sm text-storm">{{ recentApps.length ? $t('dashboard.recent_ready', { count: recentApps.length }) : $t('dashboard.no_games') }}</div>
            </div>
            <router-link to="/apps" class="focus-ring inline-flex h-8 items-center gap-1.5 rounded-lg border border-storm px-3 text-xs font-medium text-silver transition-[border-color,color,background-color] duration-200 hover:border-ice hover:text-ice no-underline">
              {{ $t('navbar.library') }}
            </router-link>
          </div>
          <div v-if="recentApps.length" class="space-y-1.5">
            <div v-for="app in recentApps" :key="app.uuid" class="flex items-center gap-3 p-1.5 rounded-lg hover:bg-ice/5 transition-colors">
              <div class="w-8 h-11 rounded bg-void/60 shrink-0 overflow-hidden">
                <img v-if="app['image-path']" :src="'./api/covers/image?name=' + encodeURIComponent(app.name)" class="w-full h-full object-cover" loading="lazy" @error="$event.target.style.display='none'" />
              </div>
              <div class="flex-1 min-w-0">
                <div class="text-sm text-silver truncate">{{ app.name }}</div>
                <div class="text-[10px] text-storm" v-if="app.source && app.source !== 'manual'">{{ app.source }}</div>
              </div>
            </div>
          </div>
          <div v-else class="text-sm text-storm py-6 text-center">{{ $t('dashboard.no_games') }}</div>
        </div>
        <div class="section-card">
          <div class="flex items-start justify-between gap-3 mb-4">
            <div>
              <div class="section-kicker">{{ $t('dashboard.launch_deck') }}</div>
              <div class="mt-2 text-sm text-storm max-w-md">{{ $t('dashboard.launch_deck_desc') }}</div>
            </div>
            <span class="px-2 py-1 rounded-full text-[10px] font-medium whitespace-nowrap" :class="headlessEnabled ? 'bg-accent/15 text-accent' : 'bg-storm/20 text-storm'">
              {{ headlessEnabled ? $t('dashboard.headless') : $t('dashboard.windowed') }}
            </span>
          </div>
          <div class="grid gap-2 sm:grid-cols-3">
            <router-link to="/apps" class="action-tile">
              <div class="text-sm font-medium text-silver">{{ $t('navbar.library') }}</div>
              <div class="mt-1 text-[11px] text-storm">{{ $t('dashboard.launch_deck_library_desc') }}</div>
            </router-link>
            <router-link to="/pin" class="action-tile">
              <div class="text-sm font-medium text-silver">{{ $t('navbar.pairing') }}</div>
              <div class="mt-1 text-[11px] text-storm">{{ $t('dashboard.launch_deck_pairing_desc') }}</div>
            </router-link>
            <router-link to="/config" class="action-tile">
              <div class="text-sm font-medium text-silver">{{ $t('navbar.settings') }}</div>
              <div class="mt-1 text-[11px] text-storm">{{ $t('dashboard.launch_deck_settings_desc') }}</div>
            </router-link>
          </div>
          <div class="surface-subtle mt-4 px-3 py-3">
            <div class="eyebrow-label">{{ $t('dashboard.host_context') }}</div>
            <div class="mt-3 flex flex-wrap gap-2 text-[11px] text-silver">
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
            <router-link to="/info" class="mt-3 inline-flex text-[11px] font-medium text-ice no-underline hover:text-ice/80">
              {{ $t('dashboard.host_context_desc') }}
            </router-link>
          </div>
        </div>
      </div>
    </template>

    <!-- Session History (idle) -->
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
  </div>
</template>

<script setup>
import { ref, computed, watch, onMounted, onUnmounted, nextTick, reactive } from 'vue'
import { useStreamStats } from '../composables/useStreamStats'
import { useSystemStats } from '../composables/useSystemStats'
import { useSessionHistory, formatDuration } from '../composables/useSessionHistory'
import { useAiOptimizer } from '../composables/useAiOptimizer'
import { useFavicon } from '../composables/useFavicon'
import Skeleton from '../components/Skeleton.vue'
import GaugeArc from '../components/GaugeArc.vue'
import QuickControls from '../components/QuickControls.vue'
import { useI18n } from 'vue-i18n'
import { resolveClientSettingsSync } from '../client-settings-sync'

const { stats } = useStreamStats(1000)
const { gpu, displays, audio, sessionType } = useSystemStats(3000)
const { sessions, clearHistory } = useSessionHistory(stats)
const { status: aiStatus, fetchStatus: fetchAiStatus, fetchDevices: fetchAiDevices, getSuggestion: getAiSuggestion } = useAiOptimizer()

// AI optimization state for current stream
const aiOptimization = ref(null)
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

const actionSummary = computed(() => {
  if (!statsLoaded.value) return t('dashboard.loading_summary')
  if (stats.value?.streaming) return t('dashboard.streaming_summary')
  return t('dashboard.idle_summary', { count: pairedClients.value })
})

const primaryFocus = computed(() => {
  if (stats.value?.streaming) {
    return { title: t('dashboard.primary_stream_title'), desc: t('dashboard.primary_stream_desc') }
  }
  if (!pairedClients.value) {
    return { title: t('dashboard.primary_pair_title'), desc: t('dashboard.primary_pair_desc') }
  }
  return { title: t('dashboard.primary_ready_title'), desc: t('dashboard.primary_ready_desc') }
})

const readinessLabel = computed(() => {
  if (stats.value?.streaming) return t('dashboard.readiness_live')
  if (pairedClients.value > 0) return t('dashboard.readiness_ready')
  return t('dashboard.readiness_setup')
})

const readinessTone = computed(() => {
  if (stats.value?.streaming || pairedClients.value > 0) return 'text-green-400'
  return 'text-amber-300'
})

const readinessDetail = computed(() => {
  if (stats.value?.streaming) return t('dashboard.readiness_live_desc')
  if (pairedClients.value > 0) return t('dashboard.readiness_ready_desc')
  return t('dashboard.readiness_setup_desc')
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
      ? 'border-green-500/15 bg-green-500/5 hover:border-green-500/30 hover:bg-green-500/8'
      : 'border-amber-300/15 bg-amber-300/5 hover:border-amber-300/30 hover:bg-amber-300/8',
    badgeClass: item.ok ? 'bg-green-500/10 text-green-300' : 'bg-amber-300/10 text-amber-200'
  }))
})

const readyChecksPassing = computed(() => readyChecks.value.filter((item) => item.ok).length)

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

const clientSettingsSyncLabel = computed(() => {
  if (!clientSettingsSync.value.available) return 'Sync unavailable'
  if (clientSettingsSync.value.relaunchRequired) return 'Sync pending'
  return 'Nova sync ready'
})

const clientSettingsSyncTone = computed(() => {
  if (!clientSettingsSync.value.available || clientSettingsSync.value.relaunchRequired) {
    return 'border-amber-300/30 bg-amber-300/10 text-amber-200'
  }
  return 'border-green-400/30 bg-green-400/10 text-green-300'
})

const clientSettingsSyncCopy = computed(() => {
  if (!clientSettingsSync.value.available) {
    return 'Nova-facing client settings are unavailable on this Polaris build.'
  }
  if (clientSettingsSync.value.relaunchRequired) {
    return 'A requested display mode is saved and will become active after the stream relaunches.'
  }
  return 'Nova-facing controls are available for live bitrate, Adaptive Bitrate, AI Optimizer, and next-stream display choices.'
})

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
  const count = connectedClients.value.length || 1
  return `${count} ${count === 1 ? 'viewer' : 'viewers'}`
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
    windowed_dmabuf_override: 'Windowed private compositor is preserving the DMA-BUF/CUDA capture path.',
    headless_shm_fallback: 'Headless Stream is using SHM/system-memory capture. The stream can be healthy, but high-FPS NVIDIA testing needs the GPU-native path.',
    headless_shm_default: 'Headless Stream is using SHM/system-memory capture. The stream can be healthy, but high-FPS NVIDIA testing needs the GPU-native path.',
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

function metricNumber(value) {
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : 0
}

function modeLabelFromBool(value) {
  return value ? 'Headless Stream' : 'Windowed Stream'
}

function streamDisplayModeLabel(statsPayload) {
  const explicitMode = statsPayload?.stream_display_mode
  if (explicitMode) return explicitMode
  return modeLabelFromBool(Boolean(statsPayload?.runtime_effective_headless))
}

const runtimeBackendLabel = computed(() => {
  return titleizeToken(stats.value?.runtime_backend || 'unknown')
})

const runtimeRequestedMode = computed(() => {
  if (!stats.value?.streaming) return '--'
  return modeLabelFromBool(Boolean(stats.value?.runtime_requested_headless))
})

const runtimeEffectiveMode = computed(() => {
  if (!stats.value?.streaming) return '--'
  return streamDisplayModeLabel(stats.value)
})

const runtimeModeTone = computed(() => {
  if (!stats.value?.streaming) return 'bg-storm/20 text-storm'
  const mode = String(runtimeEffectiveMode.value || '').toLowerCase()
  if (mode.includes('headless stream')) return 'bg-accent/15 text-accent'
  if (mode.includes('windowed stream') || mode.includes('host virtual')) return 'bg-amber-500/15 text-amber-300'
  return 'bg-storm/20 text-storm'
})

const runtimeEffectiveTone = computed(() => {
  if (!stats.value?.streaming) return 'text-storm'
  const mode = String(runtimeEffectiveMode.value || '').toLowerCase()
  if (mode.includes('headless stream')) return 'text-accent'
  if (mode.includes('windowed stream') || mode.includes('host virtual')) return 'text-amber-300'
  return 'text-silver'
})

const runtimeOverrideLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return stats.value?.runtime_gpu_native_override_active ? 'Active' : 'Inactive'
})

const runtimeOverrideTone = computed(() => {
  if (!stats.value?.streaming) return 'text-storm'
  return stats.value?.runtime_gpu_native_override_active ? 'text-amber-300' : 'text-green-400'
})

const headlessGpuNativeOverrideActive = computed(() => {
  if (!stats.value?.streaming) return false
  const effectiveKnown = stats.value?.runtime_effective_headless !== undefined && stats.value?.runtime_effective_headless !== null
  return Boolean(stats.value?.runtime_requested_headless) &&
    effectiveKnown &&
    !Boolean(stats.value?.runtime_effective_headless) &&
    Boolean(stats.value?.runtime_gpu_native_override_active)
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

const captureResidencyLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return titleizeToken(stats.value?.capture_residency || 'unknown')
})

const captureFormatLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  const format = stats.value?.capture_format
  if (!format) return '--'
  return String(format).toUpperCase()
})

const capturePathLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return titleizeToken(stats.value?.capture_path || 'unknown')
})

const captureReasonLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return captureReasonMessage(stats.value?.capture_path_reason)
})

const captureCpuCopyLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return stats.value?.capture_cpu_copy ? 'CPU copy' : 'No CPU copy'
})

const captureCpuCopyTone = computed(() => {
  if (!stats.value?.streaming) return 'text-storm'
  return stats.value?.capture_cpu_copy ? 'text-orange-300' : 'text-green-400'
})

const captureGpuNativeLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return stats.value?.capture_gpu_native ? 'GPU native' : 'Mixed path'
})

const captureGpuNativeTone = computed(() => {
  if (!stats.value?.streaming) return 'text-storm'
  return stats.value?.capture_gpu_native ? 'text-green-400' : 'text-amber-300'
})

const encodeTargetLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  const device = stats.value?.encode_target_device || 'unknown'
  const residency = stats.value?.encode_target_residency || 'unknown'
  const format = stats.value?.encode_target_format || 'unknown'
  return `${titleizeToken(device)} / ${titleizeToken(residency)} / ${String(format).toUpperCase()}`
})

const encodeTargetTone = computed(() => {
  if (!stats.value?.streaming) return 'text-storm'
  return String(stats.value?.encode_target_residency || '').toLowerCase() === 'gpu'
    ? 'text-green-400'
    : 'text-orange-300'
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
  if (nestedLabwcShmFallbackActive.value) return 'text-orange-300'
  if (stats.value?.capture_cpu_copy) return 'text-orange-300'
  if (stats.value?.capture_gpu_native) return 'text-green-400'
  return 'text-amber-300'
})

const fpsTargetGap = computed(() => {
  if (!stats.value?.streaming) return null
  const encoded = metricNumber(stats.value?.fps)
  const target = metricNumber(stats.value?.session_target_fps || stats.value?.requested_client_fps)

  if (target < 90 || encoded <= 0 || encoded >= target * 0.85) return null
  return { encoded, target }
})

const streamPathNotices = computed(() => {
  if (!stats.value?.streaming) return []

  const notices = []
  const transport = captureTransportLabel.value
  const residency = captureResidencyLabel.value
  const encodeTarget = encodeTargetLabel.value

  if (headlessGpuNativeOverrideActive.value) {
    notices.push({
      key: 'gpu-native-override',
      title: 'GPU-native override',
      message: 'Polaris requested headless, but is running windowed labwc so DMA-BUF/CUDA capture can stay GPU-resident.',
      surfaceClass: 'border-amber-300/25 bg-amber-300/10',
      titleClass: 'text-amber-200',
    })
  }

  if (stats.value?.capture_gpu_native) {
    notices.push({
      key: 'gpu-native-active',
      title: 'GPU-native path active',
      message: `${captureReasonLabel.value} Capture is ${transport}/${residency} and encode target is ${encodeTarget}.`,
      surfaceClass: 'border-green-400/25 bg-green-400/10',
      titleClass: 'text-green-300',
    })
  } else if (stats.value?.capture_cpu_copy) {
    notices.push({
      key: 'cpu-copy-active',
      title: 'CPU copy path active',
      message: `${captureReasonLabel.value} Capture is ${transport}/${residency}.`,
      surfaceClass: 'border-orange-300/25 bg-orange-300/10',
      titleClass: 'text-orange-200',
    })
  }

  if (fpsTargetGap.value) {
    notices.push({
      key: 'fps-target-gap',
      title: 'FPS target gap',
      message: `Client target is ${fpsTargetGap.value.target.toFixed(0)} FPS; encoder is averaging ${fpsTargetGap.value.encoded.toFixed(1)} FPS. Check game caps, VSync, or launch flags before treating this as a capture fallback.`,
      surfaceClass: 'border-ice/25 bg-ice/10',
      titleClass: 'text-ice',
    })
  }

  return notices
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

// Check if a specific client name has AI-optimized settings (for multi-viewer list)
function isClientAiOptimized(clientName) {
  return aiCacheKeys.value.some(key => key.startsWith(clientName + ':'))
}

function gradeColor(grade) {
  if (grade === 'A') return 'text-green-400'
  if (grade === 'B') return 'text-ice'
  if (grade === 'C') return 'text-yellow-400'
  if (grade === 'D') return 'text-orange-400'
  return 'text-red-400'
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
let previewTimer = null

function startPreview() {
  previewLoaded.value = false
  previewError.value = false
  previewBackoffMs.value = PREVIEW_REFRESH_MS
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
  previewUrl.value = `./api/display/screenshot?t=${Date.now()}${output}`
}

function handlePreviewLoad() {
  previewLoaded.value = true
  previewError.value = false
  previewBackoffMs.value = PREVIEW_REFRESH_MS
  schedulePreviewRefresh(PREVIEW_REFRESH_MS)
}

function handlePreviewError() {
  previewLoaded.value = false
  previewError.value = true
  previewBackoffMs.value = Math.min(
    Math.max(PREVIEW_FAILURE_BACKOFF_MS, previewBackoffMs.value * 2),
    PREVIEW_MAX_BACKOFF_MS,
  )
  schedulePreviewRefresh(previewBackoffMs.value)
}

function retryPreviewNow() {
  previewBackoffMs.value = PREVIEW_REFRESH_MS
  refreshPreview()
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

// Stream quality scoring (0-100, computed from live stats)
const qualityScore = computed(() => {
  if (!stats.value?.streaming) return 0
  const s = stats.value
  let score = 100

  // FPS stability: penalize if below target (assume 60fps baseline)
  if (s.fps < 55) score -= Math.min(30, (60 - s.fps) * 2)
  else if (s.fps < 58) score -= 5

  // Encode time: penalize if above 8ms (1 frame at 120fps)
  if (s.encode_time_ms > 16) score -= 25
  else if (s.encode_time_ms > 8) score -= Math.min(15, (s.encode_time_ms - 8) * 2)

  // Latency: penalize above 20ms
  if (s.latency_ms > 50) score -= 20
  else if (s.latency_ms > 20) score -= Math.min(10, (s.latency_ms - 20) / 3)

  // Packet loss: heavily penalize
  if (s.packet_loss > 5) score -= 30
  else if (s.packet_loss > 1) score -= s.packet_loss * 5
  else if (s.packet_loss > 0) score -= 3

  return Math.max(0, Math.min(100, Math.round(score)))
})

const qualityGrade = computed(() => {
  const s = qualityScore.value
  if (s >= 90) return 'A'
  if (s >= 75) return 'B'
  if (s >= 55) return 'C'
  if (s >= 35) return 'D'
  return 'F'
})

// Stream metrics for the left column (compact vertical list)
const streamMetrics = computed(() => {
  if (!stats.value?.streaming) return []
  const s = stats.value
  const fpsColor = s.fps >= 55 ? 'text-green-400' : s.fps >= 30 ? 'text-yellow-400' : 'text-red-400'
  const latColor = s.latency_ms <= 20 ? 'text-green-400' : s.latency_ms <= 50 ? 'text-yellow-400' : 'text-red-400'
  const lossColor = s.packet_loss < 0.5 ? 'text-green-400' : s.packet_loss < 2 ? 'text-yellow-400' : 'text-red-400'
  return [
    { label: 'FPS', value: s.fps.toFixed(1), color: fpsColor },
    { label: 'RTT', value: s.latency_ms.toFixed(0) + 'ms', color: latColor },
    { label: 'Bitrate', value: (s.bitrate_kbps / 1000).toFixed(1) + ' Mbps', color: 'text-silver' },
    { label: 'Encode', value: s.encode_time_ms.toFixed(1) + 'ms', color: 'text-silver' },
    { label: 'Codec', value: s.codec?.toUpperCase(), color: 'text-accent' },
    { label: 'Resolution', value: s.width + '×' + s.height, color: 'text-silver' },
    { label: 'Pkt Loss', value: s.packet_loss.toFixed(1) + '%', color: lossColor },
    { label: 'Sent', value: formatBytes(s.bytes_sent), color: 'text-storm' },
  ]
})

const primaryStreamMetrics = computed(() => {
  const labels = new Set(['FPS', 'RTT', 'Bitrate', 'Encode'])
  return streamMetrics.value.filter((metric) => labels.has(metric.label))
})

const previewHeadline = computed(() => (
  previewExpanded.value
    ? t('dashboard.preview_headline_expanded')
    : t('dashboard.preview_headline')
))

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

const aiOptimizationSummary = computed(() => {
  const reasoning = aiOptimization.value?.reasoning
  if (!reasoning) return ''
  const firstSentence = String(reasoning).split(/(?<=[.!?])\s+/)[0]
  return firstSentence || String(reasoning)
})

const runtimeSummaryRows = computed(() => ([
  { label: 'Backend', value: runtimeBackendLabel.value, tone: 'text-silver' },
  { label: 'Path', value: capturePathLabel.value, tone: captureGpuNativeTone.value },
  { label: 'Transport', value: captureTransportLabel.value, tone: 'text-silver' },
  { label: 'Format', value: captureFormatLabel.value, tone: 'text-silver' },
  { label: 'Residency', value: captureResidencyLabel.value, tone: 'text-silver' },
  { label: 'Encode', value: encodeTargetLabel.value, tone: encodeTargetTone.value },
  { label: 'Copy', value: captureCpuCopyLabel.value, tone: captureCpuCopyTone.value },
  { label: 'Native', value: captureGpuNativeLabel.value, tone: captureGpuNativeTone.value },
  { label: 'Requested', value: runtimeRequestedMode.value, tone: 'text-silver' },
  { label: 'Effective', value: runtimeEffectiveMode.value, tone: runtimeEffectiveTone.value },
  { label: 'Override', value: runtimeOverrideLabel.value, tone: runtimeOverrideTone.value },
]))

// qualityGrade, qualityScore already defined above — reuse them
const qualityBadgeClass = computed(() => {
  const g = qualityGrade.value
  return {
    'bg-green-500/20 text-green-400': g === 'A',
    'bg-blue-500/20 text-blue-400': g === 'B',
    'bg-yellow-500/20 text-yellow-400': g === 'C',
    'bg-orange-500/20 text-orange-400': g === 'D',
    'bg-red-500/20 text-red-400': g === 'F' || g === '-',
  }
})

// Optimization recommendations (computed from live stats)
const recommendations = computed(() => {
  if (!stats.value?.streaming) return []
  const s = stats.value
  const recs = []

  if (s.encode_time_ms > 12)
    recs.push({ color: 'text-yellow-400', message: `Encode time is ${s.encode_time_ms.toFixed(1)}ms — consider lowering resolution or switching to NVENC Low Latency preset.` })
  else if (s.encode_time_ms > 8)
    recs.push({ color: 'text-ice', message: `Encode time is ${s.encode_time_ms.toFixed(1)}ms — approaching limit for 120fps. Monitor for frame drops.` })

  if (s.packet_loss > 2)
    recs.push({ color: 'text-red-400', message: `Packet loss at ${s.packet_loss}% — try enabling FEC, lowering bitrate, or checking network quality.` })
  else if (s.packet_loss > 0.5)
    recs.push({ color: 'text-yellow-400', message: `Minor packet loss (${s.packet_loss.toFixed(1)}%) — stream may have occasional artifacts.` })

  if (fpsTargetGap.value)
    recs.push({ color: 'text-ice', message: `Client requested ${fpsTargetGap.value.target.toFixed(0)} FPS, but encode is near ${fpsTargetGap.value.encoded.toFixed(1)} FPS. Check the game's frame cap, VSync, or idle/menu throttling first.` })
  else if (s.fps < 55 && s.fps > 0)
    recs.push({ color: 'text-yellow-400', message: `FPS dropped to ${s.fps.toFixed(1)} — GPU may be overloaded. Consider lowering resolution or quality.` })

  if (s.latency_ms > 40)
    recs.push({ color: 'text-yellow-400', message: `Latency is ${s.latency_ms.toFixed(0)}ms — check network path or try wired connection.` })

  if (fpsTargetGap.value && gpu.value && gpu.value.utilization_pct < 30 && s.encode_time_ms < 4)
    recs.push({ color: 'text-green-400', message: `GPU and encoder headroom are available (${gpu.value.utilization_pct}% GPU, ${s.encode_time_ms.toFixed(1)}ms encode), so this looks more like an app/output cap than encoder pressure.` })
  else if (gpu.value && gpu.value.utilization_pct < 30 && s.encode_time_ms < 4)
    recs.push({ color: 'text-green-400', message: `GPU utilization is low (${gpu.value.utilization_pct}%) with fast encode — you have headroom for higher resolution or quality.` })

  // Stream display mode recommendations
  if (!s.headless_mode && s.streaming)
    recs.push({ color: 'text-accent', message: `Use Headless Stream in Audio/Video settings for hidden stream-only sessions that leave the desktop layout alone.` })

  if (headlessGpuNativeOverrideActive.value)
    recs.push({ color: 'text-amber-300', message: `GPU-native override is active: Polaris requested headless but is using windowed labwc to preserve the GPU-resident capture path.` })

  if (s.capture_cpu_copy)
    recs.push({ color: 'text-orange-300', message: captureReasonLabel.value })

  // AI optimizer recommendations
  if (!s.ai_enabled && s.streaming)
    recs.push({ color: 'text-accent', message: `Enable AI Optimizer in settings for per-game encoding tuning — auto-adjusts resolution, bitrate, and codec.` })

  // Adaptive bitrate recommendation
  if (s.adaptive_target_bitrate_kbps === 0 && s.packet_loss > 0 && s.streaming)
    recs.push({ color: 'text-accent', message: `Enable Adaptive Bitrate in Audio/Video settings — auto-adjusts bitrate when network quality drops.` })

  return recs
})

const primaryRecommendations = computed(() => recommendations.value.slice(0, 2))

// Recording controls
const recording = ref({ active: false, file: '' })

async function fetchRecordingStatus() {
  try {
    const res = await fetch('./api/recording/status', { credentials: 'include' })
    if (res.ok) recording.value = await res.json()
  } catch {}
}

async function startRecording() {
  await fetch('./api/recording/start', { method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/json' } })
  fetchRecordingStatus()
}

async function stopRecording() {
  await fetch('./api/recording/stop', { method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/json' } })
  fetchRecordingStatus()
}

async function saveReplay() {
  await fetch('./api/recording/save-replay', { method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/json' } })
  fetchRecordingStatus()
}

// Connected client disconnect
const connectedClientUuid = ref(null)

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
  try {
    await fetch('./api/clients/disconnect', {
      method: 'POST', credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ uuid: connectedClientUuid.value })
    })
    connectedClientUuid.value = null
  } catch (e) { console.error(e) }
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
const uPlotLoaded = ref(false)

async function loadUPlot() {
  if (!uPlotLib) {
    const mod = await import('uplot')
    await import('uplot/dist/uPlot.min.css')
    uPlotLib = mod.default
    uPlotLoaded.value = true
  }
  return uPlotLib
}

// Track whether we've gotten at least one stats response
const statsLoaded = ref(false)

// System info from config
const systemInfo = reactive({
  gpuName: '',
  gpuDriver: '',
  version: ''
})
const pairedClientCount = ref(0)

// Fetch system info
async function fetchSystemInfo() {
  try {
    const [configRes, clientsRes] = await Promise.all([
      fetch('./api/config', { credentials: 'include' }),
      fetch('./api/clients/list', { credentials: 'include' })
    ])
    if (configRes.ok) {
      const config = await configRes.json()
      refreshClientSettingsSync(config)
      systemInfo.gpuName = config.adapter_name || ''
      systemInfo.gpuDriver = config.gpu_driver || ''
      systemInfo.version = config.version || ''
      streamingOutput.value = config.linux_streaming_output || config.output_name || ''
      discoveryEnabled.value = config.enable_discovery !== 'disabled'
      pairingEnabled.value = config.enable_pairing !== 'disabled'
    }
    if (clientsRes.ok) {
      const clients = await clientsRes.json()
      pairedClientCount.value = clients.named_certs?.length || 0
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

function formatBytes(bytes) {
  if (!bytes || bytes === 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  const i = Math.floor(Math.log(bytes) / Math.log(1024))
  return (bytes / Math.pow(1024, i)).toFixed(1) + ' ' + units[i]
}

function makeChartOpts(title, suffix, color = '#c8d6e5') {
  const r = parseInt(color.slice(1,3), 16)
  const g = parseInt(color.slice(3,5), 16)
  const b = parseInt(color.slice(5,7), 16)
  return {
    width: 300,
    height: 96,
    cursor: { show: false },
    legend: { show: false },
    axes: [
      {
        stroke: '#4c5265',
        grid: { stroke: '#4c526520', width: 1 },
        ticks: { show: false },
        font: '9px sans-serif',
        values: () => [],
      },
      {
        stroke: '#687b81',
        grid: { stroke: '#4c526520', width: 1 },
        ticks: { stroke: '#4c5265', width: 1 },
        font: '9px sans-serif',
        size: 35,
      },
    ],
    series: [
      {},
      {
        stroke: color,
        width: 1.5,
        fill: `rgba(${r}, ${g}, ${b}, 0.06)`,
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
  nextTick(() => {
    if (fpsChartEl.value && !fpsChart) {
      fpsChart = initChart(fpsChartEl.value, makeChartOpts('FPS', 'fps', '#4ade80'))
    }
    if (bitrateChartEl.value && !bitrateChart) {
      bitrateChart = initChart(bitrateChartEl.value, makeChartOpts('Bitrate', 'Mbps', '#38bdf8'))
    }
    if (latencyChartEl.value && !latencyChart) {
      latencyChart = initChart(latencyChartEl.value, makeChartOpts('Latency', 'ms', '#fbbf24'))
    }
    if (gpuChartEl.value && !gpuChart) {
      gpuChart = initChart(gpuChartEl.value, makeChartOpts('GPU', '%', '#c084fc'))
    }
    if (lossChartEl.value && !lossChart) {
      lossChart = initChart(lossChartEl.value, makeChartOpts('Loss', '%', '#f87171'))
    }
    if (encodeChartEl.value && !encodeChart) {
      encodeChart = initChart(encodeChartEl.value, makeChartOpts('Encode', 'ms'))
    }
  })
}

function destroyCharts() {
  if (fpsChart) { fpsChart.destroy(); fpsChart = null }
  if (bitrateChart) { bitrateChart.destroy(); bitrateChart = null }
  if (encodeChart) { encodeChart.destroy(); encodeChart = null }
  if (latencyChart) { latencyChart.destroy(); latencyChart = null }
  if (gpuChart) { gpuChart.destroy(); gpuChart = null }
  if (lossChart) { lossChart.destroy(); lossChart = null }
  timestamps.value = []
  fpsHistory.value = []
  bitrateHistory.value = []
  encodeHistory.value = []
  latencyHistory.value = []
  gpuHistory.value = []
  lossHistory.value = []
}

watch(stats, (newStats, oldStats) => {
  statsLoaded.value = true

  if (!newStats || !newStats.streaming) {
    if (oldStats?.streaming) {
      stopPreview()
    }
    destroyCharts()
    connectedClientUuid.value = null
    aiOptimization.value = null
    return
  }

  // Resolve client UUID, recording status, AI optimization, and auto-show preview when streaming starts
  if (newStats.streaming && (!oldStats || !oldStats.streaming)) {
    resolveConnectedClient()
    fetchRecordingStatus()
    if (!showPreview.value) startPreview()
    // Fetch AI optimization for connected device
    if (newStats.client_name) {
      getAiSuggestion(newStats.client_name).then(opt => {
        if (opt && opt.status) aiOptimization.value = opt
      })
    }
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

onMounted(async () => {
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
      pairedClients.value = (data.named_certs || []).length
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
  destroyCharts()
  if (resizeObserver) {
    resizeObserver.disconnect()
    resizeObserver = null
  }
})
</script>
