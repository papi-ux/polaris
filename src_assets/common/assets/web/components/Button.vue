<template>
  <button
    :class="cn(buttonVariants({ variant, size }), $attrs.class)"
    :disabled="disabled || loading"
  >
    <svg v-if="loading" class="animate-spin shrink-0" :class="iconSize" fill="none" viewBox="0 0 24 24">
      <circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" />
      <path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z" />
    </svg>
    <slot />
  </button>
</template>

<script setup>
import { computed } from 'vue'
import { cva } from 'class-variance-authority'
import { clsx } from 'clsx'
import { twMerge } from 'tailwind-merge'

const cn = (...inputs) => twMerge(clsx(inputs))

const props = defineProps({
  variant: { type: String, default: 'primary' },
  size: { type: String, default: 'md' },
  loading: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
})

const buttonVariants = cva(
  'inline-flex items-center justify-center gap-2 font-medium rounded-lg transition-all duration-200 cursor-pointer disabled:opacity-50 disabled:cursor-not-allowed focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ice/50 focus-visible:ring-offset-2 focus-visible:ring-offset-deep',
  {
    variants: {
      variant: {
        primary: 'bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] active:bg-ice/80',
        danger: 'bg-red-500 text-white hover:bg-red-600 hover:shadow-[0_0_24px_rgba(239,68,68,0.2)] active:bg-red-700',
        success: 'bg-ice/20 text-ice hover:bg-ice/30 hover:shadow-[0_0_24px_rgba(34,197,94,0.2)] active:bg-green-800',
        warning: 'bg-yellow-600 text-white hover:bg-yellow-700 active:bg-yellow-800',
        ghost: 'text-storm hover:text-silver hover:bg-twilight/50 active:bg-twilight',
        outline: 'border border-storm text-silver hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] active:bg-twilight/30',
      },
      size: {
        sm: 'h-8 px-3 text-xs',
        md: 'h-9 px-4 text-sm',
        lg: 'h-11 px-6 text-base',
        icon: 'h-9 w-9 p-0',
      },
    },
    defaultVariants: {
      variant: 'primary',
      size: 'md',
    },
  }
)

const iconSize = computed(() => {
  return { sm: 'w-3 h-3', md: 'w-4 h-4', lg: 'w-5 h-5', icon: 'w-4 h-4' }[props.size]
})
</script>
