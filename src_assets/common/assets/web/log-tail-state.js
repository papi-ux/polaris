export const MAX_BROWSER_LOG_BYTES = 256 * 1024
export const MAX_BROWSER_LOG_LINES = 2000

const LOG_TAIL_ENDPOINT = './polaris/v1/diagnostics/logs/tail'

export function createLogTailState() {
  return {
    bytes: new Uint8Array(),
    text: '',
    startOffset: null,
    endOffset: null,
    generation: null,
    truncated: false,
    reset: true,
  }
}

function decodeBase64(value) {
  if (typeof value !== 'string' || value.length % 4 !== 0 ||
      !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) {
    throw new Error('Invalid Base64 log-tail content')
  }

  let binary
  try {
    binary = globalThis.atob(value)
  } catch {
    throw new Error('Invalid Base64 log-tail content')
  }

  const bytes = new Uint8Array(binary.length)
  for (let index = 0; index < binary.length; ++index) {
    bytes[index] = binary.charCodeAt(index)
  }
  return bytes
}

function validatePayload(payload) {
  if (!payload || payload.status !== true || payload.schema_version !== 1) {
    throw new Error('Unsupported log-tail schema')
  }
  if (payload.content_encoding !== 'base64') {
    throw new Error('Unsupported log-tail content encoding')
  }
  if (!Number.isSafeInteger(payload.content_bytes) || payload.content_bytes < 0) {
    throw new Error('Invalid log-tail byte count')
  }
  if (!Number.isSafeInteger(payload.start_offset) || !Number.isSafeInteger(payload.end_offset) ||
      payload.start_offset < 0 || payload.end_offset < payload.start_offset) {
    throw new Error('Invalid log-tail offsets')
  }
  if (!Number.isSafeInteger(payload.generation) || payload.generation < 0) {
    throw new Error('Invalid log-tail generation')
  }
  if (payload.end_offset - payload.start_offset !== payload.content_bytes) {
    throw new Error('Log-tail offsets do not match byte count')
  }
  if (typeof payload.truncated !== 'boolean' || typeof payload.reset !== 'boolean') {
    throw new Error('Invalid log-tail state flags')
  }

  const bytes = decodeBase64(payload.content)
  if (bytes.byteLength !== payload.content_bytes) {
    throw new Error('Decoded log-tail byte count does not match response')
  }
  return bytes
}

function concatenate(left, right) {
  const combined = new Uint8Array(left.byteLength + right.byteLength)
  combined.set(left, 0)
  combined.set(right, left.byteLength)
  return combined
}

function trimBytes(bytes, maxBytes, maxLines) {
  let removed = Math.max(0, bytes.byteLength - maxBytes)
  let bounded = bytes.subarray(removed)

  if (bounded.byteLength > 0) {
    let lineCount = bounded[bounded.byteLength - 1] === 0x0a ? 0 : 1
    for (const byte of bounded) {
      if (byte === 0x0a) ++lineCount
    }

    let linesToSkip = Math.max(0, lineCount - maxLines)
    let lineBytesToSkip = 0
    while (linesToSkip > 0 && lineBytesToSkip < bounded.byteLength) {
      if (bounded[lineBytesToSkip++] === 0x0a) --linesToSkip
    }
    removed += lineBytesToSkip
    bounded = bounded.subarray(lineBytesToSkip)
  }

  return {
    bytes: bounded.slice(),
    removed,
  }
}

function decodeText(bytes, charset) {
  const label = typeof charset === 'string' && charset ? charset : 'utf-8'
  return new TextDecoder(label).decode(bytes)
}

export function applyLogTailPayload(state, payload, {
  maxBytes = MAX_BROWSER_LOG_BYTES,
  maxLines = MAX_BROWSER_LOG_LINES,
} = {}) {
  if (!Number.isSafeInteger(maxBytes) || maxBytes < 1 ||
      !Number.isSafeInteger(maxLines) || maxLines < 1) {
    throw new Error('Browser log-tail bounds must be positive integers')
  }

  const incoming = validatePayload(payload)
  const previous = state || createLogTailState()
  const contiguous = previous.endOffset !== null && payload.start_offset === previous.endOffset &&
    previous.generation === payload.generation
  const append = payload.reset === false && contiguous && previous.startOffset !== null
  const combined = append ? concatenate(previous.bytes, incoming) : incoming
  const initialStartOffset = append ? previous.startOffset : payload.start_offset
  const bounded = trimBytes(combined, maxBytes, maxLines)
  const boundedStartOffset = initialStartOffset + bounded.removed

  return {
    bytes: bounded.bytes,
    text: decodeText(bounded.bytes, payload.charset),
    startOffset: boundedStartOffset,
    endOffset: payload.end_offset,
    generation: payload.generation,
    truncated: payload.truncated || boundedStartOffset > 0,
    reset: payload.reset || !append,
  }
}

export async function fetchLogTail(state = createLogTailState(), {
  fetchImpl = globalThis.fetch,
  maxBytes = MAX_BROWSER_LOG_BYTES,
  maxLines = MAX_BROWSER_LOG_LINES,
  endpoint = LOG_TAIL_ENDPOINT,
} = {}) {
  const query = new URLSearchParams({
    max_bytes: String(maxBytes),
    max_lines: String(maxLines),
  })
  if (Number.isSafeInteger(state.endOffset) && state.endOffset >= 0 &&
      Number.isSafeInteger(state.generation) && state.generation >= 0) {
    query.set('after', String(state.endOffset))
    query.set('after_generation', String(state.generation))
  }

  const response = await fetchImpl(`${endpoint}?${query}`, {
    credentials: 'include',
    cache: 'no-store',
  })
  if (!response.ok) {
    throw new Error(`Log-tail request failed with HTTP ${response.status || 'error'}`)
  }

  return applyLogTailPayload(state, await response.json(), { maxBytes, maxLines })
}
