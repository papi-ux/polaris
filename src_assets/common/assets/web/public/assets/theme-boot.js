// Applies the stored theme before first paint so a non-default theme never
// flashes the default palette. Loaded as a classic blocking script from
// template_header.html because the production CSP is script-src 'self'
// (no inline scripts), and served from /assets/ because that is a route the
// Polaris host actually exposes (the web root is not served).
//
// This file intentionally mirrors theme.js: the storage key, the id list,
// and the default-means-no-attribute rule. theme.test.js pins the two files
// together, so change them in lockstep.
(function () {
  var KNOWN_THEMES = ['polaris', 'portable-chrome', 'oled', 'miami', 'high-contrast']
  try {
    var theme = localStorage.getItem('polaris-theme')
    if (theme && theme !== 'polaris' && KNOWN_THEMES.indexOf(theme) !== -1) {
      document.documentElement.setAttribute('data-theme', theme)
    }
  } catch (e) {
    // Storage unavailable: stay on the default theme.
  }
})()
