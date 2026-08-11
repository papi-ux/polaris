// Applies the stored theme before first paint so a non-default theme never
// flashes the default palette. Loaded as a classic blocking script from
// template_header.html because the production CSP is script-src 'self'
// (no inline scripts). Mirrors theme.js: default theme = no attribute.
(function () {
  try {
    var theme = localStorage.getItem('polaris-theme')
    if (theme && theme !== 'polaris') {
      document.documentElement.setAttribute('data-theme', theme)
    }
  } catch (e) {
    // Storage unavailable: stay on the default theme.
  }
})()
