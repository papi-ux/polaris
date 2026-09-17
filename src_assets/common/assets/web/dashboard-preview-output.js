// Only the modes that stream an existing host connector can crop the preview to
// it. A private session renders on its own compositor output, so naming the
// saved connector there crops to an output that does not exist (#633).
const CONNECTOR_MODES = new Set(['headless_dongle', 'host_virtual_display'])

export function previewOutputForConfig(config = {}) {
  const connector = CONNECTOR_MODES.has(config.linux_stream_mode) ? config.linux_streaming_output : ''
  return connector || config.output_name || ''
}
