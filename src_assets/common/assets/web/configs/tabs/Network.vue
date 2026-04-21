<script setup>
import { computed, ref } from 'vue'
import Checkbox from "../../Checkbox.vue";
import InfoHint from '../../components/InfoHint.vue'

const props = defineProps([
  'platform',
  'config'
])

const defaultMoonlightPort = 47989

const config = ref(props.config)
const effectivePort = computed(() => +config.value?.port ?? defaultMoonlightPort)
</script>

<template>
  <div id="network" class="config-page">
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Exposure</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Addressing and access</h3>
          <InfoHint size="sm" label="Exposure guidance">
            Define how Polaris announces itself, which ports it serves, and whether the web UI stays local-only or can be reached remotely.
          </InfoHint>
        </div>
      </div>

      <Checkbox class="mb-3"
                id="enable_discovery"
                locale-prefix="config"
                v-model="config.enable_discovery"
                default="true"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="enable_pairing"
                locale-prefix="config"
                v-model="config.enable_pairing"
                default="true"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="upnp"
                locale-prefix="config"
                v-model="config.upnp"
                default="false"
      ></Checkbox>

      <div class="mb-3">
        <label for="address_family" class="block text-sm font-medium text-storm mb-1">{{ $t('config.address_family') }}</label>
        <select id="address_family" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.address_family">
          <option value="ipv4">{{ $t('config.address_family_ipv4') }}</option>
          <option value="both">{{ $t('config.address_family_both') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.address_family_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="port" class="block text-sm font-medium text-storm mb-1">{{ $t('config.port') }}</label>
        <input type="number" min="1029" max="65514" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="port" :placeholder="defaultMoonlightPort"
               v-model="config.port" />
        <div class="text-sm text-storm mt-1">{{ $t('config.port_desc') }}</div>

        <div class="settings-warning-surface mt-3" v-if="(+effectivePort - 5) < 1024">
          {{ $t('config.port_alert_1') }}
        </div>

        <div class="settings-warning-surface mt-3" v-if="(+effectivePort + 21) > 65535">
          {{ $t('config.port_alert_2') }}
        </div>

        <div class="settings-subtle-surface mt-3">
          <div class="mb-2 text-[11px] font-medium uppercase tracking-[0.18em] text-storm">Port map</div>
          <table class="w-full text-left">
            <thead>
            <tr>
              <th scope="col">{{ $t('config.port_protocol') }}</th>
              <th scope="col">{{ $t('config.port_port') }}</th>
              <th scope="col">{{ $t('config.port_note') }}</th>
            </tr>
            </thead>
            <tbody>
            <tr>
              <td>{{ $t('config.port_tcp') }}</td>
              <td>{{+effectivePort - 5}}</td>
              <td></td>
            </tr>
            <tr>
              <td>{{ $t('config.port_tcp') }}</td>
              <td>{{+effectivePort}}</td>
              <td>
                <div class="rounded-xl border border-ice/20 bg-ice/5 p-3 text-sm text-silver" role="alert" v-if="+effectivePort !== defaultMoonlightPort">
                  {{ $t('config.port_http_port_note') }}
                </div>
              </td>
            </tr>
            <tr>
              <td>{{ $t('config.port_tcp') }}</td>
              <td>{{+effectivePort + 1}}</td>
              <td>{{ $t('config.port_web_ui') }}</td>
            </tr>
            <tr>
              <td>{{ $t('config.port_tcp') }}</td>
              <td>{{+effectivePort + 21}}</td>
              <td></td>
            </tr>
            <tr>
              <td>{{ $t('config.port_udp') }}</td>
              <td>{{+effectivePort + 9}} - {{+effectivePort + 11}}</td>
              <td></td>
            </tr>
            </tbody>
          </table>
        </div>

        <div class="settings-warning-surface mt-3" v-if="config.origin_web_ui_allowed === 'wan'">
          {{ $t('config.port_warning') }}
        </div>
      </div>

      <div class="mb-3">
        <label for="origin_web_ui_allowed" class="block text-sm font-medium text-storm mb-1">{{ $t('config.origin_web_ui_allowed') }}</label>
        <select id="origin_web_ui_allowed" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.origin_web_ui_allowed">
          <option value="pc">{{ $t('config.origin_web_ui_allowed_pc') }}</option>
          <option value="lan">{{ $t('config.origin_web_ui_allowed_lan') }}</option>
          <option value="wan">{{ $t('config.origin_web_ui_allowed_wan') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.origin_web_ui_allowed_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="external_ip" class="block text-sm font-medium text-storm mb-1">{{ $t('config.external_ip') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="external_ip" placeholder="123.456.789.12" v-model="config.external_ip" />
        <div class="text-sm text-storm mt-1">{{ $t('config.external_ip_desc') }}</div>
      </div>
    </section>

    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">Transport security</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">Encryption and trust</h3>
          <InfoHint size="sm" label="Encryption and trust guidance">
            Choose how aggressively Polaris encrypts LAN and WAN sessions, then define which subnets can use the trusted TOFU pairing path.
          </InfoHint>
        </div>
      </div>

      <div class="mb-3">
        <label for="lan_encryption_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.lan_encryption_mode') }}</label>
        <select id="lan_encryption_mode" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.lan_encryption_mode">
          <option value="0">{{ $t('_common.disabled_def') }}</option>
          <option value="1">{{ $t('config.lan_encryption_mode_1') }}</option>
          <option value="2">{{ $t('config.lan_encryption_mode_2') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.lan_encryption_mode_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="wan_encryption_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.wan_encryption_mode') }}</label>
        <select id="wan_encryption_mode" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.wan_encryption_mode">
          <option value="0">{{ $t('_common.disabled') }}</option>
          <option value="1">{{ $t('config.wan_encryption_mode_1') }}</option>
          <option value="2">{{ $t('config.wan_encryption_mode_2') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.wan_encryption_mode_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="ping_timeout" class="block text-sm font-medium text-storm mb-1">{{ $t('config.ping_timeout') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="ping_timeout" placeholder="10000" v-model="config.ping_timeout" />
        <div class="text-sm text-storm mt-1">{{ $t('config.ping_timeout_desc') }}</div>
      </div>

      <div class="mb-3">
        <label class="block text-sm font-medium text-storm mb-1">Trusted Subnet Auto-Pairing</label>
        <label class="flex items-center gap-3 rounded-xl border border-storm/40 bg-deep/60 px-3 py-3 text-sm text-silver">
          <input
            type="checkbox"
            class="h-4 w-4 rounded border-storm bg-void text-ice focus:ring-ice"
            :checked="config.trusted_subnet_auto_pairing === 'enabled'"
            @change="config.trusted_subnet_auto_pairing = $event.target.checked ? 'enabled' : 'disabled'"
          />
          <span>Allow trusted subnets to pair without a PIN.</span>
        </label>
        <div class="text-sm text-storm mt-2">Disabled by default. Use only on networks you fully control.</div>
      </div>

      <div class="mb-3">
        <label class="block text-sm font-medium text-storm mb-1">Trusted Subnets (TOFU)</label>
        <div class="text-sm text-storm mb-2">
          Use CIDR notation, for example <code class="bg-deep px-1 rounded">10.0.0.0/24</code>.
        </div>
        <div v-if="config.trusted_subnets && config.trusted_subnets.length > 0" class="space-y-2 mb-2">
          <div v-for="(subnet, index) in config.trusted_subnets" :key="index" class="flex items-center gap-2">
            <input
              type="text"
              class="flex-1 bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm"
              :placeholder="'10.0.0.0/24'"
              v-model="config.trusted_subnets[index]"
            />
            <button
              class="px-3 py-2 bg-deep border border-storm rounded-lg text-red-400 hover:bg-red-500/20 hover:border-red-500/50 transition-colors text-sm"
              @click="config.trusted_subnets.splice(index, 1)"
            >
              Remove
            </button>
          </div>
        </div>
        <button
          class="px-4 py-2 bg-deep border border-storm rounded-lg text-silver hover:bg-twilight/50 hover:border-ice/30 transition-colors text-sm"
          @click="if (!config.trusted_subnets) config.trusted_subnets = []; config.trusted_subnets.push('')"
        >
          + Add Subnet
        </button>
        <div class="settings-warning-surface mt-3" v-if="config.trusted_subnets && config.trusted_subnets.length > 0 && config.trusted_subnet_auto_pairing === 'enabled'">
          Any device on these networks can pair without approval. Only trust networks you control.
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
