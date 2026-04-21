<template>
  <div class="page-shell">
    <section class="page-header">
      <div class="page-heading">
        <div class="section-kicker">{{ $t('navbar.pairing') }}</div>
        <h1 class="page-title">Pair clients</h1>
        <p class="page-subtitle">Start with one route, then review saved devices.</p>
        <div class="page-meta">
          <span class="meta-pill">{{ pairedCount }} saved</span>
          <span class="meta-pill">{{ activePairingMethodLabel }}</span>
        </div>
      </div>
    </section>

    <section id="pair_device" class="section-card">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-end lg:justify-between">
        <div class="min-w-0">
          <div class="section-kicker">{{ $t('pin.pair_device') }}</div>
          <h2 class="section-title">Pair a device</h2>
        </div>
      </div>

      <div class="pairing-methods mt-5 grid gap-3 lg:grid-cols-3">
        <button
          v-for="method in pairingMethods"
          :key="method.key"
          type="button"
          class="focus-ring pairing-method-card rounded-xl border px-4 py-3 text-left transition-[border-color,background-color,box-shadow] duration-200"
          :class="currentTab === method.key
            ? 'border-ice/40 bg-twilight/70 shadow-[0_0_32px_rgba(124,110,255,0.12)]'
            : 'border-storm/20 bg-deep/40 hover:border-storm/40 hover:bg-twilight/30'"
          :aria-pressed="currentTab === method.key"
          @click="switchTab(method.key)"
        >
          <div class="flex items-center justify-between gap-3">
            <div class="text-sm font-semibold text-silver">{{ $t(method.titleKey) }}</div>
            <span
              v-if="method.badgeKey"
              class="rounded-full border px-2 py-0.5 text-[10px] font-medium uppercase tracking-[0.18em]"
              :class="method.badgeTone === 'recommended'
                ? 'border-ice/30 bg-ice/10 text-ice'
                : 'border-amber-300/25 bg-amber-300/10 text-amber-200'"
            >
              {{ $t(method.badgeKey) }}
            </span>
          </div>
          <p v-if="currentTab === method.key" class="mt-2 text-xs text-storm">{{ $t(method.descKey) }}</p>
        </button>
      </div>

      <div class="pairing-flow-strip mt-5">
        <div class="pairing-flow-copy">
          <span class="meta-pill">{{ activePairingMethodLabel }}</span>
          <p class="pairing-flow-text">{{ activePairingSummary }}</p>
        </div>
        <div class="pairing-inline-note">
          <span class="pairing-inline-note-label">{{ $t('_common.warning') }}</span>
          <span>Review access before sharing the client.</span>
        </div>
      </div>

      <div class="pairing-stage mt-5">
        <div class="pairing-main space-y-4">
          <section v-if="currentTab === 'TOFU'" class="surface-subtle p-5">
            <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
              <div class="min-w-0">
                <h3 class="section-title">{{ $t('pin.method_trusted_network') }}</h3>
                <p class="mt-2 text-sm text-storm">{{ $t('pin.trusted_network_desc') }}</p>
              </div>
              <a
                href="#/config"
                class="inline-flex h-9 items-center justify-center rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)]"
              >
                {{ $t('pin.configure_network') }}
              </a>
            </div>

            <div class="mt-5">
              <div class="mb-2 text-xs font-medium uppercase tracking-[0.18em] text-storm">{{ $t('pin.trusted_subnets_label') }}</div>
              <div v-if="tofuSubnets.length > 0" class="flex flex-wrap gap-2">
                <code
                  v-for="(subnet, i) in tofuSubnets"
                  :key="i"
                  class="rounded-lg border border-storm/20 bg-void/50 px-3 py-2 text-sm font-mono text-silver"
                >
                  {{ subnet }}
                </code>
              </div>
              <div v-else class="rounded-xl border border-storm/20 bg-void/50 px-4 py-3 text-sm text-storm">
                {{ $t('pin.trusted_network_empty') }}
              </div>
            </div>
          </section>

          <form
            v-if="currentTab === 'PIN'"
            class="surface-subtle p-5"
            @submit.prevent="registerDevice"
          >
            <div class="min-w-0">
              <h3 class="section-title">{{ $t('pin.manual_pin_title') }}</h3>
              <p class="mt-2 text-sm text-storm">{{ $t('pin.manual_pin_desc') }}</p>
            </div>

            <div class="mt-5 grid gap-4">
              <div>
                <label for="pin-input" class="mb-1 block text-sm font-medium text-storm">{{ $t('pin.pin_code') }}</label>
                <input
                  id="pin-input"
                  v-model="pinCode"
                  name="pairing_pin"
                  autocomplete="one-time-code"
                  inputmode="numeric"
                  pattern="\\d*"
                  required
                  class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
                  :placeholder="$t('pin.pin_code_placeholder')"
                />
              </div>

              <div>
                <label for="name-input" class="mb-1 block text-sm font-medium text-storm">{{ $t('pin.device_name_label') }}</label>
                <input
                  id="name-input"
                  v-model="pinDeviceName"
                  name="pairing_device_name"
                  autocomplete="off"
                  class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
                  :placeholder="$t('pin.device_name_placeholder')"
                />
              </div>

              <div class="flex flex-wrap items-center gap-3">
                <button
                  type="submit"
                  class="inline-flex h-9 items-center justify-center rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)]"
                >
                  {{ $t('pin.send') }}
                </button>
              </div>
            </div>

            <div
              v-if="pinMessage"
              class="mt-4 rounded-lg border-l-4 p-3 text-silver"
              :class="pinStatus === 'success'
                ? 'border-green-500 bg-twilight/50'
                : 'border-red-500 bg-twilight/50'"
            >
              {{ pinMessage }}
            </div>
          </form>

          <form
            v-if="currentTab === 'OTP'"
            class="surface-subtle p-5"
            @submit.prevent="requestOTP"
          >
            <div class="flex flex-col gap-5 lg:grid lg:grid-cols-[minmax(0,0.95fr)_minmax(220px,0.85fr)]">
              <div class="space-y-4">
                <div class="min-w-0">
                  <h3 class="section-title">{{ otp ? $t('pin.qr_ready_title') : $t('pin.qr_title') }}</h3>
                  <p class="mt-2 text-sm text-storm">{{ otp ? $t('pin.qr_ready_desc') : $t('pin.qr_desc') }}</p>
                </div>

                <div v-if="editingHost" class="grid gap-3">
                  <div>
                    <label for="otp-host" class="mb-1 block text-sm font-medium text-storm">{{ $t('pin.host_address') }}</label>
                    <input
                      id="otp-host"
                      v-model="hostAddr"
                      name="otp_host"
                      autocomplete="off"
                      spellcheck="false"
                      class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
                      :placeholder="$t('pin.host_address_placeholder')"
                    />
                  </div>
                  <div>
                    <label for="otp-port" class="mb-1 block text-sm font-medium text-storm">{{ $t('pin.host_port') }}</label>
                    <input
                      id="otp-port"
                      v-model="hostPort"
                      name="otp_port"
                      autocomplete="off"
                      inputmode="numeric"
                      spellcheck="false"
                      class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
                      :placeholder="$t('pin.host_port_placeholder')"
                    />
                  </div>
                  <div class="flex gap-3">
                    <button
                      type="button"
                      class="inline-flex h-9 items-center justify-center rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] disabled:opacity-50"
                      :disabled="!canSaveHost"
                      @click="saveHost"
                    >
                      {{ $t('_common.save') }}
                    </button>
                    <button
                      type="button"
                      class="inline-flex h-9 items-center justify-center rounded-lg border border-storm px-4 text-sm font-medium text-silver transition-all duration-200 hover:border-ice hover:text-ice"
                      @click="editHost"
                    >
                      {{ $t('_common.cancel') }}
                    </button>
                  </div>
                </div>

                <div v-else class="grid gap-4">
                  <div>
                    <label for="otp-passphrase" class="mb-1 block text-sm font-medium text-storm">{{ $t('pin.otp_passphrase') }}</label>
                    <input
                      id="otp-passphrase"
                      v-model="passphrase"
                      name="otp_passphrase"
                      autocomplete="off"
                      pattern="[0-9a-zA-Z]{4,}"
                      required
                      spellcheck="false"
                      class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
                      :placeholder="$t('pin.otp_passphrase_placeholder')"
                    />
                  </div>

                  <div>
                    <label for="otp-device-name" class="mb-1 block text-sm font-medium text-storm">{{ $t('pin.device_name_label') }}</label>
                    <input
                      id="otp-device-name"
                      v-model="deviceName"
                      name="otp_device_name"
                      autocomplete="off"
                      class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
                      :placeholder="$t('pin.device_name_placeholder')"
                    />
                  </div>

                  <div class="flex flex-wrap items-center gap-3">
                    <button
                      type="submit"
                      class="inline-flex h-9 items-center justify-center gap-2 rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)]"
                    >
                      <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v1m6 11h2m-6 0h-2v4m0-11v3m0 0h.01M12 12h4.01M16 20h4M4 12h4m12 0h.01M5 8h2a1 1 0 001-1V5a1 1 0 00-1-1H5a1 1 0 00-1 1v2a1 1 0 001 1zm12 0h2a1 1 0 001-1V5a1 1 0 00-1-1h-2a1 1 0 00-1 1v2a1 1 0 001 1zM5 20h2a1 1 0 001-1v-2a1 1 0 00-1-1H5a1 1 0 00-1 1v2a1 1 0 001 1z"/>
                      </svg>
                      {{ otp ? $t('pin.regenerate_qr') : $t('pin.generate_qr') }}
                    </button>
                  </div>
                </div>
              </div>

              <div class="rounded-2xl border border-storm/20 bg-void/50 p-4">
                <div v-if="otp && hostAddr" class="flex flex-col items-center text-center">
                  <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm">{{ $t('pin.scan_with_nova') }}</div>
                  <div id="qrRef" class="mt-3 flex justify-center"></div>
                  <div class="mt-3 text-xs text-storm">{{ $t('pin.scan_or_open') }}</div>
                  <div class="mt-2 flex items-center gap-2 break-all text-sm text-silver">
                    <a class="text-ice hover:text-ice/80" :href="deepLink">{{ `art://${hostAddr}:${hostPort}` }}</a>
                    <button
                      type="button"
                      class="text-storm transition-colors hover:text-ice"
                      :aria-label="$t('pin.edit_host')"
                      @click="editHost"
                    >
                      <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z"/>
                      </svg>
                    </button>
                  </div>
                  <div class="mt-4 text-xs text-storm">{{ $t('pin.or_enter_pin') }}</div>
                  <h4 class="mt-2 text-3xl font-bold tracking-[0.3em] text-ice">{{ otp }}</h4>
                </div>
                <div v-else class="space-y-3">
                  <div class="text-sm font-medium text-silver">{{ $t('pin.qr_preview_title') }}</div>
                  <p class="text-sm text-storm">{{ $t('pin.qr_preview_desc') }}</p>
                  <div class="rounded-xl border border-dashed border-storm/30 bg-deep/50 px-4 py-8 text-center text-sm text-storm">
                    {{ $t('pin.qr_waiting') }}
                  </div>
                </div>
              </div>
            </div>

            <div
              v-if="otpMessage"
              class="mt-4 rounded-lg border-l-4 p-3 text-silver"
              :class="otpStatus === 'success'
                ? 'border-green-500 bg-twilight/50'
                : otpStatus === 'danger'
                  ? 'border-red-500 bg-twilight/50'
                  : 'border-yellow-500 bg-twilight/50'"
            >
              {{ otpMessage }}
            </div>
          </form>
        </div>
      </div>
    </section>

    <div v-if="!clientsLoaded" class="space-y-3">
      <Skeleton type="card" />
      <Skeleton type="card" />
    </div>

    <section id="device_management" v-else class="section-card overflow-hidden">
      <div class="p-5">
        <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
          <div class="min-w-0">
            <div class="section-kicker">{{ $t('pin.paired_devices') }}</div>
            <h2 id="unpair" class="section-title">Saved devices</h2>
          </div>
          <div class="pairing-device-toolbar flex flex-wrap items-center gap-2">
            <span class="rounded-full border border-storm/30 bg-deep/60 px-2.5 py-1 text-xs text-storm">
              {{ pairedCount }} saved
            </span>
            <span class="rounded-full border border-storm/30 bg-deep/60 px-2.5 py-1 text-xs text-storm">
              {{ connectedCount }} connected
            </span>
            <button
              type="button"
              class="inline-flex h-9 items-center justify-center rounded-lg bg-red-500 px-4 text-sm font-medium text-white transition-all duration-200 hover:bg-red-600 hover:shadow-[0_0_24px_rgba(239,68,68,0.2)] disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="unpairAllPressed || clients.length === 0"
              @click="unpairAll"
            >
              {{ $t('pin.unpair_all') }}
            </button>
          </div>
        </div>

        <div v-if="showApplyMessage" class="mt-4 flex items-center gap-3 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver">
          <div><b>{{ $t('_common.success') }}</b> {{ $t('pin.unpair_single_success') }}</div>
          <button
            type="button"
            class="ml-auto rounded-lg bg-ice/20 px-3 py-1 text-sm text-ice transition hover:bg-ice/30"
            @click="clickedApplyBanner"
          >
            {{ $t('_common.dismiss') }}
          </button>
        </div>
        <div v-if="unpairAllStatus === true" class="mt-4 rounded-lg border-l-4 border-green-500 bg-twilight/50 p-3 text-silver">
          {{ $t('pin.unpair_all_success') }}
        </div>
        <div v-if="unpairAllStatus === false" class="mt-4 rounded-lg border-l-4 border-red-500 bg-twilight/50 p-3 text-silver">
          {{ $t('pin.unpair_all_error') }}
        </div>
      </div>

      <div class="border-t border-storm/30 p-5">
        <div v-if="clients.length === 0" class="rounded-2xl border border-storm/20 bg-deep/40 p-6 text-center">
          <em class="text-storm">{{ $t('pin.unpair_single_no_devices') }}</em>
        </div>

        <div v-else class="space-y-4">
          <article
            v-for="client in clients"
            :key="client.uuid"
            :class="client.editing
              ? 'fixed inset-0 z-50 flex items-center justify-end bg-black/70 p-4 sm:p-6'
              : 'rounded-2xl border border-storm/20 bg-deep/40'"
            :role="client.editing ? 'dialog' : undefined"
            :aria-modal="client.editing ? 'true' : undefined"
            :aria-labelledby="client.editing ? `client-edit-title-${client.uuid}` : undefined"
            tabindex="-1"
            @click.self="cancelEdit(client)"
            @keydown.esc.stop.prevent="cancelEdit(client)"
          >
            <div v-if="client.editing" class="w-full max-w-[1120px] max-h-[calc(100vh-2rem)] overflow-y-auto rounded-2xl border border-storm/20 bg-deep/95 shadow-[0_24px_80px_rgba(0,0,0,0.55)]">
            <div class="p-5">
              <div class="pairing-client-toolbar flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
                <div>
                  <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm">{{ $t('pin.edit_access') }}</div>
                  <h3 :id="`client-edit-title-${client.uuid}`" class="mt-2 text-lg font-semibold text-silver">{{ client.name || $t('pin.unpair_single_unknown') }}</h3>
                  <p class="mt-2 text-sm text-storm">Update access, display behavior, and client actions.</p>
                </div>
                <div class="pairing-client-actions flex flex-wrap items-center gap-2">
                  <span
                    class="rounded-full border px-2.5 py-1 text-xs font-medium"
                    :class="client.connected
                      ? 'border-green-500/30 bg-green-500/10 text-green-300'
                      : 'border-storm/30 bg-deep/60 text-storm'"
                  >
                    {{ client.connected ? $t('pin.status_connected') : $t('pin.status_offline') }}
                  </span>
                  <span
                    class="rounded-full border px-2.5 py-1 text-xs font-medium"
                    :class="accessToneClass(client.editPerm)"
                  >
                    {{ accessPresetLabel(client.editPerm) }}
                  </span>
                  <button
                    type="button"
                    class="inline-flex h-9 items-center justify-center rounded-lg bg-ice px-4 text-sm font-medium text-void transition-all duration-200 hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)]"
                    @click="saveClient(client)"
                  >
                    {{ $t('_common.save') }}
                  </button>
                  <button
                    type="button"
                    class="inline-flex h-9 items-center justify-center rounded-lg border border-storm px-4 text-sm font-medium text-silver transition-all duration-200 hover:border-ice hover:text-ice"
                    @click="cancelEdit(client)"
                  >
                    {{ $t('_common.cancel') }}
                  </button>
                </div>
              </div>

              <div class="mt-5 grid gap-4 xl:grid-cols-[minmax(0,1.05fr)_minmax(320px,0.95fr)]">
                <section class="rounded-2xl border border-storm/20 bg-void/40 p-4">
                  <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm">{{ $t('pin.identity_access') }}</div>
                  <h4 class="mt-2 text-base font-semibold text-silver">{{ $t('pin.access_presets') }}</h4>
                  <p class="mt-2 text-sm text-storm">{{ $t('pin.access_presets_desc') }}</p>

                  <div class="mt-4">
                    <label :for="`client-name-${client.uuid}`" class="mb-1 block text-sm font-medium text-storm">{{ $t('pin.device_name_label') }}</label>
                    <input
                      :id="`client-name-${client.uuid}`"
                      v-model="client.editName"
                      autocomplete="off"
                      class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
                      :placeholder="$t('pin.device_name_placeholder')"
                      @keyup.enter="saveClient(client)"
                    />
                  </div>

                  <div class="mt-4 flex flex-wrap gap-2">
                    <button
                      v-for="preset in permissionPresets"
                      :key="preset.key"
                      type="button"
                      class="rounded-lg border px-3 py-2 text-sm font-medium transition-all duration-200"
                      :class="permissionPresetKey(client.editPerm) === preset.key
                        ? preset.activeClass
                        : 'border-storm/30 bg-deep/40 text-storm hover:border-storm/50 hover:text-silver'"
                      @click="applyPermissionPreset(client, preset.key)"
                    >
                      {{ $t(preset.labelKey) }}
                    </button>
                  </div>

                  <details class="mt-4 rounded-xl border border-storm/20 bg-deep/40 p-4">
                    <summary class="cursor-pointer list-none text-sm font-medium text-silver">
                      {{ $t('pin.fine_tune_permissions') }}
                    </summary>
                    <div class="mt-4 flex flex-wrap gap-4">
                      <div v-for="group in permissionGroups" :key="group.name" class="flex min-w-[180px] flex-col gap-2">
                        <div class="text-xs font-medium uppercase tracking-[0.18em] text-storm">{{ group.name }}</div>
                        <button
                          v-for="perm in group.permissions"
                          :key="perm.name"
                          type="button"
                          class="rounded-lg border px-2 py-1 text-left text-xs transition"
                          :disabled="isSuppressed(client.editPerm, perm.name, perm.suppressed_by)"
                          :class="(isSuppressed(client.editPerm, perm.name, perm.suppressed_by) || checkPermission(client.editPerm, perm.name))
                            ? 'border-green-600 bg-green-600/20 text-green-400'
                            : 'border-storm bg-deep text-storm hover:border-ice hover:text-silver'"
                          @click="togglePermission(client, perm.name)"
                        >
                          {{ $t(`permissions.${perm.name}`) }}
                        </button>
                      </div>
                    </div>
                  </details>

                  <div class="mt-4 space-y-3">
                    <Checkbox
                      :id="`enable_legacy_ordering-${client.uuid}`"
                      class=""
                      label="pin.enable_legacy_ordering"
                      desc="pin.enable_legacy_ordering_desc"
                      v-model="client.editEnableLegacyOrdering"
                      default="true"
                    />

                    <Checkbox
                      v-if="platform === 'windows'"
                      :id="`always_use_virtual_display-${client.uuid}`"
                      class=""
                      label="pin.always_use_virtual_display"
                      desc="pin.always_use_virtual_display_desc"
                      v-model="client.editAlwaysUseVirtualDisplay"
                      default="false"
                    />

                    <Checkbox
                      :id="`allow_client_commands-${client.uuid}`"
                      class=""
                      label="pin.allow_client_commands"
                      desc="pin.allow_client_commands_desc"
                      v-model="client.editAllowClientCommands"
                      default="true"
                    />
                  </div>
                </section>

                <section class="rounded-2xl border border-storm/20 bg-void/40 p-4">
                  <div class="flex items-start justify-between gap-4">
                    <div>
                      <div class="text-[10px] font-semibold uppercase tracking-[0.24em] text-storm">{{ $t('pin.display_profile') }}</div>
                      <h4 class="mt-2 text-base font-semibold text-silver">{{ $t('pin.display_profile') }}</h4>
                      <p class="mt-2 text-sm text-storm">{{ $t('pin.display_profile_desc') }}</p>
                    </div>
                    <button
                      type="button"
                      class="inline-flex h-7 items-center justify-center gap-1.5 rounded-lg border border-purple-500/30 px-2.5 text-xs font-medium text-purple-400 transition-all duration-200 hover:bg-purple-500/10 disabled:opacity-50"
                      :disabled="aiLoading"
                      @click="fetchAiSuggestion(client)"
                    >
                      <svg v-if="aiLoading && aiSuggestionFor === (client.editName || client.name)" class="h-3 w-3 animate-spin" fill="none" viewBox="0 0 24 24" aria-hidden="true">
                        <circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" />
                        <path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
                      </svg>
                      <svg v-else class="h-3 w-3" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9.663 17h4.673M12 3v1m6.364 1.636l-.707.707M21 12h-1M4 12H3m3.343-5.657l-.707-.707m2.828 9.9a5 5 0 117.072 0l-.548.547A3.374 3.374 0 0014 18.469V19a2 2 0 11-4 0v-.531c0-.895-.356-1.754-.988-2.386l-.548-.547z" />
                      </svg>
                      {{ $t('pin.ai_suggest') }}
                    </button>
                  </div>

                  <div
                    v-if="aiSuggestion && aiSuggestionFor === (client.editName || client.name)"
                    class="mt-4 rounded-xl border border-purple-500/20 bg-purple-500/5 p-3"
                  >
                    <div class="flex items-center justify-between gap-3">
                      <div class="flex items-center gap-2">
                        <span class="text-sm font-medium text-purple-300">{{ $t('pin.ai_suggestion') }}</span>
                        <span class="text-xs text-storm">{{ aiSuggestion.source }}</span>
                      </div>
                      <button
                        type="button"
                        class="text-storm transition-colors hover:text-silver"
                        :aria-label="$t('_common.dismiss')"
                        @click="dismissSuggestion"
                      >
                        <svg class="h-3.5 w-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12" />
                        </svg>
                      </button>
                    </div>
                    <div class="mt-2 flex flex-wrap gap-x-4 gap-y-1 text-sm">
                      <span v-if="aiSuggestion.display_mode" class="text-silver/60">{{ $t('pin.mode_short') }} <span class="font-medium text-silver">{{ aiSuggestion.display_mode }}</span></span>
                      <span v-if="aiSuggestion.target_bitrate_kbps" class="text-silver/60">{{ $t('pin.bitrate_short') }} <span class="font-medium text-silver">{{ (aiSuggestion.target_bitrate_kbps / 1000).toFixed(0) }} Mbps</span></span>
                      <span v-if="aiSuggestion.color_range != null" class="text-silver/60">{{ $t('pin.color_short') }} <span class="font-medium text-silver">{{ colorRangeLabel(aiSuggestion.color_range) }}</span></span>
                      <span v-if="aiSuggestion.hdr != null" class="text-silver/60">HDR <span class="font-medium text-silver">{{ aiSuggestion.hdr ? 'On' : 'Off' }}</span></span>
                    </div>
                    <div v-if="aiSuggestion.reasoning" class="mt-2 text-xs italic text-silver/60">{{ aiSuggestion.reasoning }}</div>
                    <button
                      type="button"
                      class="mt-3 inline-flex h-7 items-center justify-center rounded-lg bg-purple-500 px-3 text-xs font-medium text-white transition-all duration-200 hover:bg-purple-600"
                      @click="applySuggestion(client)"
                    >
                      {{ $t('pin.apply_suggestion') }}
                    </button>
                  </div>

                  <div class="mt-4 grid gap-4 md:grid-cols-2">
                    <div class="md:col-span-2">
                      <label :for="`display-mode-${client.uuid}`" class="mb-1 block text-sm font-medium text-storm">{{ $t('pin.display_mode_override') }}</label>
                      <input
                        :id="`display-mode-${client.uuid}`"
                        v-model="client.editDisplayMode"
                        autocomplete="off"
                        class="w-full rounded-lg border border-storm bg-deep px-3 py-2 text-silver focus:border-ice focus:outline-none"
                        placeholder="1920x1080x59.94"
                        @input="validateModeOverride"
                      />
                      <div class="mt-1 text-sm text-storm">
                        {{ $t('pin.display_mode_override_desc') }}
                        <a href="https://github.com/papi-ux/polaris/wiki/Display-Mode-Override" target="_blank" class="text-ice hover:text-ice/80">
                          {{ $t('_common.learn_more') }}
                        </a>
                      </div>
                    </div>

                    <div>
                      <label class="mb-1 block text-xs font-medium uppercase tracking-[0.18em] text-storm">{{ $t('pin.output_name') }}</label>
                      <input
                        v-model="client.editProfile.output_name"
                        type="text"
                        autocomplete="off"
                        class="w-full rounded-lg border border-storm/50 bg-void/50 px-2.5 py-1.5 text-sm text-silver focus:border-ice focus:outline-none"
                        placeholder="e.g. HDMI-A-1"
                      />
                    </div>
                    <div>
                      <label class="mb-1 block text-xs font-medium uppercase tracking-[0.18em] text-storm">{{ $t('pin.color_range_label') }}</label>
                      <select
                        v-model="client.editProfile.color_range"
                        class="w-full rounded-lg border border-storm/50 bg-void/50 px-2.5 py-1.5 text-sm text-silver focus:border-ice focus:outline-none"
                      >
                        <option :value="0">{{ $t('pin.color_client_default') }}</option>
                        <option :value="1">{{ $t('pin.color_limited') }}</option>
                        <option :value="2">{{ $t('pin.color_full') }}</option>
                      </select>
                    </div>
                    <div>
                      <label class="mb-1 block text-xs font-medium uppercase tracking-[0.18em] text-storm">{{ $t('pin.wol_mac_address') }}</label>
                      <input
                        v-model="client.editProfile.mac_address"
                        type="text"
                        autocomplete="off"
                        spellcheck="false"
                        class="w-full rounded-lg border border-storm/50 bg-void/50 px-2.5 py-1.5 text-sm text-silver focus:border-ice focus:outline-none"
                        placeholder="AA:BB:CC:DD:EE:FF"
                      />
                    </div>
                    <div class="flex items-center gap-2 pt-5">
                      <input v-model="client.editProfile.hdr" type="checkbox" class="h-4 w-4 rounded border-storm bg-void text-ice accent-ice" />
                      <label class="text-sm text-silver">{{ $t('pin.enable_hdr') }}</label>
                    </div>
                  </div>
                </section>
              </div>

              <details
                v-if="client.editAllowClientCommands"
                class="mt-4 rounded-2xl border border-storm/20 bg-void/40 p-4"
              >
                <summary class="cursor-pointer list-none text-sm font-medium text-silver">
                  {{ $t('pin.client_commands_section') }}
                </summary>

                <div class="mt-4 space-y-5">
                  <div v-for="cmdType in ['do', 'undo']" :key="cmdType" class="flex flex-col">
                    <label class="block text-sm font-medium text-storm mb-1">{{ $t(`pin.client_${cmdType}_cmd`) }}</label>
                    <div class="text-sm text-storm">
                      {{ $t(`pin.client_${cmdType}_cmd_desc`) }}
                      <a href="https://github.com/papi-ux/polaris/wiki/Client-Commands" target="_blank" class="text-ice hover:text-ice/80">
                        {{ $t('_common.learn_more') }}
                      </a>
                    </div>
                    <table v-if="client[`edit_${cmdType}`].length > 0" class="mt-2 w-full text-left">
                      <thead>
                        <tr class="border-b border-storm">
                          <th class="py-2 text-sm text-storm" style="width: 80%">{{ $t('_common.cmd_val') }}</th>
                          <th v-if="platform === 'windows'" class="py-2 text-sm text-storm" style="min-width: 10em; max-width: 12em;">{{ $t('_common.run_as') }}</th>
                          <th class="py-2" style="min-width: 110px;"></th>
                        </tr>
                      </thead>
                      <tbody>
                        <tr v-for="(c, i) in client[`edit_${cmdType}`]" :key="`${cmdType}-${i}`" class="border-b border-storm/50">
                          <td class="py-2 pr-2">
                            <input
                              v-model="c.cmd"
                              type="text"
                              autocomplete="off"
                              class="w-full rounded-lg border border-storm bg-deep px-3 py-2 font-mono text-sm text-silver focus:border-ice focus:outline-none"
                            />
                          </td>
                          <td v-if="platform === 'windows'" class="py-2 pr-2">
                            <div class="flex items-center gap-2">
                              <input :id="`client-${cmdType}-cmd-admin-${i}`" v-model="c.elevated" type="checkbox" class="h-4 w-4 rounded border-storm bg-void text-ice" />
                              <label :for="`client-${cmdType}-cmd-admin-${i}`" class="text-sm text-storm">{{ $t('_common.elevated') }}</label>
                            </div>
                          </td>
                          <td class="py-2">
                            <div class="flex gap-1">
                              <button
                                type="button"
                                class="rounded bg-red-500 px-2 py-1 text-white transition hover:bg-red-600"
                                :aria-label="$t('pin.remove_command')"
                                @click="removeCmd(client[`edit_${cmdType}`], i)"
                              >
                                <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                                  <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
                                </svg>
                              </button>
                              <button
                                type="button"
                                class="rounded bg-ice/20 px-2 py-1 text-ice transition hover:bg-ice/30"
                                :aria-label="$t('pin.add_command_after')"
                                @click="addCmd(client[`edit_${cmdType}`], i)"
                              >
                                <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                                  <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4" />
                                </svg>
                              </button>
                            </div>
                          </td>
                        </tr>
                      </tbody>
                    </table>
                    <button
                      type="button"
                      class="mt-2 self-start rounded-lg bg-ice/20 px-3 py-1 text-sm text-ice transition hover:bg-ice/30"
                      @click="addCmd(client[`edit_${cmdType}`], -1)"
                    >
                      + {{ $t('config.add') }}
                    </button>
                  </div>
                </div>
              </details>
            </div>
            </div>

            <div v-else class="p-5">
              <div class="pairing-client-toolbar flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
                <div class="min-w-0">
                  <div class="flex flex-wrap items-center gap-2">
                    <h3 class="text-lg font-semibold text-silver">{{ client.name !== '' ? client.name : $t('pin.unpair_single_unknown') }}</h3>
                    <span
                      class="rounded-full border px-2.5 py-1 text-xs font-medium"
                      :class="client.connected
                        ? 'border-green-500/30 bg-green-500/10 text-green-300'
                        : 'border-storm/30 bg-deep/60 text-storm'"
                    >
                      {{ client.connected ? $t('pin.status_connected') : $t('pin.status_offline') }}
                    </span>
                    <span
                      class="rounded-full border px-2.5 py-1 text-xs font-medium"
                      :class="accessToneClass(client.perm)"
                    >
                      {{ accessPresetLabel(client.perm) }}
                    </span>
                  </div>
                  <div class="mt-3 font-mono text-xs text-storm">[ {{ permToStr(client.perm) }} ]</div>
                </div>

                <div class="pairing-client-actions flex flex-wrap items-center gap-2">
                  <button
                    v-if="client.name"
                    type="button"
                    class="rounded-lg bg-green-600/20 px-2.5 py-2 text-green-400 transition hover:bg-green-600/30"
                    :disabled="client.wolSending"
                    :aria-label="$t('pin.wol_send')"
                    @click="sendWol(client)"
                  >
                    <svg v-if="!client.wolSending" class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                      <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z" />
                    </svg>
                    <svg v-else class="h-4 w-4 animate-spin" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                      <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15" />
                    </svg>
                  </button>
                  <button
                    v-if="client.connected"
                    type="button"
                    class="rounded-lg bg-yellow-600 px-2.5 py-2 text-white transition hover:bg-yellow-700"
                    :aria-label="$t('pin.disconnect_client')"
                    @click="disconnectClient(client.uuid)"
                  >
                    <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                      <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13.828 10.172a4 4 0 00-5.656 0l-4 4a4 4 0 105.656 5.656l1.102-1.101m-.758-4.899a4 4 0 005.656 0l4-4a4 4 0 00-5.656-5.656l-1.1 1.1" />
                    </svg>
                  </button>
                  <button
                    type="button"
                    class="rounded-lg bg-ice/20 px-2.5 py-2 text-ice transition hover:bg-ice/30"
                    :aria-label="$t('pin.edit_access')"
                    @click="editClient(client)"
                  >
                    <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                      <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z" />
                    </svg>
                  </button>
                  <button
                    type="button"
                    class="rounded-lg bg-red-500/20 px-2.5 py-2 text-red-400 transition hover:bg-red-500/30"
                    :aria-label="$t('pin.unpair_single_action')"
                    @click="handleUnpairSingle(client.uuid)"
                  >
                    <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
                      <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
                    </svg>
                  </button>
                </div>
              </div>

              <div class="mt-4 grid gap-2 md:grid-cols-3">
                <div class="rounded-lg border border-storm/15 bg-void/25 px-3 py-2.5">
                  <div class="text-[10px] font-semibold uppercase tracking-[0.18em] text-storm">{{ $t('pin.permissions_summary') }}</div>
                  <div class="mt-1.5 text-sm font-medium text-silver">{{ accessPresetLabel(client.perm) }}</div>
                </div>
                <div class="rounded-lg border border-storm/15 bg-void/25 px-3 py-2.5">
                  <div class="text-[10px] font-semibold uppercase tracking-[0.18em] text-storm">{{ $t('pin.display_summary') }}</div>
                  <div class="mt-1.5 text-sm font-medium text-silver">{{ clientDisplaySummary(client) }}</div>
                </div>
                <div class="rounded-lg border border-storm/15 bg-void/25 px-3 py-2.5">
                  <div class="text-[10px] font-semibold uppercase tracking-[0.18em] text-storm">{{ $t('pin.client_commands_summary') }}</div>
                  <div class="mt-1.5 text-sm font-medium text-silver">{{ clientCommandSummary(client) }}</div>
                </div>
              </div>
            </div>
          </article>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup>
import { computed, inject, nextTick, ref } from 'vue'
import Checkbox from '../Checkbox.vue'
import Skeleton from '../components/Skeleton.vue'
import { useToast } from '../composables/useToast'
import { useAiOptimizer } from '../composables/useAiOptimizer'
import {
  useClients,
  permissionMapping, permissionGroups,
  permToStr, checkPermission, isSuppressed
} from '../composables/useClients'

const { toast: showToast } = useToast()
const { getSuggestion: getAiSuggestion, optimize: aiOptimize, loading: aiLoading } = useAiOptimizer()
const i18n = inject('i18n')
const {
  clients, clientsLoaded, platform, profiles,
  refreshClients, refreshProfiles, saveClient: saveClientAPI,
  saveProfile, unpairSingle, unpairAll: unpairAllAPI,
  disconnectClient, sendWol: sendWolAPI
} = useClients()

const pairingMethods = [
  { key: 'OTP', titleKey: 'pin.method_nova_qr', descKey: 'pin.method_nova_qr_desc', badgeKey: 'pin.method_recommended', badgeTone: 'recommended' },
  { key: 'TOFU', titleKey: 'pin.method_trusted_network', descKey: 'pin.method_trusted_network_desc', badgeKey: 'pin.method_advanced', badgeTone: 'advanced' },
  { key: 'PIN', titleKey: 'pin.method_manual_pin', descKey: 'pin.method_manual_pin_desc', badgeKey: 'pin.method_fallback', badgeTone: 'advanced' },
]

const permissionPresets = [
  {
    key: 'viewer',
    labelKey: 'pin.viewer_access',
    activeClass: 'border-blue-400/40 bg-blue-400/10 text-blue-200',
  },
  {
    key: 'standard',
    labelKey: 'pin.standard_access',
    activeClass: 'border-ice/40 bg-ice/10 text-ice',
  },
  {
    key: 'full',
    labelKey: 'pin.full_control',
    activeClass: 'border-red-400/40 bg-red-400/10 text-red-200',
  },
]

let resetOTPTimeout = null
let qrContainer = null
let qrCode = null
let qrCodeLoader = null
let hostInfoCache = null
let hostManuallySet = false
let currentEditingClient = null
let lastFocusedElement = null

const ensureQRCode = async () => {
  if (window.QRCode) {
    return window.QRCode
  }

  if (!qrCodeLoader) {
    qrCodeLoader = new Promise((resolve, reject) => {
      const script = document.createElement('script')
      script.src = `${import.meta.env.BASE_URL}assets/js/qrcode.min.js`
      script.async = true
      script.dataset.qrcodeLoader = 'true'
      script.onload = () => {
        if (window.QRCode) {
          resolve(window.QRCode)
        } else {
          qrCodeLoader = null
          reject(new Error('QRCode global not found after script load'))
        }
      }
      script.onerror = () => {
        qrCodeLoader = null
        reject(new Error('Failed to load QRCode script'))
      }
      document.body.appendChild(script)
    })
  }

  return qrCodeLoader
}

const initQR = async () => {
  if (!qrContainer) {
    qrContainer = document.createElement('div')
    qrContainer.className = 'mb-2 rounded bg-white p-2'
  }

  if (!qrCode) {
    const QRCodeCtor = await ensureQRCode()
    qrCode = new QRCodeCtor(qrContainer)
  }
}

const updateQR = async (url) => {
  await initQR()
  if (!qrCode) return
  qrCode.clear()
  qrCode.makeCode(url)
  const refContainer = document.querySelector('#qrRef')
  if (refContainer) refContainer.appendChild(qrContainer)
}

const saveHostCacheFn = (addr, port, manual) => {
  hostInfoCache = { hostAddr: addr, hostPort: port }
  if (manual) {
    sessionStorage.setItem('hostInfo', JSON.stringify(hostInfoCache))
    hostManuallySet = true
  }
}

const cmdTpl = { cmd: '', elevated: 'false' }

const aiSuggestion = ref(null)
const aiSuggestionFor = ref(null)

async function fetchAiSuggestion(client) {
  const name = client.editName || client.name
  if (!name) {
    showToast('Device needs a name for AI suggestions', 'error')
    return
  }
  aiSuggestionFor.value = name
  aiSuggestion.value = null

  const suggestion = await getAiSuggestion(name)
  if (suggestion && suggestion.status) {
    aiSuggestion.value = suggestion
  } else {
    const result = await aiOptimize(name)
    if (result && result.status) {
      aiSuggestion.value = result
    } else {
      showToast('No suggestion available — device not recognized', 'info')
      aiSuggestionFor.value = null
    }
  }
}

function applySuggestion(client) {
  if (!aiSuggestion.value) return
  const suggestion = aiSuggestion.value
  if (suggestion.display_mode) client.editDisplayMode = suggestion.display_mode
  if (suggestion.color_range != null) client.editProfile.color_range = suggestion.color_range
  if (suggestion.hdr != null) client.editProfile.hdr = suggestion.hdr
  showToast('AI suggestion applied to profile', 'success')
  aiSuggestion.value = null
  aiSuggestionFor.value = null
}

function dismissSuggestion() {
  aiSuggestion.value = null
  aiSuggestionFor.value = null
}

const editingHost = ref(false)
const currentTab = ref('OTP')
const otp = ref('')
const passphrase = ref('')
const otpMessage = ref('')
const otpStatus = ref('warning')
const deviceName = ref('')
const hostAddr = ref('')
const hostPort = ref('')
const hostName = ref('')
const showApplyMessage = ref(false)
const unpairAllPressed = ref(false)
const unpairAllStatus = ref(null)
const tofuSubnets = ref([])
const pinCode = ref('')
const pinDeviceName = ref('')
const pinMessage = ref('')
const pinStatus = ref('error')

function resetState() {
  editingHost.value = false
  otp.value = ''
  passphrase.value = ''
  otpMessage.value = ''
  otpStatus.value = 'warning'
  deviceName.value = ''
  hostAddr.value = ''
  hostPort.value = ''
  hostName.value = ''
  pinCode.value = ''
  pinDeviceName.value = ''
  pinMessage.value = ''
  pinStatus.value = 'error'
  showApplyMessage.value = false
  unpairAllPressed.value = false
  unpairAllStatus.value = null
}

const deepLink = computed(() => {
  return encodeURI(`art://${hostAddr.value}:${hostPort.value}?pin=${otp.value}&passphrase=${passphrase.value}&name=${hostName.value}`)
})

const canSaveHost = computed(() => {
  return !!(hostAddr.value && hostPort.value)
})

const pairedCount = computed(() => clients.value.length)
const connectedCount = computed(() => clients.value.filter((client) => client.connected).length)
const activePairingMethod = computed(() =>
  pairingMethods.find((method) => method.key === currentTab.value) || pairingMethods[0]
)
const activePairingMethodLabel = computed(() => i18n.t(activePairingMethod.value.titleKey))
const activePairingSummary = computed(() => {
  if (currentTab.value === 'TOFU') {
    return 'Fast on a trusted LAN, but only appropriate when you fully control the network boundary.'
  }
  if (currentTab.value === 'PIN') {
    return 'Use the client PIN when you need a direct, explicit approval path.'
  }
  return 'Best default for Nova: generate one passphrase, then scan the QR code or open the deep link.'
})

function switchTab(tab) {
  resetState()
  currentTab.value = tab
  hostInfoCache = null
  clearTimeout(resetOTPTimeout)
}

function editHost() {
  editingHost.value = !editingHost.value
  if (hostInfoCache) {
    hostAddr.value = hostInfoCache.hostAddr
    hostPort.value = hostInfoCache.hostPort
  }
}

async function saveHost() {
  if (!canSaveHost.value) return
  await nextTick()
  await updateQR(deepLink.value)
  editingHost.value = false
  saveHostCacheFn(hostAddr.value, hostPort.value, true)
}

function registerDevice() {
  pinMessage.value = ''
  fetch('./api/pin', {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify({ pin: pinCode.value, name: pinDeviceName.value })
  })
    .then((response) => response.json())
    .then((response) => {
      if (response.status === true) {
        pinStatus.value = 'success'
        pinMessage.value = i18n.t('pin.pair_success')
        pinCode.value = ''
        pinDeviceName.value = ''
        setTimeout(() => refreshClients(), 1000)
        showToast(i18n.t('pin.pair_success_check_perm'), 'success')
      } else {
        pinStatus.value = 'error'
        pinMessage.value = i18n.t('pin.pair_failure')
      }
    })
    .catch(() => {
      pinStatus.value = 'error'
      pinMessage.value = i18n.t('pin.pair_failure')
    })
}

async function requestOTP() {
  if (editingHost.value) return
  fetch('./api/otp', {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify({ passphrase: passphrase.value, deviceName: deviceName.value })
  })
    .then(resp => resp.json())
    .then(async resp => {
      if (!resp.status) {
        otpMessage.value = resp.message
        otpStatus.value = 'danger'
        return
      }
      otp.value = resp.otp
      hostName.value = resp.name
      otpStatus.value = 'success'
      otpMessage.value = i18n.t('pin.otp_success')

      const isLocalHost = ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname)

      if (hostManuallySet && hostInfoCache) {
        hostAddr.value = hostInfoCache.hostAddr
        hostPort.value = hostInfoCache.hostPort
      } else {
        hostAddr.value = resp.ip
        hostPort.value = parseInt(location.port, 10) - 1
        if (!isLocalHost) {
          hostAddr.value = location.hostname
        }
        saveHostCacheFn(hostAddr.value, hostPort.value)
      }

      if (hostAddr.value) {
        await nextTick()
        await updateQR(deepLink.value)
        if (resetOTPTimeout !== null) clearTimeout(resetOTPTimeout)
        resetOTPTimeout = setTimeout(() => {
          resetState()
          otp.value = i18n.t('pin.otp_expired')
          otpMessage.value = i18n.t('pin.otp_expired_msg')
          resetOTPTimeout = null
        }, 3 * 60 * 1000)
        if (!isLocalHost) {
          setTimeout(() => {
            if (window.confirm(i18n.t('pin.otp_pair_now'))) {
              window.open(deepLink.value)
            }
          }, 0)
        }
      }
    })
    .catch(() => {
      otpStatus.value = 'danger'
      otpMessage.value = i18n.t('pin.otp_request_failed')
    })
}

function clickedApplyBanner() {
  showApplyMessage.value = false
}

function addCmd(arr, idx) {
  const newCmd = Object.assign({}, cmdTpl)
  if (idx < 0) {
    arr.push(newCmd)
  } else {
    arr.splice(idx + 1, 0, newCmd)
  }
}

function removeCmd(arr, idx) {
  arr.splice(idx, 1)
}

function permissionPresetKey(perm) {
  if ((perm & permissionMapping._all) === permissionMapping._all) return 'full'
  if (perm === permissionMapping._default) return 'standard'
  if (perm === permissionMapping.view || perm === permissionMapping.list || perm === permissionMapping._allow_view) return 'viewer'
  return 'custom'
}

function accessPresetLabel(perm) {
  switch (permissionPresetKey(perm)) {
    case 'full':
      return i18n.t('pin.full_control')
    case 'standard':
      return i18n.t('pin.standard_access')
    case 'viewer':
      return i18n.t('pin.viewer_access')
    default:
      return i18n.t('pin.custom_access')
  }
}

function accessToneClass(perm) {
  switch (permissionPresetKey(perm)) {
    case 'full':
      return 'border-red-400/30 bg-red-400/10 text-red-200'
    case 'standard':
      return 'border-ice/30 bg-ice/10 text-ice'
    case 'viewer':
      return 'border-blue-400/30 bg-blue-400/10 text-blue-200'
    default:
      return 'border-storm/30 bg-deep/60 text-storm'
  }
}

function applyPermissionPreset(client, preset) {
  switch (preset) {
    case 'viewer':
      client.editPerm = permissionMapping.view
      break
    case 'standard':
      client.editPerm = permissionMapping._default
      break
    case 'full':
      client.editPerm = permissionMapping._all
      break
  }
}

function colorRangeLabel(colorRange) {
  if (colorRange === 2) return i18n.t('pin.color_full')
  if (colorRange === 1) return i18n.t('pin.color_limited')
  return i18n.t('pin.color_client_default')
}

function clientDisplaySummary(client) {
  const profile = profiles.value[client.name] || {}
  const parts = []
  if (client.display_mode) parts.push(client.display_mode)
  if (profile.output_name) parts.push(profile.output_name)
  if (profile.hdr) parts.push('HDR')
  if (profile.color_range === 1 || profile.color_range === 2) parts.push(colorRangeLabel(profile.color_range))
  return parts.length > 0 ? parts.join(' · ') : i18n.t('pin.display_summary_auto')
}

function clientCommandSummary(client) {
  return client.allow_client_commands ? i18n.t('pin.commands_enabled') : i18n.t('pin.commands_disabled')
}

function editClient(client) {
  if (currentEditingClient) {
    cancelEdit(currentEditingClient)
  }
  lastFocusedElement = document.activeElement
  client.editing = true
  client.editPerm = client.perm
  client.editName = client.name
  client.editAllowClientCommands = client.allow_client_commands
  client.editEnableLegacyOrdering = client.enable_legacy_ordering
  client.editAlwaysUseVirtualDisplay = client.always_use_virtual_display
  client.editDisplayMode = client.display_mode
  client.edit_do = JSON.parse(JSON.stringify(client.do || []))
  client.edit_undo = JSON.parse(JSON.stringify(client.undo || []))
  const profile = profiles.value[client.name] || {}
  client.editProfile = {
    output_name: profile.output_name || '',
    color_range: profile.color_range || 0,
    hdr: profile.hdr || false,
    mac_address: profile.mac_address || ''
  }
  currentEditingClient = client
  nextTick(() => {
    document.getElementById(`client-name-${client.uuid}`)?.focus()
  })
}

function cancelEdit(client) {
  currentEditingClient = null
  client.editing = false
  client.editPerm = client.perm
  client.editName = client.name
  client.editDisplayMode = client.display_mode
  client.editAllowClientCommands = client.allow_client_commands
  client.editEnableLegacyOrdering = client.enable_legacy_ordering
  client.editAlwaysUseVirtualDisplay = client.always_use_virtual_display
  dismissSuggestion()
  nextTick(() => {
    lastFocusedElement?.focus?.()
  })
}

function saveClient(client) {
  client.editing = false
  currentEditingClient = null
  const profileData = client.editProfile
  saveClientAPI(client)
    .catch(err => showToast(i18n.t('pin.save_client_error') + err, 'error'))
  if (profileData) {
    saveProfile(client.editName || client.name, profileData)
  }
  nextTick(() => {
    lastFocusedElement?.focus?.()
  })
}

function togglePermission(client, permission) {
  client.editPerm ^= permissionMapping[permission]
}

function validateModeOverride(event) {
  const value = event.target.value.trim()
  if (value && !value.match(/^\d+x\d+x\d+(\.\d+)?$/)) {
    event.target.setCustomValidity(i18n.t('pin.display_mode_override_error'))
  } else {
    event.target.setCustomValidity('')
  }
  event.target.reportValidity()
}

function sendWol(client) {
  sendWolAPI(client)
    .then(resp => {
      if (resp?.status) showToast(i18n.t('pin.wol_success'), 'success')
      else showToast(i18n.t('pin.wol_error') + (resp?.error ? ` ${resp.error}` : ''), 'error')
    })
    .catch(() => showToast(i18n.t('pin.wol_error'), 'error'))
}

function unpairAll() {
  if (!window.confirm(i18n.t('pin.unpair_all_confirm'))) return
  unpairAllPressed.value = true
  unpairAllAPI()
    .then(() => {
      unpairAllPressed.value = false
      unpairAllStatus.value = true
      setTimeout(() => { unpairAllStatus.value = null }, 5000)
    })
    .catch(() => {
      unpairAllPressed.value = false
      unpairAllStatus.value = false
      setTimeout(() => { unpairAllStatus.value = null }, 5000)
    })
}

function handleUnpairSingle(uuid) {
  if (!window.confirm(i18n.t('pin.unpair_single_confirm'))) return
  unpairSingle(uuid)
    .then(() => { showApplyMessage.value = true })
    .catch(() => showToast(i18n.t('pin.unpair_all_error'), 'error'))
}

hostInfoCache = JSON.parse(sessionStorage.getItem('hostInfo'))
hostManuallySet = false
if (hostInfoCache) hostManuallySet = true
refreshClients()
refreshProfiles()

fetch('./api/config', { credentials: 'include' })
  .then(r => r.json())
  .then(cfg => {
    if (typeof cfg.trusted_subnets === 'string' && cfg.trusted_subnets) {
      try {
        const parsed = JSON.parse(cfg.trusted_subnets)
        tofuSubnets.value = Array.isArray(parsed) ? parsed : cfg.trusted_subnets.split(/[\n,]+/).map(s => s.trim()).filter(Boolean)
      } catch {
        tofuSubnets.value = cfg.trusted_subnets.split(/[\n,]+/).map(s => s.trim()).filter(Boolean)
      }
    }
  })
  .catch(() => {})
</script>
