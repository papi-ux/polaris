<template>
  <!-- Shown only while a Space is running, changing or needs attention; one compact panel. -->
  <section class="mc-panel" data-dashboard-spaces aria-labelledby="dashboard-spaces-title">
    <div class="flex flex-wrap items-center justify-between gap-x-4 gap-y-2">
      <h2 id="dashboard-spaces-title" class="mc-panel-label">{{ $t(model.hasActivity ? 'dashboard_spaces.active_title' : 'dashboard_spaces.title') }}</h2>
      <div class="flex flex-wrap items-center gap-x-4 gap-y-2">
        <span class="meta-pill" :class="model.attention ? 'border-warning/30 text-warning-bright' : ''" data-spaces-attention>
          {{ $t(model.attention ? 'dashboard_spaces.attention' : model.changing ? 'dashboard_spaces.updating' : 'dashboard_spaces.reported') }}
        </span>
        <router-link to="/spaces" class="focus-ring mc-inline-link">{{ $t('dashboard_spaces.manage') }}</router-link>
        <button type="button" class="focus-ring mc-inline-link disabled:opacity-50" :disabled="loading" data-spaces-refresh @click="$emit('refresh')">{{ $t(loading ? 'dashboard_spaces.refreshing' : 'dashboard_spaces.refresh') }}</button>
      </div>
    </div>
    <p v-if="model.stale" class="mt-3 text-sm text-warning-bright" role="alert">{{ $t('dashboard_spaces.stale') }}</p>
    <p v-else-if="!model.activityKnown" class="mt-3 text-sm text-storm">{{ $t('dashboard_spaces.activity_unknown') }}</p>
    <ul v-if="model.sessions.length" class="mt-3 divide-y divide-storm/20" :aria-label="$t('dashboard_spaces.sessions')">
      <li v-for="session in model.sessions" :key="session.key" class="flex flex-wrap items-center justify-between gap-2 py-3" data-space-session>
        <div class="min-w-0">
          <div class="break-words font-medium text-silver">{{ session.name || $t('dashboard_spaces.unnamed') }}</div>
          <div class="mt-1 text-xs text-storm">{{ session.client || $t('dashboard_spaces.unknown_device') }}<template v-if="session.family"> · {{ familyName(session.family) }}</template></div>
        </div>
        <span class="data-pill">{{ $t(`dashboard_spaces.${model.stale ? 'last_' : ''}${session.state}`) }}</span>
      </li>
    </ul>
    <p v-if="model.remaining" class="mt-2 text-xs text-storm">{{ $t('dashboard_spaces.more', { count: model.remaining }) }}</p>
    <ul v-if="model.attention && !model.stale" class="mt-3 list-inside list-disc text-sm text-warning-bright">
      <li v-if="model.failed || !model.available">{{ $t('dashboard_spaces.unavailable') }}</li>
      <li v-if="model.mismatch">{{ $t('dashboard_spaces.mismatch', { count: model.mismatch }) }}</li>
      <li v-if="model.unsupported">{{ $t('dashboard_spaces.unsupported', { count: model.unsupported }) }}</li>
      <li v-if="model.jobFailed">{{ $t('dashboard_spaces.job_failed') }}</li>
    </ul>
    <p v-if="model.previewBlocked" class="mt-3 rounded-lg border border-storm/20 px-3 py-2 text-sm text-storm" data-space-preview-note>{{ $t('dashboard_spaces.no_preview') }}</p>
  </section>
</template>

<script setup>
defineProps({ model: { type: Object, required: true }, loading: Boolean })
defineEmits(['refresh'])
const familyName = family => ({ steam: 'Steam', heroic: 'Heroic', lutris: 'Lutris' })[family] || ''
</script>
