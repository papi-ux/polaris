<template>
  <div class="space-y-4">
    <section class="section-card">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
        <div class="min-w-0">
          <div class="section-kicker">{{ $t('resource_card.resources') }}</div>
          <h2 class="section-title">{{ $t('resource_card.resources') }}</h2>
          <p class="section-copy max-w-2xl">Support, docs, and tuning references in one place.</p>
        </div>
        <div class="flex flex-wrap gap-2">
          <a
            v-for="resource in resources"
            :key="resource.href"
            class="focus-ring inline-flex h-9 items-center justify-center rounded-lg border border-storm px-4 text-sm font-medium text-silver transition-[border-color,color,box-shadow,background-color] duration-200 hover:border-ice hover:bg-twilight/35 hover:text-ice no-underline"
            :href="resource.href"
            target="_blank"
          >
            {{ $t(resource.labelKey) }}
          </a>
        </div>
      </div>
    </section>

    <section class="section-card">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
        <div class="min-w-0">
          <div class="section-kicker">{{ $t('resource_card.legal') }}</div>
          <h2 class="section-title">{{ $t('resource_card.legal') }}</h2>
          <p class="section-copy max-w-2xl">License and third-party notice documents.</p>
        </div>
        <div class="flex flex-wrap gap-2">
          <a
            v-for="document in legalDocs"
            :key="document.href"
            class="focus-ring inline-flex h-9 items-center justify-center rounded-lg bg-danger/18 px-4 text-sm font-medium text-danger transition-[background-color,color] duration-200 hover:bg-danger/28 no-underline"
            :href="document.href"
            target="_blank"
          >
            {{ $t(document.labelKey) }}
          </a>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup>
import { resources, legalDocs } from './resource-links.js'
</script>
