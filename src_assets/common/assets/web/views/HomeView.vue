<template>
  <h1 class="text-2xl font-bold text-silver my-6">{{ $t('index.welcome') }}</h1>
  <p class="text-silver">{{ $t('index.description') }}</p>

  <!-- Fatal errors -->
  <div class="bg-twilight/50 border-l-4 border-red-500 text-silver p-4 rounded-lg my-4" v-if="fancyLogs.find(x => x.level === 'Fatal')">
    <div class="flex items-center gap-2 mb-2">
      <svg class="w-6 h-6 text-red-500" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4c-.77-.833-1.964-.833-2.732 0L4.082 16.5c-.77.833.192 2.5 1.732 2.5z"/></svg>
      <p v-html="$t('index.startup_errors')"></p>
    </div>
    <ul class="list-disc list-inside ml-4 mb-3">
      <li v-for="v in fancyLogs.filter(x => x.level === 'Fatal')">{{v.value}}</li>
    </ul>
    <router-link class="bg-red-500 text-white px-4 py-2 rounded-lg hover:bg-red-600 transition no-underline inline-block" to="/troubleshooting#logs">View Logs</router-link>
  </div>

  <!-- Version -->
  <div class="card p-4 my-6" v-if="version">
    <h2 class="text-xl font-semibold text-silver">Version {{version.version}}</h2>

    <div v-if="loading" class="mt-3 text-storm">
      {{ $t('index.loading_latest') }}
    </div>

    <div class="bg-twilight/50 border-l-4 border-ice text-silver p-3 rounded-lg mt-3" v-if="buildVersionIsDirty">
      {{ $t('index.version_dirty') }}
    </div>

    <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-3 rounded-lg mt-3" v-if="installedVersionNotStable">
      {{ $t('index.installed_version_not_stable') }}
    </div>

    <div v-else-if="(!preReleaseBuildAvailable || !notifyPreReleases) && !stableBuildAvailable && !buildVersionIsDirty">
      <div class="bg-twilight/50 border-l-4 border-green-500 text-silver p-3 rounded-lg mt-3">
        {{ $t('index.version_latest') }}
      </div>
    </div>

    <div v-if="notifyPreReleases && preReleaseBuildAvailable">
      <div class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-3 rounded-lg mt-3">
        <div class="flex justify-between items-center">
          <p class="my-2">{{ $t('index.new_pre_release') }}</p>
          <a class="bg-ice/20 text-ice px-4 py-2 rounded-lg hover:bg-ice/30 transition no-underline" :href="preReleaseVersion.release.html_url" target="_blank">{{ $t('index.download') }}</a>
        </div>
        <pre class="font-bold mt-2 text-silver whitespace-pre-wrap">{{preReleaseVersion.release.name}}</pre>
        <pre class="text-storm whitespace-pre-wrap">{{preReleaseVersion.release.body}}</pre>
      </div>
    </div>

    <div v-if="stableBuildAvailable">
      <div class="bg-twilight/50 border-l-4 border-yellow-500 text-silver p-3 rounded-lg mt-3">
        <div class="flex justify-between items-center">
          <p class="my-2">{{ $t('index.new_stable') }}</p>
          <a class="bg-ice/20 text-ice px-4 py-2 rounded-lg hover:bg-ice/30 transition no-underline" :href="githubVersion.release.html_url" target="_blank">{{ $t('index.download') }}</a>
        </div>
        <h3 class="text-lg font-semibold text-silver mt-2">{{githubVersion.release.name}}</h3>
        <pre class="text-storm whitespace-pre-wrap">{{githubVersion.release.body}}</pre>
      </div>
    </div>
  </div>

  <!-- Resources -->
  <div class="my-6">
    <ClientCard />
    <ResourceCard />
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
import ClientCard from '../ClientCard.vue'
import ResourceCard from '../ResourceCard.vue'
import PolarisVersion from '../polaris_version'

const version = ref(null)
const githubVersion = ref(null)
const notifyPreReleases = ref(false)
const preReleaseVersion = ref(null)
const loading = ref(true)
const logs = ref(null)

const installedVersionNotStable = computed(() => {
  if (!githubVersion.value || !version.value) return false
  return version.value.isGreater(githubVersion.value)
})

const stableBuildAvailable = computed(() => {
  if (!githubVersion.value || !version.value) return false
  return githubVersion.value.isGreater(version.value)
})

const preReleaseBuildAvailable = computed(() => {
  if (!preReleaseVersion.value || !githubVersion.value || !version.value) return false
  return preReleaseVersion.value.isGreater(version.value, true) && preReleaseVersion.value.isGreater(githubVersion.value, true)
})

const buildVersionIsDirty = computed(() => {
  return version.value?.version?.split(".").length === 5 &&
    version.value.version.indexOf("dirty") !== -1
})

/** Parse the text errors, calculating the text, the timestamp and the level */
const fancyLogs = computed(() => {
  if (!logs.value) return []
  let regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g
  let rawLogLines = (logs.value.split(regex)).splice(1)
  let logLines = []
  for (let i = 0; i < rawLogLines.length; i += 2) {
    logLines.push({ timestamp: rawLogLines[i], level: rawLogLines[i + 1].split(":")[0], value: rawLogLines[i + 1] })
  }
  return logLines
})

// created() logic
;(async () => {
  try {
    let config = await fetch("./api/config").then((r) => r.json())
    notifyPreReleases.value = config.notify_pre_releases
    version.value = new PolarisVersion(null, config.version)
    githubVersion.value = new PolarisVersion(await fetch("https://api.github.com/repos/papi/Polaris/releases/latest").then((r) => r.json()), null)
    if (githubVersion.value) {
      preReleaseVersion.value = new PolarisVersion((await fetch("https://api.github.com/repos/papi/Polaris/releases").then((r) => r.json())).find(release => release.prerelease), null)
    }
  } catch (e) {
    // GitHub API may be blocked by CSP — version check is non-critical
  }
  try {
    logs.value = (await fetch("./api/logs").then(r => r.text()))
  } catch (e) {
    console.error(e)
  }
  loading.value = false
})()
</script>
