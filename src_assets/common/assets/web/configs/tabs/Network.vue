<script setup>
import { computed, ref } from 'vue'
import Checkbox from "../../Checkbox.vue";

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
    <!-- UPnP -->
    <Checkbox class="mb-3"
              id="upnp"
              locale-prefix="config"
              v-model="config.upnp"
              default="false"
    ></Checkbox>

    <!-- Address family -->
    <div class="mb-3">
      <label for="address_family" class="block text-sm font-medium text-storm mb-1">{{ $t('config.address_family') }}</label>
      <select id="address_family" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.address_family">
        <option value="ipv4">{{ $t('config.address_family_ipv4') }}</option>
        <option value="both">{{ $t('config.address_family_both') }}</option>
      </select>
      <div class="text-sm text-storm mt-1">{{ $t('config.address_family_desc') }}</div>
    </div>

    <!-- Port family -->
    <div class="mb-3">
      <label for="port" class="block text-sm font-medium text-storm mb-1">{{ $t('config.port') }}</label>
      <input type="number" min="1029" max="65514" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="port" :placeholder="defaultMoonlightPort"
             v-model="config.port" />
      <div class="text-sm text-storm mt-1">{{ $t('config.port_desc') }}</div>
      <!-- Add warning if any port is less than 1024 -->
      <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg" v-if="(+effectivePort - 5) < 1024">
        <svg class="w-5 h-5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4c-.77-.833-1.964-.833-2.732 0L4.082 16.5c-.77.833.192 2.5 1.732 2.5z"/></svg> {{ $t('config.port_alert_1') }}
      </div>
      <!-- Add warning if any port is above 65535 -->
      <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-3 rounded-lg" v-if="(+effectivePort + 21) > 65535">
        <svg class="w-5 h-5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4c-.77-.833-1.964-.833-2.732 0L4.082 16.5c-.77.833.192 2.5 1.732 2.5z"/></svg> {{ $t('config.port_alert_2') }}
      </div>
      <!-- Create a port table for the various ports needed by Polaris -->
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
          <!-- HTTPS -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort - 5}}</td>
          <td></td>
        </tr>
        <tr>
          <!-- HTTP -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort}}</td>
          <td>
            <div class="bg-twilight/50 border-l-4 border-ice text-silver p-3 rounded-lg" role="alert" v-if="+effectivePort !== defaultMoonlightPort">
              <svg class="w-5 h-5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg> {{ $t('config.port_http_port_note') }}
            </div>
          </td>
        </tr>
        <tr>
          <!-- Web UI -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort + 1}}</td>
          <td>{{ $t('config.port_web_ui') }}</td>
        </tr>
        <tr>
          <!-- RTSP -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort + 21}}</td>
          <td></td>
        </tr>
        <tr>
          <!-- Video, Control, Audio -->
          <td>{{ $t('config.port_udp') }}</td>
          <td>{{+effectivePort + 9}} - {{+effectivePort + 11}}</td>
          <td></td>
        </tr>
        <!--            <tr>-->
        <!--              &lt;!&ndash; Mic &ndash;&gt;-->
        <!--              <td>UDP</td>-->
        <!--              <td>{{+effectivePort + 13}}</td>-->
        <!--              <td></td>-->
        <!--            </tr>-->
        </tbody>
      </table>
      <!-- add warning about exposing web ui to the internet -->
      <div class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-3 rounded-lg" v-if="config.origin_web_ui_allowed === 'wan'">
        <svg class="w-5 h-5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4c-.77-.833-1.964-.833-2.732 0L4.082 16.5c-.77.833.192 2.5 1.732 2.5z"/></svg> {{ $t('config.port_warning') }}
      </div>
    </div>

    <!-- Origin Web UI Allowed -->
    <div class="mb-3">
      <label for="origin_web_ui_allowed" class="block text-sm font-medium text-storm mb-1">{{ $t('config.origin_web_ui_allowed') }}</label>
      <select id="origin_web_ui_allowed" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.origin_web_ui_allowed">
        <option value="pc">{{ $t('config.origin_web_ui_allowed_pc') }}</option>
        <option value="lan">{{ $t('config.origin_web_ui_allowed_lan') }}</option>
        <option value="wan">{{ $t('config.origin_web_ui_allowed_wan') }}</option>
      </select>
      <div class="text-sm text-storm mt-1">{{ $t('config.origin_web_ui_allowed_desc') }}</div>
    </div>

    <!-- External IP -->
    <div class="mb-3">
      <label for="external_ip" class="block text-sm font-medium text-storm mb-1">{{ $t('config.external_ip') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="external_ip" placeholder="123.456.789.12" v-model="config.external_ip" />
      <div class="text-sm text-storm mt-1">{{ $t('config.external_ip_desc') }}</div>
    </div>

    <!-- LAN Encryption Mode -->
    <div class="mb-3">
      <label for="lan_encryption_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.lan_encryption_mode') }}</label>
      <select id="lan_encryption_mode" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.lan_encryption_mode">
        <option value="0">{{ $t('_common.disabled_def') }}</option>
        <option value="1">{{ $t('config.lan_encryption_mode_1') }}</option>
        <option value="2">{{ $t('config.lan_encryption_mode_2') }}</option>
      </select>
      <div class="text-sm text-storm mt-1">{{ $t('config.lan_encryption_mode_desc') }}</div>
    </div>

    <!-- WAN Encryption Mode -->
    <div class="mb-3">
      <label for="wan_encryption_mode" class="block text-sm font-medium text-storm mb-1">{{ $t('config.wan_encryption_mode') }}</label>
      <select id="wan_encryption_mode" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="config.wan_encryption_mode">
        <option value="0">{{ $t('_common.disabled') }}</option>
        <option value="1">{{ $t('config.wan_encryption_mode_1') }}</option>
        <option value="2">{{ $t('config.wan_encryption_mode_2') }}</option>
      </select>
      <div class="text-sm text-storm mt-1">{{ $t('config.wan_encryption_mode_desc') }}</div>
    </div>

    <!-- Ping Timeout -->
    <div class="mb-3">
      <label for="ping_timeout" class="block text-sm font-medium text-storm mb-1">{{ $t('config.ping_timeout') }}</label>
      <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="ping_timeout" placeholder="10000" v-model="config.ping_timeout" />
      <div class="text-sm text-storm mt-1">{{ $t('config.ping_timeout_desc') }}</div>
    </div>

    <!-- Trusted Subnets (TOFU Pairing) -->
    <div class="mb-3">
      <label class="block text-sm font-medium text-storm mb-1">Trusted Subnets (TOFU)</label>
      <div class="text-sm text-storm mb-2">
        Clients on these subnets can pair automatically without a PIN. Use CIDR notation (e.g. <code class="bg-deep px-1 rounded">10.0.0.0/24</code>).
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
      <div class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-3 rounded-lg mt-2" v-if="config.trusted_subnets && config.trusted_subnets.length > 0">
        <svg class="w-5 h-5 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4c-.77-.833-1.964-.833-2.732 0L4.082 16.5c-.77.833.192 2.5 1.732 2.5z"/></svg>
        Any device on these networks can pair without approval. Only trust networks you control.
      </div>
    </div>

  </div>
</template>

<style scoped>

</style>
