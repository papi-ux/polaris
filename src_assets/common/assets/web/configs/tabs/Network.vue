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
const effectivePort = computed(() => Number(config.value?.port) || defaultMoonlightPort)
</script>

<template>
  <div id="network" class="config-page">
    <p class="text-xs text-storm" data-tab-docs-link>
      <a href="https://papi-ux.com/docs/configuration/#common-options" target="_blank" rel="noopener" class="focus-ring text-ice hover:underline">{{ $t('config.network_docs_link') }}</a>
    </p>
    <section class="settings-section">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.network_kicker_exposure') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.network_section_addressing_and_access') }}</h3>
          <InfoHint size="sm" :label="$t('config.network_hint_exposure_guidance')">
            {{ $t('config.network_hint_exposure_guidance_body') }}
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
        <select id="address_family" class="settings-input" v-model="config.address_family">
          <option value="ipv4">{{ $t('config.address_family_ipv4') }}</option>
          <option value="both">{{ $t('config.address_family_both') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.address_family_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="port" class="block text-sm font-medium text-storm mb-1">{{ $t('config.port') }}</label>
        <input type="number" min="1029" max="65514" class="settings-input" id="port" :placeholder="defaultMoonlightPort"
               v-model="config.port" />
        <div class="text-sm text-storm mt-1">{{ $t('config.port_desc') }}</div>

        <div class="settings-warning-surface mt-3" v-if="(+effectivePort - 5) < 1024">
          {{ $t('config.port_alert_1') }}
        </div>

        <div class="settings-warning-surface mt-3" v-if="(+effectivePort + 21) > 65535">
          {{ $t('config.port_alert_2') }}
        </div>

        <div class="settings-subtle-surface mt-3">
          <div class="mb-2 text-[11px] font-medium uppercase tracking-eyebrow text-storm">{{ $t('config.network_port_map') }}</div>
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
        <select id="origin_web_ui_allowed" class="settings-input" v-model="config.origin_web_ui_allowed">
          <option value="pc">{{ $t('config.origin_web_ui_allowed_pc') }}</option>
          <option value="lan">{{ $t('config.origin_web_ui_allowed_lan') }}</option>
          <option value="wan">{{ $t('config.origin_web_ui_allowed_wan') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.origin_web_ui_allowed_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="external_ip" class="block text-sm font-medium text-storm mb-1">{{ $t('config.external_ip') }}</label>
        <input type="text" class="settings-input" id="external_ip" placeholder="123.456.789.12" v-model="config.external_ip" />
        <div class="text-sm text-storm mt-1">{{ $t('config.external_ip_desc') }}</div>
      </div>
    </section>

    <section id="encryption_and_trust" class="settings-section scroll-mt-28">
      <div class="settings-section-header">
        <div class="section-kicker">{{ $t('config.network_kicker_transport_security') }}</div>
        <div class="section-title-row">
          <h3 class="settings-section-title">{{ $t('config.network_section_encryption_and_trust') }}</h3>
          <InfoHint size="sm" :label="$t('config.network_hint_encryption_and_trust_guidance')">
            {{ $t('config.network_hint_encryption_and_trust_guidance_body') }}
          </InfoHint>
        </div>
      </div>

      <div class="mb-3">
        <label for="lan_encryption_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.lan_encryption_mode') }}</label>
        <select id="lan_encryption_mode" class="settings-input" v-model="config.lan_encryption_mode">
          <option value="0">{{ $t('_common.disabled_def') }}</option>
          <option value="1">{{ $t('config.lan_encryption_mode_1') }}</option>
          <option value="2">{{ $t('config.lan_encryption_mode_2') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.lan_encryption_mode_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="wan_encryption_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.wan_encryption_mode') }}</label>
        <select id="wan_encryption_mode" class="settings-input" v-model="config.wan_encryption_mode">
          <option value="0">{{ $t('_common.disabled') }}</option>
          <option value="1">{{ $t('config.wan_encryption_mode_1') }}</option>
          <option value="2">{{ $t('config.wan_encryption_mode_2') }}</option>
        </select>
        <div class="text-sm text-storm mt-1">{{ $t('config.wan_encryption_mode_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="ping_timeout" class="block text-sm font-medium text-storm mb-1">{{ $t('config.ping_timeout') }}</label>
        <input type="text" class="settings-input" id="ping_timeout" placeholder="10000" v-model="config.ping_timeout" />
        <div class="text-sm text-storm mt-1">{{ $t('config.ping_timeout_desc') }}</div>
      </div>

      <div class="mb-3">
        <label class="block text-sm font-medium text-storm mb-1">{{ $t('config.network_tofu_label') }}</label>
        <label class="flex items-center gap-3 rounded-xl border border-storm/40 bg-deep/60 px-3 py-3 text-sm text-silver">
          <input
            type="checkbox"
            class="h-4 w-4 rounded border-storm bg-void text-ice focus:ring-ice"
            :checked="config.trusted_subnet_auto_pairing === 'enabled'"
            @change="config.trusted_subnet_auto_pairing = $event.target.checked ? 'enabled' : 'disabled'"
          />
          <span>{{ $t('config.network_tofu_copy') }}</span>
        </label>
        <div class="text-sm text-storm mt-2">{{ $t('config.network_tofu_hint') }}</div>
      </div>

      <div class="mb-3">
        <label class="block text-sm font-medium text-storm mb-1">{{ $t('config.network_trusted_subnets_label') }}</label>
        <div class="text-sm text-storm mb-2">
          Use IPv4 or IPv6 CIDR notation, for example <code class="bg-deep px-1 rounded">10.0.0.0/24</code> or <code class="bg-deep px-1 rounded">fd00:1234:5678::/64</code>.
        </div>
        <div v-if="config.trusted_subnets && config.trusted_subnets.length > 0" class="space-y-2 mb-2">
          <div v-for="(subnet, index) in config.trusted_subnets" :key="index" class="flex items-center gap-2">
            <input
              type="text"
              class="settings-input flex-1 font-mono text-sm"
              :placeholder="'10.0.0.0/24'"
              v-model="config.trusted_subnets[index]"
            />
            <button
              class="px-3 py-2 bg-deep border border-storm rounded-lg text-danger hover:bg-danger/20 hover:border-danger/50 transition-colors text-sm"
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
          {{ $t('config.network_trusted_subnets_warning') }}
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>

</style>
