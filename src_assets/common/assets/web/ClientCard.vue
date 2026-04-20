<template>
  <section class="section-card">
    <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
      <div class="min-w-0">
        <div class="section-kicker">{{ $t('client_card.clients') }}</div>
        <h2 class="section-title">{{ $t('client_card.clients') }}</h2>
        <p class="section-copy max-w-2xl">Supported client surfaces and current availability.</p>
      </div>
      <div class="grid gap-2 sm:grid-cols-3">
        <a
          v-for="{ platform, icon, name, link } of clients"
          :key="platform"
          class="focus-ring surface-subtle flex min-w-[170px] items-center justify-between gap-3 px-3 py-3 no-underline transition-[border-color,background-color,color] duration-200"
          :class="link ? 'hover:border-ice/35 hover:bg-twilight/35' : ''"
          :href="link"
          target="_blank"
          @click="shouldOpen($event, link)"
        >
          <div class="flex min-w-0 items-center gap-3">
            <span class="inline-flex h-9 w-9 items-center justify-center rounded-lg border border-storm/20 bg-void/55 text-silver">
              <svg v-if="icon === 'android'" class="h-4 w-4" fill="currentColor" viewBox="0 0 24 24"><path d="M17.532 15.106a1.003 1.003 0 1 1 .001-2.007 1.003 1.003 0 0 1 0 2.007zm-11.063 0a1.003 1.003 0 1 1 .001-2.007 1.003 1.003 0 0 1 0 2.007zm11.402-4.426 2.005-3.472a.416.416 0 1 0-.72-.415l-2.03 3.516A12.42 12.42 0 0 0 12.001 9.2c-1.84 0-3.58.389-5.125 1.109l-2.03-3.516a.416.416 0 1 0-.72.415l2.005 3.472C2.695 12.678.436 16.458 0 20.78h24c-.435-4.322-2.695-8.102-6.13-10.1z"/></svg>
              <svg v-else-if="icon === 'apple'" class="h-4 w-4" fill="currentColor" viewBox="0 0 24 24"><path d="M18.71 19.5c-.83 1.24-1.71 2.45-3.05 2.47-1.34.03-1.77-.79-3.29-.79-1.53 0-2 .77-3.27.82-1.31.05-2.3-1.32-3.14-2.53C4.25 17 2.94 12.45 4.7 9.39c.87-1.52 2.43-2.48 4.12-2.51 1.28-.02 2.5.87 3.29.87.78 0 2.26-1.07 3.8-.91.65.03 2.47.26 3.64 1.98-.09.06-2.17 1.28-2.15 3.81.03 3.02 2.65 4.03 2.68 4.04-.03.07-.42 1.44-1.38 2.83M13 3.5c.73-.83 1.94-1.46 2.94-1.5.13 1.17-.34 2.35-1.04 3.19-.69.85-1.83 1.51-2.95 1.42-.15-1.15.41-2.35 1.05-3.11z"/></svg>
              <svg v-else class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9.75 17L9 20l-1 1h8l-1-1-.75-3M3 13h18M5 17h14a2 2 0 002-2V5a2 2 0 00-2-2H5a2 2 0 00-2 2v10a2 2 0 002 2z"/></svg>
            </span>
            <div class="min-w-0">
              <div class="text-sm font-medium text-silver">{{ platform }}</div>
              <div class="truncate text-xs text-storm">{{ name }}</div>
            </div>
          </div>
          <span class="rounded-full border px-2 py-0.5 text-[10px] font-medium uppercase tracking-[0.18em]"
                :class="link ? 'border-ice/20 bg-ice/10 text-ice' : 'border-storm/20 bg-void/50 text-storm'">
            {{ link ? 'Live' : 'Planned' }}
          </span>
        </a>
      </div>
    </div>
    <p class="mt-4 text-xs italic text-storm">* {{ $t('client_card.generic_moonlight_clients_desc') }}</p>
  </section>
</template>

<script setup>
const clients = [
  {
    platform: 'Android',
    icon: 'android',
    name: 'Nova',
    link: 'https://github.com/papi-ux/nova'
  },
  {
    platform: 'iOS',
    icon: 'apple',
    name: 'Coming soon...',
    link: ''
  },
  {
    platform: 'Desktop',
    icon: 'desktop',
    name: 'Coming soon...',
    link: ''
  }
]

const shouldOpen = (e, link) => {
  if (!link) {
    e.preventDefault()
  }
}
</script>
