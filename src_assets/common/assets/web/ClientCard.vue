<template>
    <div class="card p-4">
        <h2 class="text-xl font-semibold text-silver">{{ $t('client_card.clients') }}</h2>
        <p class="text-storm mt-2">{{ $t('client_card.clients_desc') }}</p>
        <div class="flex flex-wrap gap-2 p-4 items-center">
            <a v-for="{ platform, icon, name, link } of clients"
               class="px-4 py-2 rounded-lg transition no-underline inline-flex items-center gap-2"
               :class="[link ? 'bg-ice/20 text-ice hover:bg-ice/30' : 'bg-storm/30 text-storm']"
               :href="link" target="_blank" @click="shouldOpen($event, link)">
              <span v-if="icon === 'android'">
                <svg class="w-4 h-4" fill="currentColor" viewBox="0 0 24 24"><path d="M17.532 15.106a1.003 1.003 0 1 1 .001-2.007 1.003 1.003 0 0 1 0 2.007zm-11.063 0a1.003 1.003 0 1 1 .001-2.007 1.003 1.003 0 0 1 0 2.007zm11.402-4.426 2.005-3.472a.416.416 0 1 0-.72-.415l-2.03 3.516A12.42 12.42 0 0 0 12.001 9.2c-1.84 0-3.58.389-5.125 1.109l-2.03-3.516a.416.416 0 1 0-.72.415l2.005 3.472C2.695 12.678.436 16.458 0 20.78h24c-.435-4.322-2.695-8.102-6.13-10.1z"/></svg>
              </span>
              <span v-else-if="icon === 'apple'">
                <svg class="w-4 h-4" fill="currentColor" viewBox="0 0 24 24"><path d="M18.71 19.5c-.83 1.24-1.71 2.45-3.05 2.47-1.34.03-1.77-.79-3.29-.79-1.53 0-2 .77-3.27.82-1.31.05-2.3-1.32-3.14-2.53C4.25 17 2.94 12.45 4.7 9.39c.87-1.52 2.43-2.48 4.12-2.51 1.28-.02 2.5.87 3.29.87.78 0 2.26-1.07 3.8-.91.65.03 2.47.26 3.64 1.98-.09.06-2.17 1.28-2.15 3.81.03 3.02 2.65 4.03 2.68 4.04-.03.07-.42 1.44-1.38 2.83M13 3.5c.73-.83 1.94-1.46 2.94-1.5.13 1.17-.34 2.35-1.04 3.19-.69.85-1.83 1.51-2.95 1.42-.15-1.15.41-2.35 1.05-3.11z"/></svg>
              </span>
              <span v-else>
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9.75 17L9 20l-1 1h8l-1-1-.75-3M3 13h18M5 17h14a2 2 0 002-2V5a2 2 0 00-2-2H5a2 2 0 00-2 2v10a2 2 0 002 2z"/></svg>
              </span>
              <span class="font-bold">{{ platform }}</span> [ {{ name }} ]
            </a>
        </div>
        <i class="text-sm text-storm">* {{ $t('client_card.generic_moonlight_clients_desc') }}</i>
    </div>
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
            e.preventDefault();
        }
    }
</script>
