export const BROWSER_STREAM_MAGIC = 0x31534250
export const BROWSER_STREAM_VERSION = 1
export const BROWSER_STREAM_HEADER_SIZE = 36

export const BrowserStreamKind = Object.freeze({
  video: 1,
  audio: 2,
})

export const BrowserStreamCodec = Object.freeze({
  h264: 1,
  opus: 2,
})

export const BrowserStreamFlags = Object.freeze({
  keyframe: 1,
})

export function encodeBrowserStreamControlFrame(message) {
  const encoder = new TextEncoder()
  const payload = encoder.encode(JSON.stringify(message))
  const out = new Uint8Array(4 + payload.byteLength)
  const view = new DataView(out.buffer)
  view.setUint32(0, payload.byteLength, false)
  out.set(payload, 4)
  return out
}

export function encodeBrowserStreamDatagram(envelope) {
  const payload = envelope.payload instanceof Uint8Array ? envelope.payload : new Uint8Array(envelope.payload || [])
  const out = new Uint8Array(BROWSER_STREAM_HEADER_SIZE + payload.byteLength)
  const view = new DataView(out.buffer)

  view.setUint32(0, BROWSER_STREAM_MAGIC, true)
  view.setUint8(4, BROWSER_STREAM_VERSION)
  view.setUint8(5, envelope.kind)
  view.setUint8(6, envelope.codec)
  view.setUint8(7, envelope.flags || 0)
  view.setUint32(8, envelope.sequence || 0, true)
  view.setBigUint64(12, BigInt(envelope.frameId || 0), true)
  view.setBigUint64(20, BigInt(envelope.timestampUs || 0), true)
  view.setUint16(28, envelope.chunkIndex || 0, true)
  view.setUint16(30, envelope.chunkCount || 1, true)
  view.setUint32(32, payload.byteLength, true)
  out.set(payload, BROWSER_STREAM_HEADER_SIZE)
  return out
}

export function decodeBrowserStreamDatagram(datagram) {
  const bytes = datagram instanceof Uint8Array ? datagram : new Uint8Array(datagram)
  if (bytes.byteLength < BROWSER_STREAM_HEADER_SIZE) {
    throw new Error('Browser Stream datagram is shorter than the media header')
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  if (view.getUint32(0, true) !== BROWSER_STREAM_MAGIC) {
    throw new Error('Browser Stream datagram has an invalid magic value')
  }
  if (view.getUint8(4) !== BROWSER_STREAM_VERSION) {
    throw new Error('Browser Stream datagram has an unsupported version')
  }

  const payloadLength = view.getUint32(32, true)
  if (payloadLength !== bytes.byteLength - BROWSER_STREAM_HEADER_SIZE) {
    throw new Error('Browser Stream datagram payload length does not match the header')
  }

  const chunkIndex = view.getUint16(28, true)
  const chunkCount = view.getUint16(30, true)
  if (chunkCount === 0 || chunkIndex >= chunkCount) {
    throw new Error('Browser Stream datagram has invalid chunk indexes')
  }

  return {
    kind: view.getUint8(5),
    codec: view.getUint8(6),
    flags: view.getUint8(7),
    sequence: view.getUint32(8, true),
    frameId: Number(view.getBigUint64(12, true)),
    timestampUs: Number(view.getBigUint64(20, true)),
    chunkIndex,
    chunkCount,
    payload: bytes.slice(BROWSER_STREAM_HEADER_SIZE),
  }
}

export function certHashHexToBytes(hex) {
  const clean = (hex || '').replace(/[^a-fA-F0-9]/g, '')
  if (clean.length % 2 !== 0) {
    throw new Error('Certificate hash must contain an even number of hex digits')
  }
  const bytes = new Uint8Array(clean.length / 2)
  for (let i = 0; i < clean.length; i += 2) {
    bytes[i / 2] = Number.parseInt(clean.slice(i, i + 2), 16)
  }
  return bytes
}

export function createBrowserStreamFrameAssembler(options = {}) {
  const maxFrameAgeMs = options.maxFrameAgeMs ?? 250
  const maxPendingFrames = options.maxPendingFrames ?? 8
  const pending = new Map()

  function pendingKey(datagram) {
    return `${datagram.kind}:${datagram.codec}:${datagram.frameId}`
  }

  function prune(nowMs, newestFrameId) {
    let droppedFrames = 0
    for (const [key, frame] of pending) {
      if (nowMs - frame.firstSeenMs > maxFrameAgeMs || frame.frameId + maxPendingFrames < newestFrameId) {
        pending.delete(key)
        droppedFrames += 1
      }
    }

    while (pending.size > maxPendingFrames) {
      const key = pending.keys().next().value
      pending.delete(key)
      droppedFrames += 1
    }
    return droppedFrames
  }

  function assemble(frame) {
    const payload = new Uint8Array(frame.receivedBytes)
    let offset = 0
    for (const chunk of frame.chunks) {
      payload.set(chunk, offset)
      offset += chunk.byteLength
    }
    return {
      kind: frame.kind,
      codec: frame.codec,
      flags: frame.flags,
      frameId: frame.frameId,
      timestampUs: frame.timestampUs,
      payload,
    }
  }

  return {
    get pendingCount() {
      return pending.size
    },
    reset() {
      pending.clear()
    },
    push(datagram, nowMs = performance.now()) {
      let droppedFrames = prune(nowMs, datagram.frameId)

      if (datagram.chunkCount === 1) {
        return {
          frame: {
            kind: datagram.kind,
            codec: datagram.codec,
            flags: datagram.flags,
            frameId: datagram.frameId,
            timestampUs: datagram.timestampUs,
            payload: datagram.payload,
          },
          droppedFrames,
        }
      }

      const key = pendingKey(datagram)
      let frame = pending.get(key)
      if (!frame) {
        frame = {
          kind: datagram.kind,
          codec: datagram.codec,
          flags: datagram.flags,
          frameId: datagram.frameId,
          timestampUs: datagram.timestampUs,
          chunkCount: datagram.chunkCount,
          chunks: new Array(datagram.chunkCount),
          receivedChunks: 0,
          receivedBytes: 0,
          firstSeenMs: nowMs,
        }
        pending.set(key, frame)
      }

      if (frame.chunkCount !== datagram.chunkCount || frame.chunks[datagram.chunkIndex]) {
        return { frame: null, droppedFrames }
      }

      frame.chunks[datagram.chunkIndex] = datagram.payload
      frame.receivedChunks += 1
      frame.receivedBytes += datagram.payload.byteLength

      if (frame.receivedChunks !== frame.chunkCount) {
        return { frame: null, droppedFrames }
      }

      pending.delete(key)
      return { frame: assemble(frame), droppedFrames }
    },
  }
}
