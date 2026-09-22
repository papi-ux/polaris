/**
 * Shared semantic status tones for self-test cards and badges.
 *
 * Doctor & Support checklists and the built-in self tests grade everything as
 * pass, fail, or warning, and a Doctor finding that needs nothing done is info. These strings are the single source for the tone
 * classes those grades render with; any status outside the known set keeps
 * the historical fallback of the warning tone so a new grade degrades to
 * "check this" instead of rendering unstyled.
 */
export const STATUS_TONES = {
  pass: {
    card: 'border-success/20',
    badge: 'border border-success/30 bg-success/10 text-success',
    label: 'Looks good',
  },
  fail: {
    card: 'border-danger/25',
    badge: 'border border-danger/30 bg-danger/10 text-danger-bright',
    label: 'Fix first',
  },
  warning: {
    card: 'border-warning/25',
    badge: 'border border-warning/30 bg-warning/10 text-warning-bright',
    label: 'Check',
  },
  info: {
    card: 'border-ice/20',
    badge: 'border border-ice/30 bg-ice/10 text-ice',
    label: 'Note',
  },
}

export function statusTone(status) {
  return STATUS_TONES[status] || STATUS_TONES.warning
}
