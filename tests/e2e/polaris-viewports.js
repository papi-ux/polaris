export const POLARIS_VIEWPORTS = [
  {
    name: 'desktop-cockpit',
    label: 'Desktop Linux host operator',
    width: 1440,
    height: 900,
  },
  {
    name: 'desktop-compact',
    label: 'Desktop compact',
    width: 1280,
    height: 720,
  },
  {
    name: 'ultrawide-1080p',
    label: 'Ultrawide 1080p',
    width: 1920,
    height: 1080,
  },
  {
    name: 'phone-portrait',
    label: 'Phone portrait',
    width: 390,
    height: 844,
  },
  {
    name: 'phone-landscape',
    label: 'Phone landscape',
    width: 844,
    height: 390,
  },
  {
    name: 'tablet-portrait',
    label: 'Tablet portrait',
    width: 768,
    height: 1024,
  },
  {
    name: 'tablet-landscape',
    label: 'Tablet landscape',
    width: 1024,
    height: 768,
  },
  {
    name: 'handheld-landscape',
    label: 'Handheld landscape',
    width: 1280,
    height: 720,
  },
]

export function getViewportByName(name) {
  return POLARIS_VIEWPORTS.find((viewport) => viewport.name === name) || null
}
