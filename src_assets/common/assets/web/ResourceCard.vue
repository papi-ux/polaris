<template>
    <div class="card p-4 mt-4">
        <h2 class="text-xl font-semibold text-silver">{{ $t('resource_card.resources') }}</h2>
        <p class="text-storm mt-2">{{ $t('resource_card.resources_desc') }}</p>
        <div class="flex flex-wrap gap-2 p-4 items-center">
            <a class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] transition-all duration-200 no-underline inline-flex items-center gap-2" href="https://github.com/papi/Polaris/discussions" target="_blank">
                <svg class="w-4 h-4" fill="currentColor" viewBox="0 0 24 24"><path d="M12 0c-6.626 0-12 5.373-12 12 0 5.302 3.438 9.8 8.207 11.387.599.111.793-.261.793-.577v-2.234c-3.338.726-4.033-1.416-4.033-1.416-.546-1.387-1.333-1.756-1.333-1.756-1.089-.745.083-.729.083-.729 1.205.084 1.839 1.237 1.839 1.237 1.07 1.834 2.807 1.304 3.492.997.107-.775.418-1.305.762-1.604-2.665-.305-5.467-1.334-5.467-5.931 0-1.311.469-2.381 1.236-3.221-.124-.303-.535-1.524.117-3.176 0 0 1.008-.322 3.301 1.23.957-.266 1.983-.399 3.003-.404 1.02.005 2.047.138 3.006.404 2.291-1.552 3.297-1.23 3.297-1.23.653 1.653.242 2.874.118 3.176.77.84 1.235 1.911 1.235 3.221 0 4.609-2.807 5.624-5.479 5.921.43.372.823 1.102.823 2.222v3.293c0 .319.192.694.801.576 4.765-1.589 8.199-6.086 8.199-11.386 0-6.627-5.373-12-12-12z"/></svg>
                {{ $t('resource_card.github_discussions') }}
            </a>
            <a class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] transition-all duration-200 no-underline inline-flex items-center gap-2" href="https://github.com/papi/Polaris/wiki" target="_blank">
                <svg class="w-4 h-4" fill="currentColor" viewBox="0 0 24 24"><path d="M12 0c-6.626 0-12 5.373-12 12 0 5.302 3.438 9.8 8.207 11.387.599.111.793-.261.793-.577v-2.234c-3.338.726-4.033-1.416-4.033-1.416-.546-1.387-1.333-1.756-1.333-1.756-1.089-.745.083-.729.083-.729 1.205.084 1.839 1.237 1.839 1.237 1.07 1.834 2.807 1.304 3.492.997.107-.775.418-1.305.762-1.604-2.665-.305-5.467-1.334-5.467-5.931 0-1.311.469-2.381 1.236-3.221-.124-.303-.535-1.524.117-3.176 0 0 1.008-.322 3.301 1.23.957-.266 1.983-.399 3.003-.404 1.02.005 2.047.138 3.006.404 2.291-1.552 3.297-1.23 3.297-1.23.653 1.653.242 2.874.118 3.176.77.84 1.235 1.911 1.235 3.221 0 4.609-2.807 5.624-5.479 5.921.43.372.823 1.102.823 2.222v3.293c0 .319.192.694.801.576 4.765-1.589 8.199-6.086 8.199-11.386 0-6.627-5.373-12-12-12z"/></svg>
                {{ $t('resource_card.github_wiki') }}
            </a>
            <a class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] transition-all duration-200 no-underline inline-flex items-center gap-2" href="https://github.com/papi/Polaris/wiki/Stuttering-Clinic" target="_blank">
                <svg class="w-4 h-4" fill="currentColor" viewBox="0 0 24 24"><path d="M12 0c-6.626 0-12 5.373-12 12 0 5.302 3.438 9.8 8.207 11.387.599.111.793-.261.793-.577v-2.234c-3.338.726-4.033-1.416-4.033-1.416-.546-1.387-1.333-1.756-1.333-1.756-1.089-.745.083-.729.083-.729 1.205.084 1.839 1.237 1.839 1.237 1.07 1.834 2.807 1.304 3.492.997.107-.775.418-1.305.762-1.604-2.665-.305-5.467-1.334-5.467-5.931 0-1.311.469-2.381 1.236-3.221-.124-.303-.535-1.524.117-3.176 0 0 1.008-.322 3.301 1.23.957-.266 1.983-.399 3.003-.404 1.02.005 2.047.138 3.006.404 2.291-1.552 3.297-1.23 3.297-1.23.653 1.653.242 2.874.118 3.176.77.84 1.235 1.911 1.235 3.221 0 4.609-2.807 5.624-5.479 5.921.43.372.823 1.102.823 2.222v3.293c0 .319.192.694.801.576 4.765-1.589 8.199-6.086 8.199-11.386 0-6.627-5.373-12-12-12z"/></svg>
                {{ $t('resource_card.github_stuttering_clinic') }}
            </a>
        </div>
    </div>
    <!-- Legal -->
    <div class="card p-4 mt-4">
        <h2 class="text-xl font-semibold text-silver">{{ $t('resource_card.legal') }}</h2>
        <p class="text-storm mt-2">{{ $t('resource_card.legal_desc') }}</p>
        <div class="flex flex-wrap gap-2 p-4 items-center">
            <a class="bg-red-500/20 text-red-400 px-4 py-2 rounded-lg hover:bg-red-500/30 transition no-underline inline-flex items-center gap-2" href="https://github.com/papi/Polaris/blob/master/LICENSE" target="_blank">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z"/></svg>
                {{ $t('resource_card.license') }}
            </a>
            <a class="bg-red-500/20 text-red-400 px-4 py-2 rounded-lg hover:bg-red-500/30 transition no-underline inline-flex items-center gap-2" href="https://github.com/papi/Polaris/blob/master/NOTICE" target="_blank">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4c-.77-.833-1.964-.833-2.732 0L4.082 16.5c-.77.833.192 2.5 1.732 2.5z"/></svg>
                {{ $t('resource_card.third_party_notice') }}
            </a>
        </div>
    </div>
</template>
