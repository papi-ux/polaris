<template>
  <div class="page-shell pb-2">
    <section class="page-header">
      <div class="page-heading">
        <h1 class="page-title">{{ $t('navbar.troubleshoot') }}</h1>
        <p class="page-subtitle">{{ $t('troubleshooting.overview') }}</p>
      </div>
      <div class="page-meta">
        <span v-if="platform" class="meta-pill">
          {{ platform }}
        </span>
        <span v-if="version" class="meta-pill">
          v{{ version }}
        </span>
      </div>
    </section>

    <section v-if="showCrashBanner" class="section-card space-y-3" data-previous-run-banner>
      <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
        <div>
          <div class="section-kicker">{{ $t('troubleshooting.previous_run') }}</div>
          <h2 class="section-title">{{ $t('troubleshooting.previous_run_title') }}</h2>
          <p class="section-copy">{{ previousRunSummary }}</p>
          <p v-if="lastRun?.started_at" class="mt-1 text-xs text-storm">
            {{ $t('troubleshooting.previous_run_started') }} {{ lastRun.started_at }}
          </p>
        </div>
        <div class="flex flex-shrink-0 flex-col gap-2 sm:flex-row">
          <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-primary" :disabled="generatingIssueDraft" @click="openPrefilledIssue">
            {{ $t('troubleshooting.report_problem') }}
          </button>
          <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary" @click="dismissCrashBanner">
            {{ $t('troubleshooting.previous_run_dismiss') }}
          </button>
        </div>
      </div>
    </section>

    <section class="section-card space-y-4" data-fix-my-stream>
      <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
        <div>
          <div class="section-kicker">{{ $t('troubleshooting.fix_my_stream_mode') }}</div>
          <h2 class="section-title">{{ $t('troubleshooting.fix_my_stream') }}</h2>
          <p class="section-copy">{{ $t('troubleshooting.fix_my_stream_desc') }}</p>
        </div>
        <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-primary" :disabled="downloadingSupportBundle" @click="downloadSupportBundle">
          {{ $t('troubleshooting.export_anonymized_diagnostics') }}
        </button>
      </div>
      <div
        class="surface-subtle border border-storm/20"
        :class="doctorDiagnosisIdle ? 'doctor-diagnosis-idle p-3' : 'p-4'"
        data-doctor-diagnosis
      >
        <div class="section-kicker">{{ $t('troubleshooting.doctor_plain_diagnosis') }}</div>
        <h3 class="mt-1 text-lg font-semibold text-silver">{{ doctorPlainDiagnosis.title }}</h3>
        <p class="mt-2 text-sm leading-relaxed text-storm">{{ doctorPlainDiagnosis.detail }}</p>
        <p class="mt-3 text-xs leading-relaxed text-ice">{{ doctorPlainDiagnosis.action }}</p>
        <details v-if="!doctorDiagnosisIdle" class="mt-4 rounded-xl border border-storm/20 bg-deep/35 p-3">
          <summary class="cursor-pointer text-sm font-medium text-silver">{{ $t('troubleshooting.advanced_diagnostics') }}</summary>
          <div class="mt-3 grid gap-2 sm:grid-cols-2">
            <div v-for="item in doctorAdvancedItems" :key="item.label" class="rounded-lg border border-storm/15 bg-void/40 px-3 py-2">
              <div class="text-[11px] uppercase tracking-wide text-storm">{{ item.label }}</div>
              <div class="mt-1 break-words text-sm text-silver">{{ item.value }}</div>
            </div>
          </div>
        </details>
        <div v-if="!doctorDiagnosisIdle" class="mt-4 rounded-xl border border-info/20 bg-info/10 p-3" data-ai-doctor-explanation>
          <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
            <div>
              <div class="text-[11px] font-semibold uppercase tracking-eyebrow text-info-bright">{{ $t('troubleshooting.ai_doctor_explanation') }}</div>
              <p class="mt-1 text-xs leading-relaxed text-info-bright">{{ $t('troubleshooting.ai_doctor_explanation_privacy') }}</p>
              <p class="mt-2 text-[11px] leading-relaxed text-storm">{{ aiDoctorCategoriesText }}</p>
            </div>
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary" :disabled="requestingAiDoctorExplanation" @click="requestAiDoctorExplanation">
              {{ requestingAiDoctorExplanation ? $t('troubleshooting.ai_doctor_explanation_pending') : $t('troubleshooting.ai_doctor_explanation_button') }}
            </button>
          </div>
          <div v-if="aiDoctorExplanation" class="mt-3 rounded-lg border border-storm/15 bg-void/40 p-3 text-sm text-silver">
            <div class="font-semibold">{{ aiDoctorExplanation.likely_cause }}</div>
            <ul class="mt-2 list-disc space-y-1 pl-5 text-storm">
              <li v-for="item in aiDoctorExplanation.evidence" :key="item">{{ item }}</li>
            </ul>
            <div class="mt-3 text-xs uppercase tracking-eyebrow text-ice">{{ $t('troubleshooting.ai_doctor_try_first') }}</div>
            <ul class="mt-1 list-disc space-y-1 pl-5 text-storm">
              <li v-for="item in aiDoctorExplanation.try_first" :key="item">{{ item }}</li>
            </ul>
            <details class="mt-3">
              <summary class="cursor-pointer text-xs font-medium text-silver">{{ $t('troubleshooting.advanced_diagnostics') }}</summary>
              <p class="mt-2 text-xs leading-relaxed text-storm">{{ aiDoctorExplanation.advanced_detail }}</p>
              <p class="mt-2 text-[11px] text-ice">{{ $t('troubleshooting.ai_doctor_no_actions') }} · {{ aiDoctorExplanation.confidence }}</p>
            </details>
          </div>
        </div>
      </div>
      <div v-if="!doctorDiagnosisIdle" class="grid gap-3 md:grid-cols-2 xl:grid-cols-3" data-doctor-checklist>
        <div
          v-for="item in fixMyStreamChecklist"
          :key="item.key"
          class="surface-subtle border p-4"
          :class="statusTone(item.status).card"
        >
          <div class="flex items-center justify-between gap-3">
            <div class="text-sm font-semibold text-silver">{{ item.label }}</div>
            <span class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow" :class="statusTone(item.status).badge">
              {{ fixMyStreamStatusLabel(item.status) }}
            </span>
          </div>
          <p class="mt-2 text-sm leading-relaxed text-storm">{{ item.detail }}</p>
          <p class="mt-3 text-xs leading-relaxed text-ice">{{ item.action }}</p>
        </div>
      </div>
    </section>

    <section class="section-card space-y-4" data-support-self-tests>
      <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
        <div>
          <div class="section-kicker">Built-in self tests</div>
          <h2 class="section-title">Network, controller, and post-session report</h2>
          <p class="section-copy">Start with quick player checks. Open the supporting evidence only when you need a deeper diagnosis.</p>
        </div>
        <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary" @click="copySupportSelfTests">
          Copy self-test summary
        </button>
      </div>

      <div class="grid gap-3 xl:grid-cols-3">
        <div class="surface-subtle border p-4" :class="statusTone(networkPathReport.status).card">
          <div class="flex items-center justify-between gap-3">
            <div class="text-sm font-semibold text-silver">Network Path Tester</div>
            <span class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow" :class="statusTone(networkPathReport.status).badge">{{ statusTone(networkPathReport.status).label }}</span>
          </div>
          <p class="mt-2 text-sm leading-relaxed text-storm">{{ networkPathReport.summary }}</p>
          <p class="mt-3 text-xs leading-relaxed text-ice">Recommended ceiling: {{ networkPathReport.recommendedBitrateKbps }} kbps.</p>
          <details class="mt-3 text-xs text-storm">
            <summary class="cursor-pointer text-ice">Advanced evidence</summary>
            <div class="mt-2 space-y-2">
              <div v-for="check in networkPathReport.checks" :key="check.key" class="rounded-lg border border-storm/15 bg-void/40 px-3 py-2">
                <div class="font-medium text-silver">{{ check.label }} · {{ statusTone(check.status).label }}</div>
                <div>{{ check.detail }}</div>
                <div class="mt-1 text-ice">{{ check.action }}</div>
              </div>
              <pre v-if="networkPathReport.nativeEvidenceText" class="whitespace-pre-wrap rounded-lg border border-storm/15 bg-void/50 p-3">{{ networkPathReport.nativeEvidenceText }}</pre>
            </div>
          </details>
        </div>

        <div class="surface-subtle border p-4" :class="statusTone(controllerInputReport.status).card">
          <div class="flex items-center justify-between gap-3">
            <div class="text-sm font-semibold text-silver">Controller/Input Tester</div>
            <span class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow" :class="statusTone(controllerInputReport.status).badge">{{ statusTone(controllerInputReport.status).label }}</span>
          </div>
          <p class="mt-2 text-sm leading-relaxed text-storm">{{ controllerInputReport.summary }}</p>
          <div class="mt-3 flex flex-wrap gap-2">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary" @click="refreshControllerSnapshot">Detect controller</button>
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary" @click="recordControllerEvent('A / primary button', 1)">Log A press</button>
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary" :disabled="controllerRumbleSupported !== true" @click="pulseControllerHaptics">Test rumble</button>
          </div>
          <details class="mt-3 text-xs text-storm">
            <summary class="cursor-pointer text-ice">Advanced evidence</summary>
            <div class="mt-2 space-y-2">
              <div v-for="check in controllerInputReport.checks" :key="check.key" class="rounded-lg border border-storm/15 bg-void/40 px-3 py-2">
                <div class="font-medium text-silver">{{ check.label }} · {{ statusTone(check.status).label }}</div>
                <div>{{ check.detail }}</div>
                <div class="mt-1 text-ice">{{ check.action }}</div>
              </div>
            </div>
          </details>
        </div>

        <div class="surface-subtle border p-4" :class="statusTone(postSessionReport.status).card">
          <div class="flex items-center justify-between gap-3">
            <div class="text-sm font-semibold text-silver">Post-session Stream Report</div>
            <span class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow" :class="statusTone(postSessionReport.status).badge">{{ postSessionReport.issueOwner }}</span>
          </div>
          <p class="mt-2 text-sm leading-relaxed text-storm">{{ postSessionReport.mainIssue }}</p>
          <p class="mt-3 text-xs leading-relaxed text-ice">Next launch: {{ postSessionReport.suggestedNextLaunchProfile }}</p>
          <details class="mt-3 text-xs text-storm">
            <summary class="cursor-pointer text-ice">Advanced evidence</summary>
            <pre class="mt-2 whitespace-pre-wrap rounded-lg border border-storm/15 bg-void/50 p-3">{{ postSessionReport.copyText }}</pre>
          </details>
        </div>
      </div>
    </section>

    <section class="section-card space-y-4">
      <div class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
        <div>
          <div class="section-kicker">{{ $t('troubleshooting.recovery_ladder') }}</div>
          <h2 class="section-title">{{ $t('troubleshooting.quick_recovery') }}</h2>
          <p class="section-copy">{{ $t('troubleshooting.quick_recovery_desc') }}</p>
        </div>
        <div class="page-meta">
          <span class="meta-pill">{{ $t('troubleshooting.recovery_guidance') }}</span>
        </div>
      </div>
      <div class="grid gap-3 md:grid-cols-2 xl:grid-cols-4">
        <div class="surface-subtle flex h-full flex-col border-success/15 p-4">
          <div class="flex items-center justify-between gap-3">
            <span class="rounded-full bg-success/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-success">{{ $t('troubleshooting.recovery_rank_1') }}</span>
            <span class="text-[10px] uppercase tracking-eyebrow text-storm">{{ $t('troubleshooting.recovery_rank_1_label') }}</span>
          </div>
          <h3 id="close_apps" class="mt-3 text-lg font-semibold text-silver">{{ $t('troubleshooting.force_close') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.force_close_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-success bg-twilight/50 p-3 text-silver" v-if="closeAppStatus === true">
            {{ $t('troubleshooting.force_close_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-danger bg-twilight/50 p-3 text-silver" v-if="closeAppStatus === false">
            {{ $t('troubleshooting.force_close_error') }}
          </div>
          <div class="mt-4">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-primary" :disabled="closeAppPressed" @click="requestConfirmedAction('forceClose')">
              {{ $t('troubleshooting.force_close') }}
            </button>
          </div>
        </div>

        <div class="surface-subtle flex h-full flex-col border-warning/15 p-4">
          <div class="flex items-center justify-between gap-3">
            <span class="rounded-full bg-warning/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-warning-bright">{{ $t('troubleshooting.recovery_rank_2') }}</span>
            <span class="text-[10px] uppercase tracking-eyebrow text-storm">{{ $t('troubleshooting.recovery_rank_2_label') }}</span>
          </div>
          <h3 id="restart" class="mt-3 text-lg font-semibold text-silver">{{ $t('troubleshooting.restart_polaris') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.restart_polaris_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-success bg-twilight/50 p-3 text-silver" v-if="serverRestarting">
            {{ $t('troubleshooting.restart_polaris_success') }}
          </div>
          <div class="mt-4">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-primary" :disabled="serverQuitting || serverRestarting" @click="requestConfirmedAction('restart')">
              {{ $t('troubleshooting.restart_polaris') }}
            </button>
          </div>
        </div>

        <div class="surface-subtle flex h-full flex-col border-danger/15 p-4">
          <div class="flex items-center justify-between gap-3">
            <span class="rounded-full bg-danger/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-danger">{{ $t('troubleshooting.recovery_rank_3') }}</span>
            <span class="text-[10px] uppercase tracking-eyebrow text-storm">{{ $t('troubleshooting.recovery_rank_3_label') }}</span>
          </div>
          <h3 id="quit" class="mt-3 text-lg font-semibold text-silver">{{ $t('troubleshooting.quit_polaris') }}</h3>
          <p class="mt-2 flex-1 text-sm text-storm">{{ $t('troubleshooting.quit_polaris_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-success bg-twilight/50 p-3 text-silver" v-if="serverQuit">
            {{ $t('troubleshooting.quit_polaris_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-success bg-twilight/50 p-3 text-silver" v-if="serverQuitting">
            {{ $t('troubleshooting.quit_polaris_success_ongoing') }}
          </div>
          <div class="mt-4">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-danger" :disabled="serverQuitting || serverRestarting" @click="requestConfirmedAction('quit')">
              {{ $t('troubleshooting.quit_polaris') }}
            </button>
          </div>
        </div>

        <div class="surface-subtle flex h-full flex-col border-info/15 p-4" v-if="platform === 'windows'">
          <div class="flex items-center justify-between gap-3">
            <span class="rounded-full bg-info/10 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow text-info-bright">Optional</span>
            <span class="text-[10px] uppercase tracking-eyebrow text-storm">Display reset</span>
          </div>
          <h3 id="dd_reset" class="mt-3 text-lg font-semibold text-silver">{{ $t('troubleshooting.dd_reset') }}</h3>
          <p class="mt-2 flex-1 whitespace-pre-line text-sm text-storm">{{ $t('troubleshooting.dd_reset_desc') }}</p>
          <div class="mt-3 rounded-lg border-l-4 border-success bg-twilight/50 p-3 text-silver" v-if="ddResetStatus === true">
            {{ $t('troubleshooting.dd_reset_success') }}
          </div>
          <div class="mt-3 rounded-lg border-l-4 border-danger bg-twilight/50 p-3 text-silver" v-if="ddResetStatus === false">
            {{ $t('troubleshooting.dd_reset_error') }}
          </div>
          <div class="mt-4">
            <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary" :disabled="ddResetPressed" @click="ddResetPersistence">
              {{ $t('troubleshooting.dd_reset') }}
            </button>
          </div>
        </div>
      </div>
    </section>

    <section class="grid gap-4 xl:grid-cols-[minmax(0,1.3fr)_minmax(320px,0.9fr)]">
      <div class="section-card">
        <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
          <div>
            <h2 id="session_snapshot" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.session_snapshot') }}</h2>
            <p class="mt-2 text-sm text-storm">{{ $t('troubleshooting.session_snapshot_desc') }}</p>
          </div>
          <span class="rounded-full border px-2.5 py-1 text-sm"
                :class="streamStatsConnected ? 'border-success/40 bg-success/10 text-success' : 'border-storm/40 bg-deep/60 text-storm'">
            {{ streamStatsConnected ? $t('troubleshooting.session_snapshot_connected') : $t('troubleshooting.session_snapshot_disconnected') }}
          </span>
        </div>

        <div v-if="!streamStats" class="mt-4 rounded-xl border border-dashed border-storm/30 bg-deep/40 px-4 py-5 text-sm text-storm">
          {{ $t('troubleshooting.session_snapshot_waiting') }}
        </div>
        <div v-else-if="!streamStats.streaming" class="mt-4 rounded-xl border border-dashed border-storm/30 bg-deep/40 px-4 py-5 text-sm text-storm">
          {{ $t('troubleshooting.session_snapshot_idle') }}
        </div>
        <template v-else>
          <div class="mt-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <div v-for="item in sessionSnapshotSummaryItems" :key="item.label" class="rounded-xl border border-storm/25 bg-deep/60 px-3 py-3">
              <div class="text-[11px] uppercase tracking-wide text-storm">{{ item.label }}</div>
              <div class="mt-1 text-sm font-medium text-silver break-words">{{ item.value }}</div>
            </div>
          </div>
          <div class="mt-4 grid gap-3 sm:grid-cols-2">
            <div v-for="item in sessionSnapshotDetailItems" :key="item.label" class="rounded-xl border border-storm/20 bg-deep/40 px-3 py-2.5">
              <div class="text-[11px] uppercase tracking-wide text-storm">{{ item.label }}</div>
              <div class="mt-1 text-sm font-medium text-silver break-words">{{ item.value }}</div>
            </div>
          </div>
        </template>
      </div>

      <div class="section-card">
        <h2 id="diagnostics" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.diagnostics') }}</h2>
        <p class="mt-2 text-sm text-storm">{{ $t('troubleshooting.diagnostics_desc') }}</p>
        <div v-if="groupedRecentIssues.length" class="mt-4 rounded-xl border border-storm/20 bg-deep/40 p-4">
          <div class="flex items-center justify-between gap-3">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.recent_incidents') }}</div>
            <div class="text-xs text-storm">{{ groupedRecentIssues.length }} {{ $t('troubleshooting.visible_now') }}</div>
          </div>
          <div class="mt-3 space-y-2">
            <div v-for="(entry, index) in groupedRecentIssues" :key="`${entry.level}-${entry.message}-${index}`" class="rounded-lg border border-storm/15 bg-void/40 px-3 py-2">
              <div class="flex items-start gap-3">
                <span class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-eyebrow"
                      :class="entry.level === 'Fatal'
                        ? 'border border-danger/30 bg-danger/10 text-danger-bright'
                        : entry.level === 'Warning'
                          ? 'border border-warning/30 bg-warning/10 text-warning-bright'
                          : 'border border-storm/30 bg-deep/60 text-storm'">
                  {{ entry.level }}
                </span>
                <div class="min-w-0 flex-1">
                  <div class="text-sm text-silver break-words">{{ entry.message }}</div>
                  <div class="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-storm">
                    <span>{{ entry.timestamp }}</span>
                    <span v-if="entry.count > 1" class="rounded-full border border-storm/20 bg-void/50 px-2 py-0.5">{{ entry.count }}×</span>
                  </div>
                </div>
              </div>
            </div>
          </div>
        </div>
        <div v-else class="mt-4 rounded-xl border border-dashed border-storm/25 bg-deep/35 px-4 py-5 text-sm text-storm">
          {{ $t('troubleshooting.recent_incidents_empty') }}
        </div>
        <div class="mt-4 rounded-xl border border-info/20 bg-info/10 px-4 py-3 text-sm text-info-bright">
          {{ $t('troubleshooting.support_redaction_notice') }}
        </div>
        <div class="mt-4 space-y-2" data-report-a-problem>
          <label class="section-kicker block" for="support-user-notes">{{ $t('troubleshooting.report_problem_notes') }}</label>
          <textarea
            id="support-user-notes"
            v-model="userNotes"
            rows="3"
            class="w-full rounded-xl border border-storm/25 bg-deep/35 px-3 py-2 text-sm text-silver"
            :placeholder="$t('troubleshooting.report_problem_notes_placeholder')"
          ></textarea>
          <button class="focus-ring troubleshooting-action-button troubleshooting-action-button-primary w-full sm:w-auto" :disabled="generatingIssueDraft" @click="openPrefilledIssue">
            {{ $t('troubleshooting.report_problem') }}
          </button>
          <p class="text-xs text-storm">{{ $t('troubleshooting.report_problem_desc') }}</p>
        </div>
        <div class="mt-4 grid gap-2 sm:grid-cols-2">
          <button class="focus-ring troubleshooting-action-card" @click="copyIssueDraft" :disabled="generatingIssueDraft">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.copy_issue_draft') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.copy_issue_draft_desc') }}</div>
          </button>
          <button class="focus-ring troubleshooting-action-card" @click="downloadIssueDraft" :disabled="generatingIssueDraft">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.download_issue_draft') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.download_issue_draft_desc') }}</div>
          </button>
          <button class="focus-ring troubleshooting-action-card" @click="copyRecentIssues">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.copy_recent_issues') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.copy_recent_issues_desc') }}</div>
          </button>
          <button class="focus-ring troubleshooting-action-card" :disabled="downloadingSupportBundle" @click="downloadSupportBundle">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.download_support_bundle') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.download_support_bundle_desc') }}</div>
          </button>
          <button class="focus-ring troubleshooting-action-card" :disabled="clearingAiCache" @click="clearAiCache">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.clear_ai_cache') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.clear_ai_cache_desc') }}</div>
          </button>
          <button v-if="platform === 'linux'" class="focus-ring troubleshooting-action-card" :disabled="cleaningStaleVirtualDisplay" @click="requestConfirmedAction('cleanupStaleVirtualDisplay')">
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.cleanup_stale_virtual_display') }}</div>
            <div class="mt-1 text-xs text-storm">{{ $t('troubleshooting.cleanup_stale_virtual_display_desc') }}</div>
          </button>
        </div>
      </div>
    </section>

    <section class="section-card">
      <div class="flex flex-col gap-3 xl:flex-row xl:items-end xl:justify-between">
        <div>
          <h2 id="logs" class="text-xl font-semibold text-silver">{{ $t('troubleshooting.logs') }}</h2>
          <p class="mt-2 text-sm text-storm">{{ $t('troubleshooting.logs_desc') }}</p>
        </div>
        <div class="flex w-full flex-col gap-3 xl:w-auto xl:items-end">
          <div class="page-meta">
            <span class="meta-pill">{{ logFilterSummary }}</span>
            <span v-if="logsTruncated" class="meta-pill" data-log-tail-truncated>
              Bounded tail · earlier content omitted · bytes {{ logStartOffset }}–{{ logEndOffset }}
            </span>
            <span v-if="logsReset" class="meta-pill" data-log-tail-reset>
              Log window reset after clear, rotation, or a missed range
            </span>
          </div>
          <div class="flex flex-wrap items-center gap-2">
            <button v-for="level in ['All', 'Error', 'Warning', 'Fatal']" :key="level"
                    @click="logLevelFilter = level === 'All' ? null : level"
                    class="focus-ring troubleshooting-filter-button"
                    :class="(logLevelFilter === level || (level === 'All' && !logLevelFilter))
                      ? 'is-active'
                      : ''">
              {{ level }}
            </button>
          </div>
          <div class="troubleshooting-filter-panel flex w-full flex-col gap-2 sm:flex-row xl:w-auto">
            <input
              type="text"
              name="logs-filter"
              autocomplete="off"
              :aria-label="$t('troubleshooting.logs_find')"
              class="w-full rounded-lg border border-storm/50 bg-deep px-3 py-1.5 text-sm text-silver focus:border-ice focus:outline-none sm:w-72"
              v-model="logFilter"
              :placeholder="$t('troubleshooting.logs_find')"
            >
            <button
              class="focus-ring troubleshooting-action-button troubleshooting-action-button-secondary"
              :aria-label="$t('troubleshooting.logs_clear_filters')"
              :disabled="clearingLogs"
              @click="clearLogs"
            >
              {{ $t('troubleshooting.logs_clear') }}
            </button>
          </div>
        </div>
      </div>

        <div v-if="logsLoading" class="mt-4 flex h-[320px] items-center justify-center rounded-xl border border-dashed border-storm/30 bg-deep/40 px-6 text-center">
          <div>
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.logs_loading') }}</div>
            <div class="mt-2 text-sm text-storm">{{ $t('troubleshooting.logs_empty_hint') }}</div>
          </div>
        </div>
        <div v-else-if="hasFilteredLogs" class="mt-4">
          <VirtualLogViewer :logs="actualLogs" :containerHeight="320" @copy="copyLogs" />
        </div>
        <div v-else class="mt-4 flex h-[320px] items-center justify-center rounded-xl border border-dashed border-storm/30 bg-deep/40 px-6 text-center">
          <div>
            <div class="text-sm font-medium text-silver">{{ $t('troubleshooting.logs_empty') }}</div>
            <div class="mt-2 text-sm text-storm">{{ $t('troubleshooting.logs_empty_hint') }}</div>
          </div>
        </div>
    </section>
    <ConfirmActionDialog
      v-if="activeConfirmAction"
      v-model="confirmActionOpen"
      :title="activeConfirmAction.title"
      :message="activeConfirmAction.message"
      :impact-items="activeConfirmAction.impactItems"
      :confirm-label="activeConfirmAction.confirmLabel"
      :cancel-label="i18n.t('_common.cancel')"
      :pending="Boolean(pendingConfirmedAction)"
      :pending-label="activeConfirmAction.pendingLabel"
      @confirm="executeConfirmedAction"
    />
  </div>
</template>

<script setup>
import { ref, computed, onBeforeUnmount, inject, watch } from 'vue'
import { useToast } from '../composables/useToast'
import { useStreamStats } from '../composables/useStreamStats'
import VirtualLogViewer from '../components/VirtualLogViewer.vue'
import ConfirmActionDialog from '../components/ConfirmActionDialog.vue'
import { requestHostRestart } from '../restart-host.js'
import {
  buildAnonymizedDiagnosticsBundle,
  buildControllerInputTestReport,
  buildFixMyStreamChecklist,
  buildGithubIssueDraft,
  buildGithubIssueUrl,
  buildNetworkPathTestReport,
  buildPostSessionStreamReport,
  buildSupportSelfTestCopy,
  describePreviousRun,
  redactSensitiveText,
} from '../diagnostics-export.js'
import { AI_DOCTOR_EXPLANATION_CATEGORIES, explainDoctorWithAi } from '../ai-doctor-explanation.js'
import { createLogTailState, fetchLogTail } from '../log-tail-state.js'
import { groupRecentIssueLogs } from '../recent-issues.js'
import { statusTone } from '../status-tones.js'

const { toast: showToast } = useToast()
const i18n = inject('i18n')
const { stats: streamStats, connected: streamStatsConnected } = useStreamStats()

const closeAppPressed = ref(false)
const closeAppStatus = ref(null)
const ddResetPressed = ref(false)
const ddResetStatus = ref(null)
const logs = ref('Loading...')
const logsTruncated = ref(false)
const logsReset = ref(false)
const logStartOffset = ref(0)
const logEndOffset = ref(0)
const clearingLogs = ref(false)
const clearingAiCache = ref(false)
const cleaningStaleVirtualDisplay = ref(false)
const downloadingSupportBundle = ref(false)
const generatingIssueDraft = ref(false)
const lastRun = ref(null)
const crashBannerDismissed = ref(false)
const userNotes = ref('')
const requestingAiDoctorExplanation = ref(false)
const aiDoctorExplanation = ref(null)
const logFilter = ref(null)
const logLevelFilter = ref(null)
let logInterval = null
let logTailState = createLogTailState()
let logRefreshInFlight = null
let logRefreshQueued = false
let logTailGeneration = 0
const serverRestarting = ref(false)
const serverQuitting = ref(false)
const serverQuit = ref(false)
const platform = ref("")
const version = ref("")
const confirmActionOpen = ref(false)
const pendingConfirmedAction = ref(null)
const requestedConfirmAction = ref(null)
const controllerEvents = ref([])
const controllerRumbleSupported = ref(null)
const nativeNetworkPathProbe = ref(null)
const lastCompletedStreamStats = ref(null)
const lastDisconnectReason = ref('')

const confirmedActions = computed(() => ({
  forceClose: {
    title: i18n.t('troubleshooting.force_close_confirm_title'),
    message: i18n.t('troubleshooting.force_close_confirm_message'),
    impactItems: [
      i18n.t('troubleshooting.force_close_impact_app'),
      i18n.t('troubleshooting.force_close_impact_clients'),
    ],
    confirmLabel: i18n.t('troubleshooting.force_close'),
    pendingLabel: i18n.t('troubleshooting.force_close_pending'),
    execute: closeApp,
  },
  restart: {
    title: i18n.t('troubleshooting.restart_polaris_confirm_title'),
    message: i18n.t('troubleshooting.restart_polaris_confirm_message'),
    impactItems: [
      i18n.t('troubleshooting.restart_polaris_impact_streams'),
      i18n.t('troubleshooting.restart_polaris_impact_web'),
    ],
    confirmLabel: i18n.t('troubleshooting.restart_polaris'),
    pendingLabel: i18n.t('troubleshooting.restart_polaris_success'),
    execute: restart,
  },
  quit: {
    title: i18n.t('troubleshooting.quit_polaris_confirm_title'),
    message: i18n.t('troubleshooting.quit_polaris_confirm'),
    impactItems: [
      i18n.t('troubleshooting.quit_polaris_impact_streams'),
      i18n.t('troubleshooting.quit_polaris_impact_access'),
    ],
    confirmLabel: i18n.t('troubleshooting.quit_polaris'),
    pendingLabel: i18n.t('troubleshooting.quit_polaris_success_ongoing'),
    execute: quit,
  },
  cleanupStaleVirtualDisplay: {
    title: i18n.t('troubleshooting.cleanup_stale_virtual_display_confirm_title'),
    message: i18n.t('troubleshooting.cleanup_stale_virtual_display_confirm_message'),
    impactItems: [
      i18n.t('troubleshooting.cleanup_stale_virtual_display_impact'),
    ],
    confirmLabel: i18n.t('troubleshooting.cleanup_stale_virtual_display'),
    pendingLabel: i18n.t('troubleshooting.cleanup_stale_virtual_display_pending'),
    execute: cleanupStaleVirtualDisplay,
  },
}))
const activeConfirmAction = computed(() => confirmedActions.value[requestedConfirmAction.value] || null)

function requestConfirmedAction(key) {
  requestedConfirmAction.value = key
  confirmActionOpen.value = true
}

async function executeConfirmedAction() {
  if (!activeConfirmAction.value || pendingConfirmedAction.value) return
  pendingConfirmedAction.value = requestedConfirmAction.value
  try {
    await activeConfirmAction.value.execute()
    confirmActionOpen.value = false
  } finally {
    pendingConfirmedAction.value = null
  }
}

function yesNo(value) {
  return value ? 'Yes' : 'No'
}

function formatNumber(value, digits = 1) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) {
    return '0'
  }
  return Number(value).toFixed(digits)
}

function formatFps(value) {
  return `${formatNumber(value, 1)} FPS`
}

function formatResolution(width, height) {
  if (!width || !height) return '(unknown)'
  return `${width}x${height}`
}

function summarizeStreamStats(s = {}) {
  if (!s.streaming) return 'No active stream'
  return `${formatFps(s.fps)} / ${formatFps(s.session_target_fps || s.requested_client_fps)} target · ${s.bitrate_kbps || 0} kbps · ${formatNumber(s.packet_loss, 2)}% loss · ${formatNumber(s.encode_time_ms, 1)} ms encode`
}

function hasRuntimeOverride(s) {
  const effectiveKnown = s.runtime_effective_headless !== undefined && s.runtime_effective_headless !== null
  return Boolean(s.runtime_requested_headless) &&
    effectiveKnown &&
    !Boolean(s.runtime_effective_headless) &&
    Boolean(s.runtime_gpu_native_override_active)
}

function runtimeModeDescription(s) {
  if (s.stream_display_mode) return s.stream_display_mode
  return `${yesNo(s.runtime_effective_headless)} effective / ${yesNo(s.runtime_requested_headless)} requested`
}

function runtimeOverrideDescription(s) {
  if (hasRuntimeOverride(s)) {
    return 'GPU-native override active: requested headless, running windowed labwc'
  }
  return s.runtime_gpu_native_override_active ? 'GPU-native override active' : 'None'
}

function captureReasonDescription(reason) {
  const key = String(reason || '').toLowerCase()
  const messages = {
    gpu_native: 'Capture and encoder conversion are GPU-resident.',
    headless_extcopy_dmabuf: 'True-headless DMA-BUF capture is active; frames stay GPU-resident through the encoder path.',
    windowed_dmabuf_override: 'Windowed private compositor is preserving the GPU-native capture path.',
    headless_shm_fallback: 'Private Stream is using SHM/system-memory capture; healthy streams can still show this, including AMD/VAAPI conservative baselines.',
    headless_shm_default: 'Private Stream is using SHM/system-memory capture; healthy streams can still show this, including AMD/VAAPI conservative baselines.',
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

function fpsTargetGapDescription(s) {
  const encoded = Number(s.fps)
  const target = Number(s.session_target_fps || s.requested_client_fps)

  if (!Number.isFinite(encoded) || !Number.isFinite(target) || target < 90 || encoded <= 0) {
    return 'None'
  }

  if (encoded >= target * 0.85) {
    return 'None'
  }

  return `${formatFps(encoded)} encoded against ${formatFps(target)} target`
}

const groupedRecentIssues = computed(() => groupRecentIssueLogs(logs.value, {
  maxSourceLines: 300,
  maxGroups: 8,
}))

const recentIssueSummaryText = computed(() => groupedRecentIssues.value
  .map((entry) => `${entry.timestamp} ${entry.level}: ${entry.message}${entry.count > 1 ? ` (${entry.count}x)` : ''}`.trim())
  .join('\n'))

const fixMyStreamChecklist = computed(() => buildFixMyStreamChecklist({
  stats: streamStats.value || {},
  statsConnected: streamStatsConnected.value,
  logs: logs.value,
  recentIssues: groupedRecentIssues.value,
}))

const doctorPayload = computed(() => streamStats.value?.doctor || null)
const doctorDiagnosisIdle = computed(() => !streamStats.value?.streaming && !doctorPayload.value)
const aiDoctorCategoriesText = computed(() => `${i18n.t('troubleshooting.ai_doctor_categories_prefix')} ${AI_DOCTOR_EXPLANATION_CATEGORIES.join(', ')}`)
const doctorPlainDiagnosis = computed(() => {
  const doctor = doctorPayload.value
  if (doctor?.simple_state || doctor?.summary || doctor?.diagnosis) {
    return {
      title: doctor.simple_state || doctor.summary || doctor.diagnosis,
      detail: doctor.detail || doctor.reason || i18n.t('troubleshooting.doctor_plain_diagnosis_desc'),
      action: doctor.safe_recovery_action?.label || doctor.safe_recovery_action?.id || i18n.t('troubleshooting.doctor_plain_diagnosis_action'),
    }
  }

  const firstProblem = fixMyStreamChecklist.value.find((item) => item.status === 'fail' || item.status === 'warning')
  if (firstProblem) {
    return {
      title: `${firstProblem.label}: ${fixMyStreamStatusLabel(firstProblem.status)}`,
      detail: firstProblem.detail,
      action: firstProblem.action,
    }
  }

  return {
    title: i18n.t('troubleshooting.doctor_plain_diagnosis_empty'),
    detail: i18n.t('troubleshooting.doctor_plain_diagnosis_desc'),
    action: i18n.t('troubleshooting.doctor_plain_diagnosis_action'),
  }
})

const doctorAdvancedItems = computed(() => {
  const s = streamStats.value || {}
  const doctor = doctorPayload.value || {}
  return [
    { label: 'Doctor issue', value: doctor.primary_issue || 'none' },
    { label: 'Doctor action', value: doctor.safe_recovery_action?.id || 'none' },
    { label: 'Client', value: s.client_name || s.client_type || '(unknown)' },
    { label: 'Runtime', value: s.launch_mode || s.stream_display_mode || s.runtime_backend || '(unknown)' },
    { label: 'Capture path', value: s.capture_path || s.capture_transport || '(unknown)' },
    { label: 'Capture reason', value: s.capture_path_reason || '(unknown)' },
    { label: 'Encoder', value: s.encoder || s.encode_target_device || '(unknown)' },
    { label: 'Active stream stats', value: summarizeStreamStats(s) },
  ]
})

const networkPathReport = computed(() => buildNetworkPathTestReport({
  nativeProbe: nativeNetworkPathProbe.value,
  host: streamStats.value?.client_ip || window.location.hostname,
  originHostname: window.location.hostname,
  hostReachable: streamStatsConnected.value,
  controlPortOpen: streamStatsConnected.value,
  streamPortOpen: streamStats.value?.streaming ? true : undefined,
  mdnsAvailable: window.location.hostname.endsWith('.local'),
  pingSamplesMs: [streamStats.value?.latency_ms].filter((value) => Number.isFinite(Number(value))),
  packetLossPercent: streamStats.value?.packet_loss,
  currentBitrateKbps: streamStats.value?.bitrate_kbps,
}))

const browserGamepads = computed(() => {
  if (!navigator.getGamepads) return []
  return Array.from(navigator.getGamepads()).filter(Boolean).map((pad) => ({
    index: pad.index,
    id: pad.id,
    buttons: pad.buttons?.length || 0,
    axes: pad.axes?.length || 0,
  }))
})

const controllerNativeEvidence = computed(() => streamStats.value?.controller_input || {})

const controllerInputReport = computed(() => buildControllerInputTestReport({
  events: controllerEvents.value,
  gamepads: browserGamepads.value,
  native: controllerNativeEvidence.value,
  rumbleSupported: controllerRumbleSupported.value,
}))

const postSessionReport = computed(() => buildPostSessionStreamReport({
  stats: lastCompletedStreamStats.value || streamStats.value || {},
  logs: logs.value,
  disconnectReason: lastDisconnectReason.value,
}))

const supportSelfTestCopy = computed(() => buildSupportSelfTestCopy({
  network: networkPathReport.value,
  controller: controllerInputReport.value,
  postSession: postSessionReport.value,
}))

function recordControllerEvent(label, pad = 1) {
  controllerEvents.value = [
    ...controllerEvents.value.slice(-15),
    { pad, control: label, type: 'manual', at: new Date().toISOString() },
  ]
}

function refreshControllerSnapshot() {
  const pads = browserGamepads.value
  if (pads.length) {
    controllerRumbleSupported.value = Boolean(navigator.getGamepads?.()[pads[0].index]?.vibrationActuator)
    recordControllerEvent('Browser gamepad visible', pads[0].index + 1)
  } else {
    recordControllerEvent('Manual button sample', 1)
  }
}

async function pulseControllerHaptics() {
  const pad = navigator.getGamepads?.().find((candidate) => candidate?.vibrationActuator)
  if (!pad?.vibrationActuator?.playEffect) return
  await pad.vibrationActuator.playEffect('dual-rumble', {
    duration: 180,
    strongMagnitude: 0.35,
    weakMagnitude: 0.25,
  })
  controllerRumbleSupported.value = true
  recordControllerEvent('Optional rumble pulse', pad.index + 1)
}

function copySupportSelfTests() {
  navigator.clipboard.writeText(supportSelfTestCopy.value)
  showToast('Support self-test summary copied.', 'success')
}

watch(streamStats, (next, previous) => {
  if (previous?.streaming && next && !next.streaming) {
    lastCompletedStreamStats.value = previous
    lastDisconnectReason.value = 'Stream telemetry changed from active to idle.'
  }
}, { deep: true })

function fixMyStreamStatusLabel(status) {
  if (status === 'pass') return i18n.t('troubleshooting.fix_my_stream_status_pass')
  if (status === 'fail') return i18n.t('troubleshooting.fix_my_stream_status_fail')
  return i18n.t('troubleshooting.fix_my_stream_status_warning')
}

const sessionSnapshotItems = computed(() => {
  if (!streamStats.value || !streamStats.value.streaming) return []

  const s = streamStats.value
  return [
    { label: 'Resolution', value: formatResolution(s.width, s.height) },
    { label: 'Codec', value: s.codec || '(unknown)' },
    { label: 'FPS', value: `${formatFps(s.fps)} encoded / ${formatFps(s.session_target_fps)} target` },
    { label: 'Bitrate', value: `${s.bitrate_kbps || 0} kbps` },
    { label: 'Client IP', value: s.client_ip || '(unknown)' },
    { label: 'Active Sessions', value: `${s.active_sessions ?? 0}` },
    { label: 'Requested FPS', value: formatFps(s.requested_client_fps) },
    { label: 'Runtime Backend', value: s.runtime_backend || '(unknown)' },
    { label: 'Stream Display Mode', value: runtimeModeDescription(s) },
    { label: 'Runtime Override', value: runtimeOverrideDescription(s) },
    { label: 'FPS Target Gap', value: fpsTargetGapDescription(s) },
    { label: 'Capture Path', value: s.capture_path || 'unknown' },
    { label: 'Capture Reason', value: captureReasonDescription(s.capture_path_reason) },
    { label: 'Capture Transport', value: `${s.capture_transport || 'unknown'} / ${s.capture_residency || 'unknown'} / ${s.capture_format || 'unknown'}` },
    { label: 'Encode Target', value: `${s.encode_target_device || 'unknown'} / ${s.encode_target_residency || 'unknown'} / ${s.encode_target_format || 'unknown'}` },
    { label: 'GPU Native', value: yesNo(s.capture_gpu_native) },
    { label: 'CPU Copy', value: yesNo(s.capture_cpu_copy) },
    { label: 'Pacing Policy', value: s.pacing_policy || 'none' },
    { label: 'Optimization Source', value: s.optimization_source || 'default' },
    { label: 'Network', value: `${formatNumber(s.latency_ms, 1)} ms latency / ${formatNumber(s.packet_loss, 2)}% loss` },
    { label: 'Frame Delivery', value: `${formatNumber((s.duplicate_frame_ratio || 0) * 100, 2)}% duplicate / ${formatNumber((s.dropped_frame_ratio || 0) * 100, 2)}% dropped` },
    { label: 'Frame Timing', value: `${formatNumber(s.avg_frame_age_ms, 2)} ms age / ${formatNumber(s.frame_interval_error_ms ?? s.frame_jitter_ms, 2)} ms target interval error` }
  ]
})

const sessionSnapshotSummaryItems = computed(() => {
  if (!streamStats.value || !streamStats.value.streaming) return []
  return [
    { label: 'Client', value: streamStats.value.client_name || '(unknown)' },
    ...sessionSnapshotItems.value.slice(0, 3)
  ]
})

const sessionSnapshotDetailItems = computed(() => {
  if (!streamStats.value || !streamStats.value.streaming) return []
  return sessionSnapshotItems.value.slice(3)
})

const actualLogs = computed(() => {
  if (!logFilter.value && !logLevelFilter.value) return logs.value
  let lines = logs.value.split("\n")
  if (logLevelFilter.value) {
    lines = lines.filter(x => x.includes(logLevelFilter.value + ':'))
  }
  if (logFilter.value) {
    lines = lines.filter(x => x.indexOf(logFilter.value) !== -1)
  }
  return lines.join("\n")
})

const logsLoading = computed(() => logs.value === 'Loading...')
const hasFilteredLogs = computed(() => actualLogs.value.trim().length > 0)
const logFilterSummary = computed(() => {
  const parts = []
  parts.push(logLevelFilter.value ? `${logLevelFilter.value} only` : i18n.t('troubleshooting.logs_filter_all'))
  if (logFilter.value) {
    parts.push(i18n.t('troubleshooting.logs_filter_query', { query: logFilter.value }))
  } else {
    parts.push(i18n.t('troubleshooting.logs_filter_live'))
  }
  return parts.join(' · ')
})

function resetLogTailState() {
  ++logTailGeneration
  logTailState = createLogTailState()
  logs.value = ''
  logsTruncated.value = false
  logsReset.value = false
  logStartOffset.value = 0
  logEndOffset.value = 0
}

function refreshLogs() {
  if (logRefreshInFlight) {
    logRefreshQueued = true
    return logRefreshInFlight
  }

  const generation = logTailGeneration
  const hadCursor = logTailState.endOffset !== null
  logRefreshInFlight = fetchLogTail(logTailState)
    .then((nextState) => {
      if (generation !== logTailGeneration) return
      logTailState = nextState
      logs.value = nextState.text
      logsTruncated.value = nextState.truncated
      logsReset.value = hadCursor && nextState.reset
      logStartOffset.value = nextState.startOffset ?? 0
      logEndOffset.value = nextState.endOffset ?? 0
    })
    .catch(error => console.error("Error fetching logs:", error))
    .finally(() => {
      logRefreshInFlight = null
      if (logRefreshQueued) {
        logRefreshQueued = false
        refreshLogs()
      }
    })
  return logRefreshInFlight
}

function refreshNetworkPathProbe() {
  fetch("./api/support/network-path-probe", { credentials: 'include' })
    .then((response) => {
      if (!response.ok) throw new Error(`HTTP ${response.status}`)
      return response.json()
    })
    .then((probe) => {
      nativeNetworkPathProbe.value = probe?.status === false ? null : probe
    })
    .catch((error) => {
      console.error("Error fetching network path probe:", error)
      nativeNetworkPathProbe.value = null
    })
}

function closeApp() {
  closeAppPressed.value = true
  return fetch("./api/apps/close", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      closeAppStatus.value = r.status
      if (!r.status) throw new Error(r.error || 'Failed to close app')
      showToast(i18n.t('troubleshooting.force_close_success') || 'Application closed successfully.', 'success')
      setTimeout(() => { closeAppStatus.value = null }, 5000)
    })
    .catch((error) => {
      closeAppStatus.value = false
      console.error(error)
      showToast(i18n.t('troubleshooting.force_close_error') || 'Error while closing application.', 'error')
      throw error
    })
    .finally(() => {
      closeAppPressed.value = false
    })
}

function copyLogs() {
  if (!actualLogs.value.trim()) {
    showToast(i18n.t('troubleshooting.copy_logs_empty') || 'No visible logs to copy.', 'info')
    return
  }
  // Every other export path redacts. This one did not, so a log line
  // holding a credential went to the clipboard intact and straight into
  // wherever it was pasted.
  navigator.clipboard.writeText(redactSensitiveText(actualLogs.value))
  showToast(i18n.t('troubleshooting.copy_logs_success') || 'Visible log lines copied.', 'success')
}

function copyRecentIssues() {
  if (!recentIssueSummaryText.value.trim()) {
    showToast(i18n.t('troubleshooting.copy_recent_issues_empty') || 'No recent warnings or errors to copy.', 'info')
    return
  }

  navigator.clipboard.writeText(recentIssueSummaryText.value)
  showToast(i18n.t('troubleshooting.copy_recent_issues_success') || 'Recent warnings and errors copied.', 'success')
}

function clearLogs() {
  clearingLogs.value = true
  fetch("./api/logs/clear", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      if (!r.status) {
        throw new Error(r.error || "Failed to clear logs")
      }

      resetLogTailState()
      showToast(i18n.t('troubleshooting.logs_clear_success') || 'Logs cleared.', 'success')
      refreshLogs()
    })
    .catch((error) => {
      console.error(error)
      showToast(i18n.t('troubleshooting.logs_clear_error') || 'Failed to clear logs.', 'error')
    })
    .finally(() => {
      clearingLogs.value = false
    })
}

async function safeFetchJson(url) {
  try {
    const response = await fetch(url, { credentials: 'include' })
    if (!response.ok) {
      return { _error: `HTTP ${response.status}` }
    }
    return await response.json()
  } catch (error) {
    return { _error: error instanceof Error ? error.message : String(error) }
  }
}

async function safeFetchText(url) {
  try {
    const response = await fetch(url, { credentials: 'include' })
    if (!response.ok) {
      return ''
    }
    return await response.text()
  } catch (error) {
    return ''
  }
}

async function refreshLastRun() {
  const result = await safeFetchJson('./polaris/v1/diagnostics/last-run')
  lastRun.value = result && !result._error ? result : null
}

const previousRunSummary = computed(() => describePreviousRun(lastRun.value || {}))
const showCrashBanner = computed(() => Boolean(previousRunSummary.value) && !crashBannerDismissed.value)

function dismissCrashBanner() {
  crashBannerDismissed.value = true
}

async function openPrefilledIssue() {
  generatingIssueDraft.value = true
  try {
    const context = await collectSupportContext()
    // The bundle is downloaded first so the user has the attachment in hand by
    // the time the form opens. Nothing is submitted for them.
    const bundle = buildAnonymizedDiagnosticsBundle(context)
    const timestamp = new Date().toISOString().replace(/[:]/g, '-')
    triggerDownload(`polaris-anonymized-diagnostics-${timestamp}.json`, JSON.stringify(bundle, null, 2), 'application/json;charset=utf-8')
    window.open(buildGithubIssueUrl(context), '_blank', 'noopener')
    showToast(i18n.t('troubleshooting.report_problem_success') || 'Support bundle downloaded. Attach it to the issue that just opened.', 'success')
  } catch (error) {
    console.error(error)
    showToast(i18n.t('troubleshooting.report_problem_error') || 'Failed to prepare the report.', 'error')
  } finally {
    generatingIssueDraft.value = false
  }
}

function triggerDownload(filename, content, mimeType) {
  const blob = new Blob([content], { type: mimeType })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = filename
  document.body.appendChild(link)
  link.click()
  document.body.removeChild(link)
  URL.revokeObjectURL(url)
}

function clearAiCache() {
  clearingAiCache.value = true
  fetch("./api/ai/cache/clear", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      if (!r.status) {
        throw new Error(r.error || 'Failed to clear AI cache')
      }

      showToast(i18n.t('troubleshooting.clear_ai_cache_success') || 'AI optimization cache cleared.', 'success')
    })
    .catch((error) => {
      console.error(error)
      showToast(i18n.t('troubleshooting.clear_ai_cache_error') || 'Failed to clear AI optimization cache.', 'error')
    })
    .finally(() => {
      clearingAiCache.value = false
    })
}

function cleanupStaleVirtualDisplay() {
  cleaningStaleVirtualDisplay.value = true
  return fetch("./api/virtual-display/cleanup-stale", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      if (!r.status) {
        throw new Error(r.error || 'Failed to clean stale virtual display state')
      }

      if (r.cleaned) {
        showToast(i18n.t('troubleshooting.cleanup_stale_virtual_display_success') || 'Stale virtual display state cleaned up.', 'success')
      } else {
        showToast(i18n.t('troubleshooting.cleanup_stale_virtual_display_none') || 'No stale virtual display state was found.', 'info')
      }
    })
    .catch((error) => {
      console.error(error)
      showToast(i18n.t('troubleshooting.cleanup_stale_virtual_display_error') || 'Failed to clean stale virtual display state.', 'error')
    })
    .finally(() => {
      cleaningStaleVirtualDisplay.value = false
    })
}

async function collectSupportContext() {
  const [systemStats, aiStatus, aiCache, aiHistory, config, previousRunLogs] = await Promise.all([
    safeFetchJson('./api/stats/system'),
    safeFetchJson('./api/ai/status'),
    safeFetchJson('./api/ai/cache'),
    safeFetchJson('./api/ai/history'),
    safeFetchJson('./api/config'),
    // The run that crashed cannot serve its own log. This is the retained copy.
    safeFetchText('./polaris/v1/diagnostics/logs/previous')
  ])

  return {
    crash: lastRun.value || {},
    previous_run_logs: previousRunLogs,
    silent_failures: streamStats.value?.doctor?.silent_failures || [],
    user_notes: userNotes.value,
    generated_at: new Date().toISOString(),
    platform: platform.value || config.platform || 'unknown',
    version: version.value || config.version || 'unknown',
    browser_user_agent: navigator.userAgent,
    stream_stats_connected: streamStatsConnected.value,
    network_path_probe: nativeNetworkPathProbe.value,
    fix_my_stream_checklist: fixMyStreamChecklist.value,
    session_snapshot: streamStats.value,
    client: {
      type: streamStats.value?.client_type || streamStats.value?.client_name || 'unknown',
      name: streamStats.value?.client_name || '',
    },
    config,
    system_stats: systemStats,
    ai_status: aiStatus,
    ai_cache: aiCache,
    ai_history: aiHistory,
    recent_issues: groupedRecentIssues.value,
    logs: logs.value
  }
}

async function createSupportBundle(providedContext = null) {
  const context = providedContext || await collectSupportContext()
  return buildAnonymizedDiagnosticsBundle({
    ...context,
    issue_draft: buildGithubIssueDraft(context),
  })
}

async function copyIssueDraft() {
  generatingIssueDraft.value = true
  try {
    const context = await collectSupportContext()
    await navigator.clipboard.writeText(buildGithubIssueDraft(context))
    showToast(i18n.t('troubleshooting.copy_issue_draft_success') || 'Issue draft copied.', 'success')
  } catch (error) {
    console.error(error)
    showToast(i18n.t('troubleshooting.issue_draft_error') || 'Failed to build issue draft.', 'error')
  } finally {
    generatingIssueDraft.value = false
  }
}

async function downloadIssueDraft() {
  generatingIssueDraft.value = true
  try {
    const context = await collectSupportContext()
    const timestamp = new Date().toISOString().replace(/[:]/g, '-')
    triggerDownload(`polaris-issue-draft-${timestamp}.md`, buildGithubIssueDraft(context), 'text/markdown;charset=utf-8')
    showToast(i18n.t('troubleshooting.download_issue_draft_success') || 'Issue draft downloaded.', 'success')
  } catch (error) {
    console.error(error)
    showToast(i18n.t('troubleshooting.issue_draft_error') || 'Failed to build issue draft.', 'error')
  } finally {
    generatingIssueDraft.value = false
  }
}

async function requestAiDoctorExplanation() {
  requestingAiDoctorExplanation.value = true
  try {
    // Provider controls stay local to Polaris. The exported support bundle is
    // intentionally anonymized, so fields such as ai_auth_mode are redacted
    // and must never be recycled as live provider configuration.
    const context = await collectSupportContext()
    const supportBundle = await createSupportBundle(context)
    const config = context.config || {}
    const result = await explainDoctorWithAi({
      aiEnabled: config.ai_enabled === true || config.ai_enabled === 'enabled' || config.ai_enabled === 'true',
      config,
      supportBundle,
      deterministicSummary: doctorPlainDiagnosis.value,
    })
    aiDoctorExplanation.value = result.explanation || null
    if (result.disabled) {
      showToast(result.error || i18n.t('troubleshooting.ai_doctor_disabled'), 'info')
    } else if (!result.status) {
      showToast(i18n.t('troubleshooting.ai_doctor_fallback') || 'AI explanation unavailable; showing deterministic Doctor output.', 'info')
    } else {
      showToast(i18n.t('troubleshooting.ai_doctor_success') || 'AI Doctor explanation ready.', 'success')
    }
  } catch (error) {
    console.error(error)
    showToast(i18n.t('troubleshooting.ai_doctor_error') || 'Failed to request AI Doctor explanation.', 'error')
  } finally {
    requestingAiDoctorExplanation.value = false
  }
}

async function downloadSupportBundle() {
  downloadingSupportBundle.value = true

  try {
    const bundle = await createSupportBundle()

    const timestamp = new Date().toISOString().replace(/[:]/g, '-')
    triggerDownload(`polaris-anonymized-diagnostics-${timestamp}.json`, JSON.stringify(bundle, null, 2), 'application/json;charset=utf-8')
    showToast(i18n.t('troubleshooting.download_support_bundle_success') || 'Support bundle downloaded.', 'success')
  } catch (error) {
    console.error(error)
    showToast(i18n.t('troubleshooting.download_support_bundle_error') || 'Failed to build support bundle.', 'error')
  } finally {
    downloadingSupportBundle.value = false
  }
}

function restart() {
  serverRestarting.value = true
  showToast(i18n.t('troubleshooting.restart_polaris_success') || 'Polaris is restarting...', 'info')

  return requestHostRestart({
    onReady: () => {
      serverRestarting.value = false
      showToast(i18n.t('troubleshooting.restart_ready') || 'Polaris is back online.', 'success')
      refreshLogs()
    },
    onTimeout: () => {
      serverRestarting.value = false
      showToast(i18n.t('troubleshooting.restart_timeout') || 'Restart requested. If the Web UI is still unavailable, wait a moment and refresh manually.', 'info')
    },
  }).catch((e) => {
    serverRestarting.value = false
    console.error(e)
    showToast(i18n.t('troubleshooting.restart_polaris_error') || 'Failed to request Polaris restart.', 'error')
  })
}

function quit() {
  serverQuitting.value = true
  return fetch("./api/quit", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
  .then(() => {
    serverQuitting.value = false
    serverQuit.value = false
    showToast(i18n.t('troubleshooting.quit_polaris_error') || 'Exit failed!', 'error')
  })
  .catch(() => {
    serverQuitting.value = false
    serverQuit.value = true
    showToast(i18n.t('troubleshooting.quit_polaris_success') || 'Polaris has exited.', 'success')
  })
}

function ddResetPersistence() {
  ddResetPressed.value = true
  fetch("/api/reset-display-device-persistence", {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' }
  })
    .then((r) => r.json())
    .then((r) => {
      ddResetPressed.value = false
      ddResetStatus.value = r.status
      setTimeout(() => { ddResetStatus.value = null }, 5000)
    })
}

// created() logic
fetch("/api/config")
  .then((r) => r.json())
  .then((r) => {
    platform.value = r.platform
    version.value = r.version || ''
  })

logInterval = setInterval(() => { refreshLogs() }, 5000)
refreshLogs()
refreshNetworkPathProbe()
refreshLastRun()

onBeforeUnmount(() => {
  clearInterval(logInterval)
})
</script>
