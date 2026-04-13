<template>
  <div class="flex flex-col items-center pt-6">
    <!-- Tab pills -->
    <div class="flex gap-2 mb-6">
      <button
        class="px-4 py-2 rounded-lg text-sm font-medium transition-colors"
        :class="currentTab === '#TOFU' ? 'bg-twilight text-ice' : 'text-storm hover:bg-twilight/50'"
        @click="switchTab('TOFU')"
      >TOFU</button>
      <button
        class="px-4 py-2 rounded-lg text-sm font-medium transition-colors"
        :class="currentTab === '#OTP' ? 'bg-twilight text-ice' : 'text-storm hover:bg-twilight/50'"
        @click="switchTab('OTP')"
      >
        <span class="flex items-center gap-1.5">
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v1m6 11h2m-6 0h-2v4m0-11v3m0 0h.01M12 12h4.01M16 20h4M4 12h4m12 0h.01M5 8h2a1 1 0 001-1V5a1 1 0 00-1-1H5a1 1 0 00-1 1v2a1 1 0 001 1zm12 0h2a1 1 0 001-1V5a1 1 0 00-1-1h-2a1 1 0 00-1 1v2a1 1 0 001 1zM5 20h2a1 1 0 001-1v-2a1 1 0 00-1-1H5a1 1 0 00-1 1v2a1 1 0 001 1z"/></svg>
          QR Code
        </span>
      </button>
      <button
        class="px-4 py-2 rounded-lg text-sm font-medium transition-colors"
        :class="currentTab === '#PIN' ? 'bg-twilight text-ice' : 'text-storm hover:bg-twilight/50'"
        @click="switchTab('PIN')"
      >{{ $t('pin.pin_pairing') }}</button>
    </div>

    <!-- TOFU pairing info -->
    <div v-if="currentTab === '#TOFU'" class="flex flex-col items-center w-full max-w-md">
      <div class="card flex flex-col p-6 mb-4 w-full">
        <h3 class="text-lg font-semibold text-silver mb-2">Trust on First Use</h3>
        <p class="text-storm text-sm mb-4">
          Clients on trusted subnets can pair automatically without entering a PIN.
          The client sends a pairing request and Polaris auto-approves it.
        </p>

        <div class="mb-4">
          <label class="block text-sm font-medium text-storm mb-2">Trusted Subnets</label>
          <div v-if="tofuSubnets.length > 0" class="space-y-2 mb-2">
            <div v-for="(subnet, i) in tofuSubnets" :key="i" class="flex items-center gap-2">
              <code class="flex-1 bg-deep border border-storm rounded-lg px-3 py-2 text-silver font-mono text-sm">{{ subnet }}</code>
            </div>
          </div>
          <div v-else class="bg-deep border border-storm rounded-lg px-3 py-3 text-storm text-sm mb-2">
            No trusted subnets configured. TOFU pairing is disabled.
          </div>
        </div>

        <a href="#/config" class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200 self-start">
          Configure in Network Settings
        </a>
      </div>

      <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-3 rounded-lg mb-3 w-full">
        <b>How it works:</b> In the Nova app, tap your server → "Pair (Accept on Server)". If your device is on a trusted subnet, pairing completes automatically.
      </div>
    </div>

    <!-- PIN pairing form -->
    <form v-if="currentTab === '#PIN'" class="flex flex-col items-center" id="form" @submit.prevent="registerDevice">
      <div class="card flex flex-col p-6 mb-4 w-full max-w-md">
        <input type="text" pattern="\d*" :placeholder="`${$t('navbar.pin')}`" autofocus id="pin-input" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none mt-2" required />
        <input type="text" :placeholder="`${$t('pin.device_name')}`" id="name-input" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none my-4" />
        <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200">{{ $t('pin.send') }}</button>
      </div>
      <div id="status"></div>
    </form>

    <!-- QR Code / OTP pairing form -->
    <form v-if="currentTab === '#OTP'" class="flex flex-col items-center" @submit.prevent="requestOTP">
      <div class="card flex flex-col p-6 mb-4 w-full max-w-md">
        <!-- QR code display (shown after generation) -->
        <div v-if="otp && hostAddr" class="flex flex-col items-center mb-4">
          <div class="text-[10px] font-semibold text-storm uppercase tracking-wider mb-2">Scan with Nova</div>
          <div id="qrRef" class="flex justify-center"></div>
          <p class="text-center text-storm text-xs mt-2">
            <a class="text-storm hover:text-silver" :href="deepLink">art://{{ hostAddr }}:{{ hostPort }}</a>
            <button class="ml-1 text-storm hover:text-ice" @click="editHost">
              <svg class="w-3.5 h-3.5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z"/></svg>
            </button>
          </p>
          <div class="text-xs text-storm mt-1">or enter PIN manually:</div>
          <h1 class="text-3xl font-bold text-ice font-mono tracking-[0.3em]">{{ otp }}</h1>
        </div>

        <!-- Prompt (shown before generation) -->
        <div v-if="!otp" class="text-center mb-4">
          <div class="text-sm text-silver mb-1">Generate a QR code for Nova to scan</div>
          <div class="text-xs text-storm">Set a passphrase that both server and client share</div>
        </div>

        <!-- Host editing (rare) -->
        <div v-if="editingHost" class="flex flex-col gap-3">
          <input type="text" placeholder="HOST" v-model="hostAddr" autofocus class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" />
          <input type="text" placeholder="PORT" v-model="hostPort" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" />
          <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200 disabled:opacity-50" :disabled="!canSaveHost" @click.prevent="saveHost">{{ $t('_common.save') }}</button>
        </div>
        <div v-else class="flex flex-col gap-3">
          <input type="text" pattern="[0-9a-zA-Z]{4,}" :placeholder="`${$t('pin.otp_passphrase')}`" v-model="passphrase" required autofocus class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" />
          <input type="text" :placeholder="`${$t('pin.device_name')}`" v-model="deviceName" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" />
          <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v1m6 11h2m-6 0h-2v4m0-11v3m0 0h.01M12 12h4.01M16 20h4M4 12h4m12 0h.01M5 8h2a1 1 0 001-1V5a1 1 0 00-1-1H5a1 1 0 00-1 1v2a1 1 0 001 1zm12 0h2a1 1 0 001-1V5a1 1 0 00-1-1h-2a1 1 0 00-1 1v2a1 1 0 001 1zM5 20h2a1 1 0 001-1v-2a1 1 0 00-1-1H5a1 1 0 00-1 1v2a1 1 0 001 1z"/></svg>
            {{ otp ? 'Regenerate QR' : 'Generate QR Code' }}
          </button>
        </div>
      </div>
      <div v-if="otpMessage" class="border-l-4 p-3 rounded-lg mb-3 text-silver w-full max-w-md" :class="otpStatus === 'success' ? 'bg-twilight/50 border-green-500' : otpStatus === 'danger' ? 'bg-twilight/50 border-red-500' : 'bg-twilight/50 border-yellow-500'">{{ otpMessage }}</div>
      <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-3 rounded-lg mb-3 text-sm w-full max-w-md">
        <b>How it works:</b> Generate a QR code here, then in Nova tap your server → "Pair via QR Code" and scan it. The code expires in 3 minutes.
      </div>
    </form>

    <div class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-3 rounded-lg mb-4 w-full max-w-2xl">
      <b>{{ $t('_common.warning') }}</b> {{ $t('pin.warning_msg') }}
    </div>

    <!-- Manage Clients -->
    <div v-if="!clients && !clientsLoaded" class="my-6 self-stretch space-y-3">
      <Skeleton type="card" />
      <Skeleton type="card" />
    </div>
    <div class="card my-6 self-stretch" v-if="clientsLoaded">
      <div class="p-6">
        <div class="flex justify-between items-center">
          <h2 id="unpair" class="text-xl font-semibold text-silver">{{ $t('pin.device_management') }}</h2>
          <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-red-500 text-white hover:bg-red-600 hover:shadow-[0_0_24px_rgba(239,68,68,0.2)] transition-all duration-200 disabled:opacity-50 disabled:cursor-not-allowed" :disabled="unpairAllPressed" @click="unpairAll">
            {{ $t('pin.unpair_all') }}
          </button>
        </div>
        <p class="text-storm mt-3">{{ $t('pin.device_management_desc') }}</p>
        <p class="text-storm">{{ $t('pin.device_management_warning') }} <a href="https://github.com/papi/Polaris/wiki/Permission-System" target="_blank" class="text-ice hover:text-ice/80">{{ $t('_common.learn_more') }}</a></p>

        <div class="flex items-center gap-3 bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg mt-3" v-if="showApplyMessage">
          <div><b>{{ $t('_common.success') }}</b> {{ $t('pin.unpair_single_success') }}</div>
          <button class="ml-auto bg-ice/20 text-ice px-3 py-1 rounded-lg hover:bg-ice/30 transition text-sm" @click="clickedApplyBanner">{{ $t('_common.dismiss') }}</button>
        </div>
        <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg mt-3" v-if="unpairAllStatus === true">
          {{ $t('pin.unpair_all_success') }}
        </div>
        <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg mt-3" v-if="unpairAllStatus === false">
          {{ $t('pin.unpair_all_error') }}
        </div>
      </div>

      <!-- Client list -->
      <div v-if="clients && clients.length > 0" class="border-t border-storm">
        <template v-for="client in clients">
          <!-- Editing mode -->
          <div v-if="client.editing" class="border-b border-storm p-4">
            <div class="flex items-center gap-3 mb-3">
              <div class="flex-1 flex items-center gap-2">
                <span class="px-2 py-0.5 rounded text-xs font-mono" :class="client.editPerm >= 0x04000000 ? 'bg-red-500/20 text-red-400' : 'bg-ice/20 text-ice'">
                  [ {{permToStr(client.editPerm)}} ]
                </span>
                <input v-model="client.editName" @keyup.enter="saveClient(client)" class="flex-1 bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" type="text" :placeholder="$t('pin.device_name')">
              </div>
              <button class="bg-ice/20 text-ice px-3 py-2 rounded-lg hover:bg-ice/30 transition" @click="saveClient(client)">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 13l4 4L19 7"/></svg>
              </button>
              <button class="bg-storm/30 text-storm px-3 py-2 rounded-lg hover:bg-storm/50 transition" @click="cancelEdit(client)">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
              </button>
            </div>
            <div class="flex flex-wrap gap-4 justify-center">
              <div v-for="group in permissionGroups" class="flex flex-col gap-1">
                <div class="text-sm text-storm font-medium">{{ group.name }}:</div>
                <button v-for="perm in group.permissions" class="px-2 py-1 text-xs rounded transition" :disabled="isSuppressed(client.editPerm, perm.name, perm.suppressed_by)" :class="(isSuppressed(client.editPerm, perm.name, perm.suppressed_by) || checkPermission(client.editPerm, perm.name)) ? 'bg-green-600/20 text-green-400 border border-green-600' : 'bg-deep text-storm border border-storm hover:border-ice'" @click="togglePermission(client, perm.name)">
                  {{ $t(`permissions.${perm.name}`) }}
                </button>
              </div>
            </div>

            <!-- Enable legacy ordering -->
            <Checkbox class="mb-3 mt-4"
                      id="enable_legacy_ordering"
                      label="pin.enable_legacy_ordering"
                      desc="pin.enable_legacy_ordering_desc"
                      v-model="client.editEnableLegacyOrdering"
                      default="true"
            ></Checkbox>

            <!-- Always Use Virtual Display -->
            <Checkbox class="mb-3"
                      id="always_use_virtual_display"
                      label="pin.always_use_virtual_display"
                      desc="pin.always_use_virtual_display_desc"
                      v-model="client.editAlwaysUseVirtualDisplay"
                      default="false"
                      v-if="platform === 'windows'"
            ></Checkbox>
            <!-- Display Mode Override -->
            <div class="mb-3 mt-2">
              <label for="display_mode_override" class="block text-sm font-medium text-storm mb-1">{{ $t('pin.display_mode_override') }}</label>
              <input
                type="text"
                class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none"
                id="display_mode_override"
                v-model="client.editDisplayMode"
                placeholder="1920x1080x59.94"
                @input="validateModeOverride"
              />
              <div class="text-sm text-storm mt-1">{{ $t('pin.display_mode_override_desc') }} <a href="https://github.com/papi/Polaris/wiki/Display-Mode-Override" target="_blank" class="text-ice hover:text-ice/80">{{ $t('_common.learn_more') }}</a></div>
            </div>

            <!-- Display Profile -->
            <div class="mb-3 mt-4 p-3 bg-deep rounded-lg border border-storm/50">
              <div class="flex items-center justify-between mb-3">
                <div class="text-xs font-medium text-storm uppercase tracking-wider">Display Profile</div>
                <button @click="fetchAiSuggestion(client)" :disabled="aiLoading"
                        class="inline-flex items-center gap-1.5 h-7 px-2.5 text-xs font-medium rounded-lg border border-purple-500/30 text-purple-400 hover:bg-purple-500/10 transition-all duration-200 disabled:opacity-50">
                  <svg v-if="aiLoading && aiSuggestionFor === (client.editName || client.name)" class="w-3 h-3 animate-spin" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"/><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"/></svg>
                  <svg v-else class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9.663 17h4.673M12 3v1m6.364 1.636l-.707.707M21 12h-1M4 12H3m3.343-5.657l-.707-.707m2.828 9.9a5 5 0 117.072 0l-.548.547A3.374 3.374 0 0014 18.469V19a2 2 0 11-4 0v-.531c0-.895-.356-1.754-.988-2.386l-.548-.547z"/></svg>
                  AI Suggest
                </button>
              </div>
              <!-- AI Suggestion Card -->
              <div v-if="aiSuggestion && aiSuggestionFor === (client.editName || client.name)"
                   class="mb-3 p-3 rounded-lg border border-purple-500/20 bg-purple-500/5 animate-[fade-in_0.3s_ease-out]">
                <div class="flex items-center justify-between mb-2">
                  <div class="flex items-center gap-2">
                    <svg class="w-4 h-4 text-purple-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9.663 17h4.673M12 3v1m6.364 1.636l-.707.707M21 12h-1M4 12H3m3.343-5.657l-.707-.707m2.828 9.9a5 5 0 117.072 0l-.548.547A3.374 3.374 0 0014 18.469V19a2 2 0 11-4 0v-.531c0-.895-.356-1.754-.988-2.386l-.548-.547z"/></svg>
                    <span class="text-sm font-medium text-purple-300">AI Suggestion</span>
                    <span class="text-xs text-storm">{{ aiSuggestion.source }}</span>
                  </div>
                  <button @click="dismissSuggestion" class="text-storm hover:text-silver transition-colors">
                    <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
                  </button>
                </div>
                <div class="flex flex-wrap gap-x-4 gap-y-1 text-sm mb-2">
                  <span v-if="aiSuggestion.display_mode" class="text-silver/60">Mode <span class="text-silver font-medium">{{ aiSuggestion.display_mode }}</span></span>
                  <span v-if="aiSuggestion.target_bitrate_kbps" class="text-silver/60">Bitrate <span class="text-silver font-medium">{{ (aiSuggestion.target_bitrate_kbps / 1000).toFixed(0) }} Mbps</span></span>
                  <span v-if="aiSuggestion.color_range != null" class="text-silver/60">Color <span class="text-silver font-medium">{{ aiSuggestion.color_range === 2 ? 'Full' : aiSuggestion.color_range === 1 ? 'Limited' : 'Default' }}</span></span>
                  <span v-if="aiSuggestion.hdr != null" class="text-silver/60">HDR <span class="text-silver font-medium">{{ aiSuggestion.hdr ? 'On' : 'Off' }}</span></span>
                </div>
                <div v-if="aiSuggestion.reasoning" class="text-xs text-silver/60 italic mb-2">{{ aiSuggestion.reasoning }}</div>
                <button @click="applySuggestion(client)"
                        class="inline-flex items-center gap-1.5 h-7 px-3 text-xs font-medium rounded-lg bg-purple-500 text-white hover:bg-purple-600 transition-all duration-200">
                  Apply
                </button>
              </div>
              <div class="grid grid-cols-1 md:grid-cols-2 gap-3">
                <div>
                  <label class="block text-xs text-storm mb-1">Output Name</label>
                  <input type="text" class="w-full bg-void/50 border border-storm/50 rounded-lg px-2.5 py-1.5 text-sm text-silver focus:border-ice focus:outline-none"
                         v-model="client.editProfile.output_name" placeholder="e.g. HDMI-A-1" />
                </div>
                <div>
                  <label class="block text-xs text-storm mb-1">Color Range</label>
                  <select v-model="client.editProfile.color_range" class="w-full bg-void/50 border border-storm/50 rounded-lg px-2.5 py-1.5 text-sm text-silver focus:border-ice focus:outline-none">
                    <option :value="0">Client Default</option>
                    <option :value="1">Limited (MPEG)</option>
                    <option :value="2">Full (JPEG)</option>
                  </select>
                </div>
                <div>
                  <label class="block text-xs text-storm mb-1">WoL MAC Address</label>
                  <input type="text" class="w-full bg-void/50 border border-storm/50 rounded-lg px-2.5 py-1.5 text-sm text-silver focus:border-ice focus:outline-none"
                         v-model="client.editProfile.mac_address" placeholder="AA:BB:CC:DD:EE:FF" />
                </div>
                <div class="flex items-center gap-2 pt-4">
                  <input type="checkbox" class="w-4 h-4 rounded border-storm bg-void text-ice accent-ice" v-model="client.editProfile.hdr" />
                  <label class="text-sm text-silver">Enable HDR</label>
                </div>
              </div>
            </div>

            <!-- Allow client commands -->
            <Checkbox class="mb-3"
                      id="allow_client_commands"
                      label="pin.allow_client_commands"
                      desc="pin.allow_client_commands_desc"
                      v-model="client.editAllowClientCommands"
                      default="true"
            ></Checkbox>

            <!-- connect/disconnect commands -->
            <div class="mb-3 mt-2 flex flex-col" v-for="cmdType in ['do', 'undo']" v-if="client.editAllowClientCommands">
              <label class="block text-sm font-medium text-storm mb-1">{{ $t(`pin.client_${cmdType}_cmd`) }}</label>
              <div class="text-sm text-storm">{{ $t(`pin.client_${cmdType}_cmd_desc`) }} <a href="https://github.com/papi/Polaris/wiki/Client-Commands" target="_blank" class="text-ice hover:text-ice/80">{{ $t('_common.learn_more') }}</a></div>
              <table class="mt-2 w-full text-left" v-if="client[`edit_${cmdType}`].length > 0">
                <thead>
                  <tr class="border-b border-storm">
                    <th class="py-2 text-storm text-sm" style="width: 80%">
                      <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 9l3 3-3 3m5 0h3M5 20h14a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z"/></svg>
                      {{ $t('_common.cmd_val') }}
                    </th>
                    <th class="py-2 text-storm text-sm" style="min-width: 10em; max-width: 12em;" v-if="platform === 'windows'">
                      {{ $t('_common.run_as') }}
                    </th>
                    <th class="py-2" style="min-width: 110px;"></th>
                  </tr>
                </thead>
                <tbody>
                <tr v-for="(c, i) in client[`edit_${cmdType}`]" class="border-b border-storm/50">
                  <td class="py-2 pr-2">
                    <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="c.cmd" />
                  </td>
                  <td v-if="platform === 'windows'" class="py-2 pr-2">
                    <div class="flex items-center gap-2">
                      <input type="checkbox" class="w-4 h-4 rounded border-storm bg-void text-ice" :id="`client-${cmdType}-cmd-admin-${i}`" v-model="c.elevated"/>
                      <label :for="`client-${cmdType}-cmd-admin-${i}`" class="text-sm text-storm">{{ $t('_common.elevated') }}</label>
                    </div>
                  </td>
                  <td class="py-2">
                    <div class="flex gap-1">
                      <button class="bg-red-500 text-white px-2 py-1 rounded hover:bg-red-600 transition" @click="removeCmd(client[`edit_${cmdType}`], i)">
                        <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
                      </button>
                      <button class="bg-ice/20 text-ice px-2 py-1 rounded hover:bg-ice/30 transition" @click="addCmd(client[`edit_${cmdType}`], i)">
                        <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
                      </button>
                    </div>
                  </td>
                </tr>
                </tbody>
              </table>
              <button class="mt-2 bg-ice/20 text-ice px-3 py-1 rounded-lg hover:bg-ice/30 transition text-sm self-center" @click="addCmd(client[`edit_${cmdType}`], -1)">
                + {{ $t('config.add') }}
              </button>
            </div>
          </div>
          <!-- Display mode -->
          <div v-else class="flex items-center border-b border-storm/50 px-4 py-3">
            <div class="flex-1 flex items-center gap-2">
              <span class="px-2 py-0.5 rounded text-xs font-mono" :class="client.perm >= 0x04000000 ? 'bg-red-500/20 text-red-400' : 'bg-ice/20 text-ice'">
                [ {{permToStr(client.perm)}} ]
              </span>
              <span class="text-silver">{{client.name != "" ? client.name : $t('pin.unpair_single_unknown')}}</span>
            </div>
            <div class="flex gap-2">
              <button v-if="client.name" class="bg-green-600/20 text-green-400 px-2 py-1 rounded hover:bg-green-600/30 transition" :disabled="client.wolSending" :title="$t('pin.wol_send')" @click="sendWol(client)">
                <svg v-if="!client.wolSending" class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"/></svg>
                <svg v-else class="w-4 h-4 animate-spin" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15"/></svg>
              </button>
              <button v-if="client.connected" class="bg-yellow-600 text-white px-2 py-1 rounded hover:bg-yellow-700 transition" @click="disconnectClient(client.uuid)">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13.828 10.172a4 4 0 00-5.656 0l-4 4a4 4 0 105.656 5.656l1.102-1.101m-.758-4.899a4 4 0 005.656 0l4-4a4 4 0 00-5.656-5.656l-1.1 1.1"/></svg>
              </button>
              <button class="bg-ice/20 text-ice px-2 py-1 rounded hover:bg-ice/30 transition" @click="editClient(client)">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z"/></svg>
              </button>
              <button class="bg-red-500/20 text-red-400 px-2 py-1 rounded hover:bg-red-500/30 transition" @click="handleUnpairSingle(client.uuid)">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
              </button>
            </div>
          </div>
        </template>
      </div>
      <div v-else class="border-t border-storm p-4 text-center">
        <em class="text-storm">{{ $t('pin.unpair_single_no_devices') }}</em>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, inject, nextTick } from 'vue'
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

let resetOTPTimeout = null
let qrContainer = null
let qrCode = null
let qrCodeLoader = null
let hostInfoCache = null
let hostManuallySet = false
let currentEditingClient = null

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
    qrContainer.className = "mb-2 p-2 bg-white rounded"
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

// AI suggestion state per client
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
  // Try device DB first, then AI
  const suggestion = await getAiSuggestion(name)
  if (suggestion && suggestion.status) {
    aiSuggestion.value = suggestion
  } else {
    // Fall back to live AI call
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
  const s = aiSuggestion.value
  if (s.display_mode) client.editDisplayMode = s.display_mode
  if (s.color_range != null) client.editProfile.color_range = s.color_range
  if (s.hdr != null) client.editProfile.hdr = s.hdr
  showToast('AI suggestion applied to profile', 'success')
  aiSuggestion.value = null
  aiSuggestionFor.value = null
}

function dismissSuggestion() {
  aiSuggestion.value = null
  aiSuggestionFor.value = null
}

// Reactive state
const editingHost = ref(false)
const currentTab = ref('#OTP')
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
  showApplyMessage.value = false
  unpairAllPressed.value = false
  unpairAllStatus.value = null
}

// Computed
const deepLink = computed(() => {
  return encodeURI(`art://${hostAddr.value}:${hostPort.value}?pin=${otp.value}&passphrase=${passphrase.value}&name=${hostName.value}`)
})

const canSaveHost = computed(() => {
  return !!(hostAddr.value && hostPort.value)
})

// Methods
function switchTab(tab) {
  resetState()
  currentTab.value = '#' + tab
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

function registerDevice(e) {
  let pin = document.querySelector("#pin-input").value
  let name = document.querySelector("#name-input").value
  document.querySelector("#status").innerHTML = ""
  let b = JSON.stringify({ pin: pin, name: name })
  fetch("./api/pin", {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: b
  })
    .then((response) => response.json())
    .then((response) => {
      if (response.status === true) {
        document.querySelector("#status").innerHTML = `<div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg">${i18n.t('pin.pair_success')}</div>`
        document.querySelector("#pin-input").value = ""
        document.querySelector("#name-input").value = ""
        setTimeout(() => refreshClients(), 1000)
        showToast(i18n.t('pin.pair_success_check_perm'), 'success')
      } else {
        document.querySelector("#status").innerHTML = `<div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg">${i18n.t('pin.pair_failure')}</div>`
      }
    })
}

async function requestOTP() {
  if (editingHost.value) return
  fetch("./api/otp", {
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

    const isLocalHost = ['localhost', '127.0.0.1', '[::1]'].indexOf(location.hostname) >= 0

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

function editClient(client) {
  if (currentEditingClient) {
    cancelEdit(currentEditingClient)
  }
  client.editing = true
  client.editPerm = client.perm
  client.editName = client.name
  client.editAllowClientCommands = client.allow_client_commands
  client.editEnableLegacyOrdering = client.enable_legacy_ordering
  client.editAlwaysUseVirtualDisplay = client.always_use_virtual_display
  client.editDisplayMode = client.display_mode
  client.edit_do = JSON.parse(JSON.stringify(client.do || []))
  client.edit_undo = JSON.parse(JSON.stringify(client.undo || []))
  // Load display profile for this client
  const p = profiles.value[client.name] || {}
  client.editProfile = {
    output_name: p.output_name || '',
    color_range: p.color_range || 0,
    hdr: p.hdr || false,
    mac_address: p.mac_address || ''
  }
  currentEditingClient = client
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
}

function saveClient(client) {
  client.editing = false
  currentEditingClient = null
  // Save client settings + display profile in parallel
  const profileData = client.editProfile
  saveClientAPI(client)
    .catch(err => showToast(i18n.t('pin.save_client_error') + err, 'error'))
  if (profileData) {
    saveProfile(client.editName || client.name, profileData)
  }
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
      else showToast(i18n.t('pin.wol_error') + (resp?.error ? ' ' + resp.error : ''), 'error')
    })
    .catch(() => showToast(i18n.t('pin.wol_error'), 'error'))
}

function unpairAll() {
  unpairAllPressed.value = true
  unpairAllAPI()
    .then(() => {
      unpairAllPressed.value = false
      unpairAllStatus.value = true
      setTimeout(() => { unpairAllStatus.value = null }, 5000)
    })
}

function handleUnpairSingle(uuid) {
  unpairSingle(uuid).then(() => { showApplyMessage.value = true })
}

// created() logic
hostInfoCache = JSON.parse(sessionStorage.getItem('hostInfo'))
hostManuallySet = false
if (hostInfoCache) hostManuallySet = true
refreshClients()
refreshProfiles()

// Fetch TOFU config
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
