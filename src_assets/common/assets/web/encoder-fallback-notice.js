// The encoder fallback the console names in plain words before a stream (#490, discussion #813).
// NVENC was the preferred encoder, the host's own encoder probe could not start it, and the
// selection that probe completed is the CPU encoder. Only those observed encoder_selection facts
// count. Nothing here judges a GPU by its model, its driver version or a CUDA architecture floor,
// because the targets a package carries depend on the compiler it was built with. A host that has
// not probed yet reports no selected encoder and gets no notice, and a fallback to another hardware
// encoder keeps its own reason.
import { SESSION_ENCODER_SELECTION } from './generated/polarisV1Contract.js'

/**
 * @param {object|null|undefined} selection The encoder_selection object of Doctor's advanced evidence.
 * @param {(key: string) => string} t Translates a message key.
 * @returns {{title: string, detail: string, action: string, evidence: string}|null}
 */
export function nvencCpuFallbackNotice(selection, t) {
  if (!selection || typeof selection !== 'object') return null
  if (selection[SESSION_ENCODER_SELECTION.FALLBACK_USED] !== true ||
    selection[SESSION_ENCODER_SELECTION.PREFERRED_ENCODER] !== 'nvenc' ||
    selection[SESSION_ENCODER_SELECTION.SELECTED_ENCODER] !== 'software') return null
  const reason = selection[SESSION_ENCODER_SELECTION.REASON]
  return {
    title: t('troubleshooting.nvenc_cpu_fallback_title'),
    detail: t('troubleshooting.nvenc_cpu_fallback_detail'),
    action: t('troubleshooting.nvenc_cpu_fallback_action'),
    evidence: typeof reason === 'string' ? reason : '',
  }
}
