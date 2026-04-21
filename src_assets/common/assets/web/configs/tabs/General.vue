<script setup>
import { ref } from 'vue'
import Checkbox from '../../Checkbox.vue'
import InfoHint from '../../components/InfoHint.vue'

const props = defineProps({
  platform: String,
  config: Object,
  globalPrepCmd: Array,
  globalStateCmd: Array,
  serverCmd: Array
})

const config = ref(props.config)
const globalPrepCmd = ref(props.globalPrepCmd)
const globalStateCmd = ref(props.globalStateCmd)
const serverCmd = ref(props.serverCmd)

const cmds = ref({
  prep: globalPrepCmd,
  state: globalStateCmd
})

const prepCmdTemplate = {
  do: "",
  undo: "",
}

const serverCmdTemplate = {
  name: "",
  cmd: ""
}

function addCmd(cmdArr, template, idx) {
  const _tpl = Object.assign({}, template);

  if (props.platform === 'windows') {
    _tpl.elevated = false;
  }
  if (idx < 0) {
    cmdArr.push(_tpl);
  } else {
    cmdArr.splice(idx, 0, _tpl);
  }
}

function removeCmd(cmdArr, index) {
  cmdArr.splice(index,1)
}
</script>

<template>
  <div id="general" class="config-page space-y-4">
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Identity</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Host identity</h3>
          <InfoHint size="sm" label="Host basics guidance">
            Set the visible Polaris identity, UI language, and log verbosity that shape day-to-day host administration.
          </InfoHint>
        </div>
      </div>
      <div class="settings-inline-stack">

    <!-- Locale -->
    <div>
      <div class="section-title-row mb-1">
        <label for="locale" class="block text-sm font-medium text-storm">{{ $t('config.locale') }}</label>
        <InfoHint size="sm" label="Locale guidance">
          Controls the language used by the Polaris web console.
        </InfoHint>
      </div>
      <select id="locale" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.locale">
        <option value="bg">Български (Bulgarian)</option>
        <option value="cs">Čeština (Czech)</option>
        <option value="de">Deutsch (German)</option>
        <option value="en">English</option>
        <option value="en_GB">English, UK</option>
        <option value="en_US">English, US</option>
        <option value="es">Español (Spanish)</option>
        <option value="fr">Français (French)</option>
        <option value="hu">Magyar (Hungarian)</option>
        <option value="it">Italiano (Italian)</option>
        <option value="ja">日本語 (Japanese)</option>
        <option value="ko">한국어 (Korean)</option>
        <option value="pl">Polski (Polish)</option>
        <option value="pt">Português (Portuguese)</option>
        <option value="pt_BR">Português, Brasileiro (Portuguese, Brazilian)</option>
        <option value="ru">Русский (Russian)</option>
        <option value="sv">svenska (Swedish)</option>
        <option value="tr">Türkçe (Turkish)</option>
        <option value="uk">Українська (Ukranian)</option>
        <option value="vi">Tiếng Việt (Vietnamese)</option>
        <option value="zh">简体中文 (Chinese Simplified)</option>
        <option value="zh_TW">繁體中文 (Chinese Traditional)</option>
      </select>
    </div>

    <!-- Polaris Name -->
    <div class="mb-3">
      <div class="section-title-row mb-1">
        <label for="sunshine_name" class="block text-sm font-medium text-storm">{{ $t('config.sunshine_name') }}</label>
        <InfoHint size="sm" label="Visible name guidance">
          Shown to clients and local discovery surfaces.
        </InfoHint>
      </div>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="sunshine_name" placeholder="Polaris"
             v-model="config.sunshine_name" />
    </div>

    <!-- Log Level -->
    <div class="mb-3">
      <div class="section-title-row mb-1">
        <label for="min_log_level" class="block text-sm font-medium text-storm">{{ $t('config.min_log_level') }}</label>
        <InfoHint size="sm" label="Log level guidance">
          Use quieter levels for routine operation and raise this only while debugging.
        </InfoHint>
      </div>
      <select id="min_log_level" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.min_log_level">
        <option value="0">{{ $t('config.min_log_level_0') }}</option>
        <option value="1">{{ $t('config.min_log_level_1') }}</option>
        <option value="2">{{ $t('config.min_log_level_2') }}</option>
        <option value="3">{{ $t('config.min_log_level_3') }}</option>
        <option value="4">{{ $t('config.min_log_level_4') }}</option>
        <option value="5">{{ $t('config.min_log_level_5') }}</option>
        <option value="6">{{ $t('config.min_log_level_6') }}</option>
      </select>
    </div>

      </div>
    </section>

    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Automation</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Automation</h3>
          <InfoHint size="sm" label="Lifecycle commands guidance">
            Run host-side commands before apps launch, when sessions resume, and when server shortcuts are triggered from clients.
          </InfoHint>
        </div>
      </div>
      <div class="settings-inline-stack">

    <!-- Global Prep/State Commands -->
    <div v-for="type in ['prep', 'state']" :id="`global_${type}_cmd`" class="d-flex flex-column">
      <div class="section-title-row mb-1">
        <label class="block text-sm font-medium text-storm">{{ $t(`config.global_${type}_cmd`) }}</label>
        <InfoHint size="sm" :label="`${type} command guidance`">
          <span v-if="type === 'prep'">Run commands before launch, then undo them after the session ends.</span>
          <span v-else>Run commands when Polaris changes session state on the host.</span>
        </InfoHint>
      </div>
      <table class="w-full text-left" v-if="cmds[type].length > 0">
        <thead>
        <tr>
          <th scope="col"><svg class="w-4 h-4 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z"/></svg> {{ $t('_common.do_cmd') }}</th>
          <th scope="col"><svg class="w-4 h-4 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 10h10a8 8 0 018 8v2M3 10l6 6m-6-6l6-6"/></svg> {{ $t('_common.undo_cmd') }}</th>
          <th scope="col" v-if="platform === 'windows'">
            <svg class="w-4 h-4 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12l2 2 4-4m5.618-4.016A11.955 11.955 0 0112 2.944a11.955 11.955 0 01-8.618 3.04A12.02 12.02 0 003 9c0 5.591 3.824 10.29 9 11.622 5.176-1.332 9-6.03 9-11.622 0-1.042-.133-2.052-.382-3.016z"/></svg> {{ $t('_common.run_as') }}
          </th>
          <th scope="col"></th>
        </tr>
        </thead>
        <tbody>
        <tr v-for="(c, i) in cmds[type]">
          <td>
            <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="c.do" />
          </td>
          <td>
            <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="c.undo" />
          </td>
          <td v-if="platform === 'windows'" class="align-middle">
            <Checkbox :id="type + '-cmd-admin-' + i"
                      label="_common.elevated"
                      desc=""
                      default="false"
                      v-model="c.elevated"
            ></Checkbox>
          </td>
          <td class="text-right">
            <button class="btn btn-danger me-2" @click="removeCmd(cmds[type], i)">
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
            </button>
            <button class="bg-ice/20 text-ice px-3 py-1.5 rounded-lg hover:bg-ice/30 transition" @click="addCmd(cmds[type], prepCmdTemplate, i)">
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
            </button>
          </td>
        </tr>
        </tbody>
      </table>
      <button class="mt-2 bg-ice/20 text-ice px-3 py-1.5 rounded-lg hover:bg-ice/30 transition" style="margin: 0 auto" @click="addCmd(cmds[type], prepCmdTemplate, -1)">
        &plus; {{ $t('config.add') }}
      </button>
    </div>

    <!-- Server Commands -->
    <div id="server_cmd" class="mb-3 d-flex flex-column">
      <div class="section-title-row mb-1">
        <label class="block text-sm font-medium text-storm">{{ $t('config.server_cmd') }}</label>
        <InfoHint size="sm" label="Server command guidance">
          Expose trusted host-side shortcuts that clients can trigger when paired access allows it.
        </InfoHint>
      </div>
      <table class="w-full text-left" v-if="serverCmd.length > 0">
        <thead>
        <tr>
          <th scope="col"><svg class="w-4 h-4 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M7 7h.01M7 3h5c.512 0 1.024.195 1.414.586l7 7a2 2 0 010 2.828l-7 7a2 2 0 01-2.828 0l-7-7A1.994 1.994 0 013 12V7a4 4 0 014-4z"/></svg> {{ $t('_common.cmd_name') }}</th>
          <th scope="col"><svg class="w-4 h-4 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 9l3 3-3 3m5 0h3M5 20h14a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z"/></svg> {{ $t('_common.cmd_val') }}</th>
          <th scope="col" v-if="platform === 'windows'">
            <svg class="w-4 h-4 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12l2 2 4-4m5.618-4.016A11.955 11.955 0 0112 2.944a11.955 11.955 0 01-8.618 3.04A12.02 12.02 0 003 9c0 5.591 3.824 10.29 9 11.622 5.176-1.332 9-6.03 9-11.622 0-1.042-.133-2.052-.382-3.016z"/></svg> {{ $t('_common.run_as') }}
          </th>
          <th scope="col"></th>
        </tr>
        </thead>
        <tbody>
        <tr v-for="(c, i) in serverCmd">
          <td>
            <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="c.name" />
          </td>
          <td>
            <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="c.cmd" />
          </td>
          <td v-if="platform === 'windows'">
            <div class="flex items-center gap-2">
              <input type="checkbox" class="w-4 h-4 rounded border-storm bg-deep text-ice focus:ring-ice" :id="'server-cmd-admin-' + i" v-model="c.elevated"/>
              <label :for="'server-cmd-admin-' + i" class="text-silver text-sm">{{ $t('_common.elevated') }}</label>
            </div>
          </td>
          <td class="text-right">
            <button class="btn btn-danger me-2" @click="removeCmd(serverCmd, i)">
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
            </button>
            <button class="bg-ice/20 text-ice px-3 py-1.5 rounded-lg hover:bg-ice/30 transition" @click="addCmd(serverCmd, serverCmdTemplate, i)">
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
            </button>
          </td>
        </tr>
        </tbody>
      </table>
      <button class="mt-2 bg-ice/20 text-ice px-3 py-1.5 rounded-lg hover:bg-ice/30 transition" style="margin: 0 auto" @click="addCmd(serverCmd, serverCmdTemplate, -1)">
        &plus; {{ $t('config.add') }}
      </button>
    </div>

      </div>
    </section>

    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Desktop & updates</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Desktop behavior</h3>
          <InfoHint size="sm" label="Desktop UI behavior guidance">
            Keep the desktop-side Polaris experience predictable, from update notifications to tray visibility and admin controls.
          </InfoHint>
        </div>
      </div>
      <div class="settings-inline-stack">

    <!-- Notify Pre-Releases -->
    <Checkbox class="mb-3"
              id="notify_pre_releases"
              locale-prefix="config"
              v-model="config.notify_pre_releases"
              default="false"
    ></Checkbox>

    <!-- Enable system tray -->
    <Checkbox class="mb-3"
              id="system_tray"
              locale-prefix="config"
              v-model="config.system_tray"
              default="true"
    ></Checkbox>

    <!-- Hide Tray Controls -->
    <Checkbox
              id="hide_tray_controls"
              locale-prefix="config"
              v-model="config.hide_tray_controls"
              default="false"
    ></Checkbox>

      </div>
    </section>

    <section class="settings-section settings-section-compact">
      <div class="settings-section-header">
        <div class="section-kicker">Metadata</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Artwork integration</h3>
          <InfoHint size="sm" label="Artwork integration guidance">
            Connect SteamGridDB so Polaris can fetch artwork for non-Steam entries in your library.
          </InfoHint>
        </div>
      </div>
      <div class="mt-1">
        <label for="steamgriddb_api_key" class="block text-sm font-medium text-storm mb-1">SteamGridDB API Key</label>
        <div v-if="config.has_steamgriddb_api_key && !config.clear_steamgriddb_api_key" class="mb-2 flex items-center justify-between gap-3 rounded-xl border border-emerald-300/20 bg-emerald-300/8 px-3 py-2 text-xs text-emerald-100">
          <span>A SteamGridDB key is already stored on the host. Leave this blank to keep it, or type a new key to replace it.</span>
          <button
            type="button"
            class="rounded-full border border-emerald-300/25 px-2.5 py-1 text-[11px] font-medium text-emerald-100 transition-colors hover:border-rose-300/40 hover:text-rose-200"
            @click="config.clear_steamgriddb_api_key = true; config.steamgriddb_api_key = ''">
            Clear Stored Key
          </button>
        </div>
        <div v-else-if="config.clear_steamgriddb_api_key" class="mb-2 flex items-center justify-between gap-3 rounded-xl border border-rose-300/20 bg-rose-300/8 px-3 py-2 text-xs text-rose-100">
          <span>The stored SteamGridDB key will be removed when you save.</span>
          <button
            type="button"
            class="rounded-full border border-rose-300/30 px-2.5 py-1 text-[11px] font-medium text-rose-100 transition-colors hover:border-ice/40 hover:text-ice"
            @click="config.clear_steamgriddb_api_key = false">
            Keep Existing Key
          </button>
        </div>
        <input type="password" id="steamgriddb_api_key"
               class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm"
               v-model="config.steamgriddb_api_key"
               @input="config.steamgriddb_api_key && (config.clear_steamgriddb_api_key = false)"
               placeholder="Enter your SteamGridDB API key" />
        <div class="text-xs text-storm mt-1">Enables cover art search for non-Steam games. Get a free key at <a href="https://www.steamgriddb.com/profile/preferences/api" target="_blank" class="text-ice hover:text-ice/80">steamgriddb.com</a>.</div>
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
