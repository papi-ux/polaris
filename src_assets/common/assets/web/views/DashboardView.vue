<template>
  <div class="space-y-4 relative">

    <!-- Header row -->
    <div class="flex items-center justify-between">
      <div>
        <h1 class="text-2xl font-bold text-silver">{{ $t('dashboard.title') }}</h1>
        <div class="text-sm text-storm mt-1">{{ actionSummary }}</div>
      </div>
      <div class="flex items-center gap-2 text-xs text-storm">
        <span v-if="stats?.streaming" class="pulse-dot"></span>
        <span :class="stats?.streaming ? 'text-green-400 font-medium' : 'text-storm'">
          {{ stats?.streaming ? $t('dashboard.live') : $t('dashboard.standby') }}
        </span>
      </div>
    </div>

    <div v-if="statsLoaded" class="grid grid-cols-1 lg:grid-cols-3 gap-3">
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">{{ $t('dashboard.primary_focus') }}</div>
        <div class="text-sm text-silver font-medium mt-1">{{ primaryFocus.title }}</div>
        <div class="text-[10px] text-storm mt-1">{{ primaryFocus.desc }}</div>
      </div>
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">{{ $t('dashboard.stream_readiness') }}</div>
        <div class="text-sm font-medium mt-1" :class="readinessTone">{{ readinessLabel }}</div>
        <div class="text-[10px] text-storm mt-1">{{ readinessDetail }}</div>
      </div>
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">{{ $t('dashboard.next_step') }}</div>
        <div class="text-sm text-silver font-medium mt-1">{{ nextStep.title }}</div>
        <div class="text-[10px] text-storm mt-1">{{ nextStep.desc }}</div>
      </div>
    </div>

    <!-- Loading skeleton state -->
    <template v-if="!statsLoaded">
      <div class="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <Skeleton type="card" v-for="n in 4" :key="n" />
      </div>
    </template>

    <!-- ═══ STREAMING LAYOUT: 3-column on desktop ═══ -->
    <template v-else-if="stats?.streaming">
      <div class="grid grid-cols-1 lg:grid-cols-[280px_1fr_280px] gap-4">

        <!-- LEFT COLUMN: Metrics -->
        <div class="space-y-3">
          <!-- Connected clients -->
          <div class="card p-3">
            <div class="flex items-center justify-between mb-1.5">
              <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">
                {{ (stats.clients?.length || 1) > 1 ? (stats.clients.length + ' Viewers') : 'Client' }}
              </div>
              <button v-if="connectedClientUuid" @click="disconnectClient"
                      class="h-5 px-1.5 text-[9px] font-medium rounded border border-red-500/30 text-red-400 hover:bg-red-500/10 transition">
                Disconnect
              </button>
            </div>
            <!-- Multi-client list (from stats.clients[]) -->
            <template v-if="stats.clients?.length > 0">
              <div v-for="(c, i) in stats.clients" :key="i"
                   class="flex items-center justify-between py-1" :class="i > 0 ? 'border-t border-storm/15' : ''">
                <div>
                  <div class="text-xs font-medium text-silver">{{ c.name }}
                    <span v-if="isClientAiOptimized(c.name)" class="inline-flex items-center gap-0.5 ml-1 px-1 py-0 rounded text-[8px] font-medium bg-accent/15 text-accent">AI</span>
                  </div>
                  <div class="text-[9px] text-storm">{{ c.ip }}</div>
                </div>
                <div class="text-[9px] text-storm text-right">
                  <div>{{ c.fps?.toFixed(0) || '--' }} fps</div>
                  <div>{{ c.latency_ms?.toFixed(0) || '--' }} ms</div>
                </div>
              </div>
            </template>
            <!-- Fallback: single client (when stats.clients[] not available) -->
            <div v-else class="flex items-center justify-between">
              <div>
                <div class="text-xs font-medium text-silver">{{ stats.client_name }}
                  <span v-if="aiOptimizedClient" class="inline-flex items-center gap-0.5 ml-1 px-1 py-0 rounded text-[8px] font-medium bg-accent/15 text-accent">AI</span>
                </div>
                <div class="text-[9px] text-storm">{{ stats.client_ip }}</div>
              </div>
            </div>
          </div>

          <div class="card p-3">
            <div class="flex items-center justify-between mb-1.5">
              <div class="text-[10px] font-semibold text-storm uppercase tracking-wider">Runtime Path</div>
              <span class="px-1.5 py-0.5 rounded text-[9px] font-medium" :class="runtimeModeTone">
                {{ runtimeEffectiveMode }}
              </span>
            </div>
            <div class="space-y-1.5">
              <div class="flex items-center justify-between">
                <span class="text-[10px] text-storm uppercase tracking-wider">Backend</span>
                <span class="text-xs text-silver font-medium">{{ runtimeBackendLabel }}</span>
              </div>
              <div class="flex items-center justify-between">
                <span class="text-[10px] text-storm uppercase tracking-wider">Requested</span>
                <span class="text-xs text-silver font-medium">{{ runtimeRequestedMode }}</span>
              </div>
              <div class="flex items-center justify-between">
                <span class="text-[10px] text-storm uppercase tracking-wider">Effective</span>
                <span class="text-xs font-medium" :class="runtimeEffectiveTone">{{ runtimeEffectiveMode }}</span>
              </div>
              <div class="flex items-center justify-between">
                <span class="text-[10px] text-storm uppercase tracking-wider">Override</span>
                <span class="text-xs font-medium" :class="runtimeOverrideTone">{{ runtimeOverrideLabel }}</span>
              </div>
              <div class="flex items-center justify-between">
                <span class="text-[10px] text-storm uppercase tracking-wider">Capture</span>
                <span class="text-xs text-silver font-medium">{{ captureTransportLabel }}</span>
              </div>
              <div class="flex items-center justify-between">
                <span class="text-[10px] text-storm uppercase tracking-wider">Residency</span>
                <span class="text-xs text-silver font-medium">{{ captureResidencyLabel }}</span>
              </div>
              <div class="flex items-center justify-between">
                <span class="text-[10px] text-storm uppercase tracking-wider">Format</span>
                <span class="text-xs text-silver font-medium">{{ captureFormatLabel }}</span>
              </div>
            </div>
            <div v-if="runtimePathNote" class="text-[10px] mt-2" :class="runtimePathNoteTone">
              {{ runtimePathNote }}
            </div>
          </div>

          <!-- Metrics grid -->
          <div class="card p-3 space-y-2">
            <div class="flex items-center justify-between" v-for="m in streamMetrics" :key="m.label">
              <span class="text-[10px] text-storm uppercase tracking-wider">{{ m.label }}</span>
              <span class="text-sm font-medium" :class="m.color">{{ m.value }}</span>
            </div>
          </div>

          <!-- Quality score -->
          <div class="card p-3">
            <div class="flex items-center gap-3">
              <div class="w-10 h-10 rounded-full flex items-center justify-center text-lg font-bold"
                   :class="qualityBadgeClass">{{ qualityGrade }}</div>
              <div>
                <div class="text-xs text-storm">Stream Quality</div>
                <div class="text-sm text-silver font-medium">{{ qualityScore }}/100</div>
              </div>
            </div>
          </div>
        </div>

        <!-- CENTER COLUMN: Preview + Charts -->
        <div class="space-y-3">
          <!-- Display preview -->
          <div class="card p-3" v-if="showPreview">
            <div class="w-full aspect-video bg-void rounded-lg overflow-hidden flex items-center justify-center" v-if="!previewLoaded">
              <svg class="w-6 h-6 animate-spin text-storm" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
            </div>
            <img :src="previewUrl" class="w-full aspect-video object-contain bg-void rounded-lg" :class="{ 'hidden': !previewLoaded }" @load="previewLoaded = true" />
            <div class="flex items-center justify-between mt-2">
              <span class="text-[10px] text-storm">{{ stats.width }}x{{ stats.height }} · {{ stats.codec }}</span>
              <button @click="stopPreview" class="text-[10px] text-storm hover:text-ice transition">Hide</button>
            </div>
          </div>
          <div v-else class="card p-3">
            <button @click="startPreview" class="w-full text-center text-xs text-storm hover:text-ice transition py-4">
              Show Display Preview
            </button>
          </div>

          <!-- Charts are rendered in the old section below (refs bind there) -->

          <!-- Recommendations -->
          <div v-if="recommendations.length" class="space-y-1.5">
            <div class="text-[10px] font-semibold text-storm uppercase tracking-wider mb-2">{{ $t('dashboard.recommendations') }}</div>
            <div v-for="(rec, i) in recommendations" :key="i"
                 class="glass rounded-lg px-3 py-2 flex items-center gap-2 text-xs">
              <svg class="w-3.5 h-3.5 shrink-0" :class="rec.color" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/>
              </svg>
              <span class="text-silver flex-1">{{ rec.message }}</span>
            </div>
          </div>
        </div>

        <!-- RIGHT COLUMN: Controls -->
        <div class="space-y-3">
          <div class="card p-3">
            <QuickControls />
          </div>

          <!-- AI Optimization -->
          <div class="card p-3" v-if="aiOptimization">
            <div class="text-[10px] font-semibold text-storm uppercase tracking-wider mb-2">{{ $t('dashboard.ai_optimization') }}</div>
            <div class="text-xs text-silver">{{ aiOptimization.reasoning }}</div>
            <div class="flex flex-wrap gap-1.5 mt-2">
              <span class="px-1.5 py-0.5 rounded text-[9px] bg-accent/10 text-accent">{{ aiOptimization.source }}</span>
              <span v-if="aiOptimization.display_mode" class="px-1.5 py-0.5 rounded text-[9px] bg-storm/20 text-silver">{{ aiOptimization.display_mode }}</span>
              <span v-if="aiOptimization.target_bitrate_kbps" class="px-1.5 py-0.5 rounded text-[9px] bg-storm/20 text-silver">{{ (aiOptimization.target_bitrate_kbps/1000).toFixed(0) }} Mbps</span>
            </div>
          </div>

          <!-- Session History -->
          <div class="card p-3" v-if="sessionHistory.length">
            <div class="text-[10px] font-semibold text-storm uppercase tracking-wider mb-2">{{ $t('dashboard.session_history') }}</div>
            <div class="space-y-1.5">
              <div v-for="(s, i) in sessionHistory.slice(0, 5)" :key="i" class="flex items-center gap-2">
                <span class="w-5 h-5 rounded-full flex items-center justify-center text-[9px] font-bold shrink-0"
                      :class="{
                        'bg-green-500/20 text-green-400': s.quality_grade === 'A',
                        'bg-blue-500/20 text-blue-400': s.quality_grade === 'B',
                        'bg-yellow-500/20 text-yellow-400': s.quality_grade === 'C',
                        'bg-orange-500/20 text-orange-400': s.quality_grade === 'D',
                        'bg-red-500/20 text-red-400': s.quality_grade === 'F'
                      }">{{ s.quality_grade }}</span>
                <div class="text-[10px] text-storm truncate">{{ s.key }}</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </template>

    <!-- ═══ IDLE LAYOUT ═══ -->
    <template v-else>
      <!-- At-a-glance status cards -->
      <div class="grid grid-cols-2 lg:grid-cols-4 gap-3">
        <div class="card p-3">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-wider mb-1">{{ $t('dashboard.status') }}</div>
          <div class="text-lg font-bold text-green-400">{{ $t('dashboard.ready') }}</div>
          <div class="text-[10px] text-storm mt-0.5">{{ pairedClients }} {{ $t('dashboard.clients_paired') }}</div>
        </div>
        <div class="card p-3">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-wider mb-1">{{ $t('dashboard.mode') }}</div>
          <div class="text-lg font-bold" :class="headlessEnabled ? 'text-accent' : 'text-silver'">{{ headlessEnabled ? $t('dashboard.headless') : $t('dashboard.windowed') }}</div>
          <div class="text-[10px] text-storm mt-0.5">{{ headlessEnabled ? $t('dashboard.headless_desc') : $t('dashboard.windowed_desc') }}</div>
        </div>
        <div class="card p-3">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-wider mb-1">GPU</div>
          <div class="text-lg font-bold" :class="gpu?.temperature_c > 65 ? 'text-yellow-400' : 'text-green-400'">{{ gpu?.temperature_c || '--' }}°C</div>
          <div class="text-[10px] text-storm mt-0.5">{{ gpu?.utilization_pct || 0 }}% load · {{ gpu?.power_draw_w?.toFixed(0) || '--' }}W</div>
        </div>
        <div class="card p-3">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-wider mb-1">AI</div>
          <div class="text-lg font-bold" :class="aiStatus?.enabled ? 'text-accent' : 'text-storm'">{{ aiStatus?.enabled ? $t('dashboard.active') : $t('dashboard.off') }}</div>
          <div class="text-[10px] text-storm mt-0.5">{{ aiStatus?.cache_count || 0 }} {{ $t('dashboard.cached') }} · {{ sessionHistory.length }} {{ $t('dashboard.sessions') }}</div>
        </div>
      </div>

      <!-- GPU Gauges + Quick Controls -->
      <div class="grid grid-cols-1 lg:grid-cols-[1fr_280px] gap-4">
        <div class="card p-4" v-if="gpu">
          <div class="flex items-center justify-between mb-3">
            <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider">{{ gpu.name }}</div>
            <div class="text-[10px] text-storm">{{ gpu.power_draw_w?.toFixed(0) || '--' }}W · {{ gpu.clock_gpu_mhz || gpu.clock_mhz || '--' }} MHz</div>
          </div>
          <div class="flex justify-around">
            <GaugeArc :value="gpu.temperature_c" :max="100" unit="°C" label="Temp"
                      :thresholds="[{ at: 0, color: '#22c55e' }, { at: 70, color: '#eab308' }, { at: 85, color: '#ef4444' }]" />
            <GaugeArc :value="gpu.utilization_pct" :max="100" unit="%" label="GPU" />
            <GaugeArc :value="gpu.encoder_pct" :max="100" unit="%" label="NVENC"
                      :thresholds="[{ at: 0, color: '#c8d6e5' }, { at: 60, color: '#eab308' }, { at: 85, color: '#ef4444' }]" />
            <GaugeArc :value="gpu.vram_used_mb / 1024" :max="gpu.vram_total_mb / 1024" unit="GB" label="VRAM"
                      :thresholds="[{ at: 0, color: '#c8d6e5' }, { at: 70, color: '#eab308' }, { at: 90, color: '#ef4444' }]" />
          </div>
        </div>
        <div class="card p-4">
          <QuickControls />
        </div>
      </div>

      <!-- Recent Games + System Info -->
      <div class="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <div class="card p-4">
          <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider mb-3">{{ $t('dashboard.recent_games') }}</div>
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
          <div v-else class="text-sm text-storm py-4 text-center">{{ $t('dashboard.no_games') }}</div>
        </div>
        <div class="card p-4">
          <div class="flex items-center gap-2 mb-3">
            <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider">{{ $t('dashboard.system') }}</div>
            <span class="px-1.5 py-0.5 rounded text-[9px] font-medium bg-accent/15 text-accent" v-if="headlessEnabled">{{ $t('dashboard.headless') }}</span>
            <span class="px-1.5 py-0.5 rounded text-[9px] font-medium bg-storm/20 text-storm" v-else>{{ $t('dashboard.windowed') }}</span>
          </div>
          <div class="grid grid-cols-2 gap-x-4 gap-y-2.5 text-sm">
            <div v-if="sessionType">
              <div class="text-[10px] text-storm uppercase">Session</div>
              <div class="text-silver font-medium capitalize">{{ sessionType }}</div>
            </div>
            <div v-if="displays.length">
              <div class="text-[10px] text-storm uppercase">Display</div>
              <div class="text-silver font-medium" v-for="d in displays" :key="d.name">
                {{ d.name }} <span class="text-storm" v-if="d.width">{{ d.width }}x{{ d.height }}</span>
              </div>
            </div>
            <div v-if="audio?.sink">
              <div class="text-[10px] text-storm uppercase">Audio</div>
              <div class="text-silver font-medium truncate" :title="audio.sink">{{ formatAudioName(audio.sink) }}</div>
            </div>
            <div>
              <div class="text-[10px] text-storm uppercase">Clients</div>
              <div class="text-silver font-medium">{{ pairedClients }} paired</div>
            </div>
            <div v-if="aiStatus">
              <div class="text-[10px] text-storm uppercase">AI Mode</div>
              <div class="text-silver font-medium">{{ aiStatus.enabled ? (aiStatus.use_subscription ? 'Subscription' : 'API Key') : 'Off' }}</div>
            </div>
            <div>
              <div class="text-[10px] text-storm uppercase">Version</div>
              <div class="text-silver font-medium">v{{ version }}</div>
            </div>
          </div>
          <div class="flex gap-2 mt-4 pt-3 border-t border-storm/20">
            <router-link to="/pin" class="inline-flex items-center gap-1.5 h-8 px-3 text-xs bg-ice text-void rounded-lg font-medium hover:bg-ice/90 transition-all no-underline">
              <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z"/></svg>
              {{ $t('dashboard.pair') }}
            </router-link>
            <router-link to="/config" class="inline-flex items-center gap-1.5 h-8 px-3 text-xs border border-storm text-silver rounded-lg font-medium hover:border-ice hover:text-ice transition-all no-underline">
              {{ $t('dashboard.configure') }}
            </router-link>
            <router-link to="/apps" class="inline-flex items-center gap-1.5 h-8 px-3 text-xs border border-storm text-silver rounded-lg font-medium hover:border-ice hover:text-ice transition-all no-underline">
              {{ $t('dashboard.apps') }}
            </router-link>
          </div>
        </div>
      </div>
    </template>

    <!-- Charts — 6 metrics in 2 rows of 3 -->
    <div class="grid grid-cols-2 lg:grid-cols-3 gap-3" v-if="stats?.streaming">
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-green-400/80 uppercase tracking-wider mb-1">FPS</div>
        <div ref="fpsChartEl" class="h-24 w-full"></div>
      </div>
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-sky-400/80 uppercase tracking-wider mb-1">Bitrate</div>
        <div ref="bitrateChartEl" class="h-24 w-full"></div>
      </div>
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-silver/60 uppercase tracking-wider mb-1">Encode</div>
        <div ref="encodeChartEl" class="h-24 w-full"></div>
      </div>
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-amber-400/80 uppercase tracking-wider mb-1">Latency</div>
        <div ref="latencyChartEl" class="h-24 w-full"></div>
      </div>
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-purple-400/80 uppercase tracking-wider mb-1">GPU Load</div>
        <div ref="gpuChartEl" class="h-24 w-full"></div>
      </div>
      <div class="card p-3">
        <div class="text-[10px] font-semibold text-red-400/80 uppercase tracking-wider mb-1">Packet Loss</div>
        <div ref="lossChartEl" class="h-24 w-full"></div>
      </div>
    </div>

    <!-- Recording controls (streaming) -->
    <div class="card p-4 flex items-center justify-between" v-if="stats?.streaming">
      <div class="flex items-center gap-3">
        <div class="text-xs font-semibold text-silver/80 uppercase tracking-wider">Recording</div>
        <div v-if="recording.active" class="flex items-center gap-2">
          <span class="w-2 h-2 rounded-full bg-red-500 animate-pulse"></span>
          <span class="text-sm text-red-400">Recording</span>
          <span class="text-xs text-silver/60" v-if="recording.file">{{ recording.file }}</span>
        </div>
        <span v-else class="text-sm text-silver/60">Idle</span>
      </div>
      <div class="flex items-center gap-2">
        <button v-if="!recording.active" @click="startRecording"
                class="inline-flex items-center gap-1.5 h-7 px-2.5 text-xs font-medium rounded-lg bg-red-500 text-white hover:bg-red-600 transition-all duration-200">
          <svg class="w-3 h-3" fill="currentColor" viewBox="0 0 24 24"><circle cx="12" cy="12" r="6"/></svg>
          Record
        </button>
        <button v-if="recording.active" @click="stopRecording"
                class="inline-flex items-center gap-1.5 h-7 px-2.5 text-xs font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice transition-all duration-200">
          <svg class="w-3 h-3" fill="currentColor" viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="1"/></svg>
          Stop
        </button>
        <button @click="saveReplay"
                class="inline-flex items-center gap-1.5 h-7 px-2.5 text-xs font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice transition-all duration-200">
          <svg class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1m-4-4l-4 4m0 0l-4-4m4 4V4"/></svg>
          Save Replay
        </button>
      </div>
    </div>

    <!-- GPU + System stats during streaming (compact bar) -->
    <div class="grid grid-cols-2 lg:grid-cols-4 gap-3" v-if="stats?.streaming && gpu">
      <div class="card p-3 flex items-center gap-3">
        <div class="text-2xl font-bold" :class="gpu.temperature_c > 80 ? 'text-red-400' : gpu.temperature_c > 65 ? 'text-yellow-400' : 'text-green-400'">{{ gpu.temperature_c }}°</div>
        <div>
          <div class="text-[10px] text-storm uppercase">GPU Temp</div>
          <div class="text-xs text-silver">{{ gpu.power_draw_w?.toFixed(0) }}W</div>
        </div>
      </div>
      <div class="card p-3 flex items-center gap-3">
        <div class="text-2xl font-bold text-purple-400">{{ gpu.utilization_pct }}%</div>
        <div>
          <div class="text-[10px] text-storm uppercase">GPU Load</div>
          <div class="text-xs text-silver">{{ gpu.clock_mhz }} MHz</div>
        </div>
      </div>
      <div class="card p-3 flex items-center gap-3">
        <div class="text-2xl font-bold text-sky-400">{{ gpu.encoder_pct }}%</div>
        <div>
          <div class="text-[10px] text-storm uppercase">NVENC</div>
          <div class="text-xs text-silver">Encoder</div>
        </div>
      </div>
      <div class="card p-3 flex items-center gap-3">
        <div class="text-2xl font-bold text-silver">{{ (gpu.vram_used_mb / 1024).toFixed(1) }}</div>
        <div>
          <div class="text-[10px] text-storm uppercase">VRAM (GB)</div>
          <div class="text-xs text-silver">/ {{ (gpu.vram_total_mb / 1024).toFixed(0) }} GB</div>
        </div>
      </div>
    </div>

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

const { stats } = useStreamStats(1000)
const { gpu, displays, audio, sessionType } = useSystemStats(3000)
const { sessions, clearHistory } = useSessionHistory(stats)
const { status: aiStatus, devices: aiDevices, fetchStatus: fetchAiStatus, fetchDevices: fetchAiDevices, getSuggestion: getAiSuggestion } = useAiOptimizer()

// AI optimization state for current stream
const aiOptimization = ref(null)
const aiCacheKeys = ref([])
const sessionHistory = ref([])
const recentApps = ref([])
const pairedClients = ref(0)
const version = ref('')
const headlessEnabled = ref(false)
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

// Check if current streaming client has AI-optimized settings
const aiOptimizedClient = computed(() => {
  if (!stats.value?.streaming || !stats.value?.client_name) return false
  const clientName = stats.value.client_name
  return aiCacheKeys.value.some(key => key.startsWith(clientName + ':'))
})

function titleizeToken(value) {
  if (!value) return '--'
  return String(value)
    .split(/[_-]+/)
    .filter(Boolean)
    .map(part => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ')
}

function modeLabelFromBool(value) {
  return value ? 'Headless' : 'Windowed'
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
  return modeLabelFromBool(Boolean(stats.value?.runtime_effective_headless))
})

const runtimeModeTone = computed(() => {
  if (!stats.value?.streaming) return 'bg-storm/20 text-storm'
  return stats.value?.runtime_effective_headless ? 'bg-accent/15 text-accent' : 'bg-amber-500/15 text-amber-300'
})

const runtimeEffectiveTone = computed(() => {
  if (!stats.value?.streaming) return 'text-storm'
  return stats.value?.runtime_effective_headless ? 'text-accent' : 'text-amber-300'
})

const runtimeOverrideLabel = computed(() => {
  if (!stats.value?.streaming) return '--'
  return stats.value?.runtime_gpu_native_override_active ? 'Active' : 'Inactive'
})

const runtimeOverrideTone = computed(() => {
  if (!stats.value?.streaming) return 'text-storm'
  return stats.value?.runtime_gpu_native_override_active ? 'text-amber-300' : 'text-green-400'
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

const runtimePathNote = computed(() => {
  if (!stats.value?.streaming) return ''

  if (nestedLabwcShmFallbackActive.value) {
    return 'Nested labwc fallback is active: Polaris is capturing the windowed compositor through SHM instead of the GPU-native fast path.'
  }

  if (stats.value?.runtime_gpu_native_override_active) {
    return 'GPU-native capture preference forced a visible compositor path for the active encoder.'
  }

  return ''
})

const runtimePathNoteTone = computed(() => {
  if (!runtimePathNote.value) return 'text-storm'
  if (nestedLabwcShmFallbackActive.value) return 'text-orange-300'
  return 'text-amber-300'
})

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
const streamingOutput = ref('')
const showPreview = ref(false)
const previewLoaded = ref(false)
const previewUrl = ref('')
let previewTimer = null

function startPreview() {
  previewLoaded.value = false
  showPreview.value = true
  refreshPreview()
  previewTimer = setInterval(refreshPreview, 2000)
}

function refreshPreview() {
  // When streaming, crop to the streaming output; otherwise show full display
  const output = streamingOutput.value ? `&output=${encodeURIComponent(streamingOutput.value)}` : ''
  previewUrl.value = `./api/display/screenshot?t=${Date.now()}${output}`
}

function stopPreview() {
  showPreview.value = false
  if (previewTimer) { clearInterval(previewTimer); previewTimer = null }
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

  if (s.fps < 55 && s.fps > 0)
    recs.push({ color: 'text-yellow-400', message: `FPS dropped to ${s.fps.toFixed(1)} — GPU may be overloaded. Consider lowering resolution or quality.` })

  if (s.latency_ms > 40)
    recs.push({ color: 'text-yellow-400', message: `Latency is ${s.latency_ms.toFixed(0)}ms — check network path or try wired connection.` })

  if (gpu.value && gpu.value.utilization_pct < 30 && s.encode_time_ms < 4)
    recs.push({ color: 'text-green-400', message: `GPU utilization is low (${gpu.value.utilization_pct}%) with fast encode — you have headroom for higher resolution or quality.` })

  // Headless mode recommendations
  if (!s.headless_mode && s.streaming)
    recs.push({ color: 'text-accent', message: `Enable Headless Mode in Audio/Video settings for invisible streaming. If GPU-native capture is preferred, Polaris can still switch to a visible compositor window when performance requires it.` })

  // AI optimizer recommendations
  if (!s.ai_enabled && s.streaming)
    recs.push({ color: 'text-accent', message: `Enable AI Optimizer in settings for per-game encoding tuning — auto-adjusts resolution, bitrate, and codec.` })

  // Adaptive bitrate recommendation
  if (s.adaptive_target_bitrate_kbps === 0 && s.packet_loss > 0 && s.streaming)
    recs.push({ color: 'text-accent', message: `Enable Adaptive Bitrate in Audio/Video settings — auto-adjusts bitrate when network quality drops.` })

  return recs
})

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
      systemInfo.gpuName = config.adapter_name || ''
      systemInfo.gpuDriver = config.gpu_driver || ''
      systemInfo.version = config.version || ''
      streamingOutput.value = config.linux_streaming_output || config.output_name || ''
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
  timestamps.value = []
  fpsHistory.value = []
  bitrateHistory.value = []
  encodeHistory.value = []
}

watch(stats, (newStats, oldStats) => {
  statsLoaded.value = true

  if (!newStats || !newStats.streaming) {
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

  // Stop preview when streaming ends
  if (!newStats.streaming && oldStats?.streaming) {
    stopPreview()
  }

  const now = Date.now() / 1000

  timestamps.value.push(now)
  fpsHistory.value.push(newStats.fps)
  bitrateHistory.value.push(newStats.bitrate_kbps / 1000)
  encodeHistory.value.push(newStats.encode_time_ms)
  latencyHistory.value.push(newStats.latency_ms)
  gpuHistory.value.push(newStats.gpu_usage || 0)
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
      version.value = data.version || '0.0.0'
      headlessEnabled.value = data.headless_mode === 'enabled'
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
