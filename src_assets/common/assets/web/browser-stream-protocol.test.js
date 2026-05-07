import {
  BrowserStreamCodec,
  BrowserStreamFlags,
  BrowserStreamKind,
  certHashHexToBytes,
  createBrowserStreamFrameAssembler,
  decodeBrowserStreamDatagram,
  encodeBrowserStreamControlFrame,
  encodeBrowserStreamDatagram,
} from './browser-stream-protocol.js'

describe('Browser Stream protocol', () => {
  it('round trips a chunked H.264 datagram', () => {
    const payload = new Uint8Array([1, 2, 3, 4])
    const encoded = encodeBrowserStreamDatagram({
      kind: BrowserStreamKind.video,
      codec: BrowserStreamCodec.h264,
      flags: BrowserStreamFlags.keyframe,
      sequence: 42,
      frameId: 7,
      timestampUs: 123456,
      chunkIndex: 1,
      chunkCount: 3,
      payload,
    })

    const decoded = decodeBrowserStreamDatagram(encoded)

    expect(decoded.kind).toBe(BrowserStreamKind.video)
    expect(decoded.codec).toBe(BrowserStreamCodec.h264)
    expect(decoded.flags).toBe(BrowserStreamFlags.keyframe)
    expect(decoded.sequence).toBe(42)
    expect(decoded.frameId).toBe(7)
    expect(decoded.timestampUs).toBe(123456)
    expect(decoded.chunkIndex).toBe(1)
    expect(decoded.chunkCount).toBe(3)
    expect([...decoded.payload]).toEqual([...payload])
  })

  it('rejects invalid chunk indexes', () => {
    const encoded = encodeBrowserStreamDatagram({
      kind: BrowserStreamKind.audio,
      codec: BrowserStreamCodec.opus,
      chunkIndex: 2,
      chunkCount: 2,
      payload: new Uint8Array([1]),
    })

    expect(() => decodeBrowserStreamDatagram(encoded)).toThrow(/invalid chunk/)
  })

  it('converts certificate hash hex to bytes', () => {
    expect([...certHashHexToBytes('0a ff 10')]).toEqual([10, 255, 16])
  })

  it('encodes reliable control frames with a big-endian length prefix', () => {
    const encoded = encodeBrowserStreamControlFrame({
      type: 'input_events',
      events: [{ type: 'key', action: 'down', key_code: 87 }],
    })
    const view = new DataView(encoded.buffer, encoded.byteOffset, encoded.byteLength)
    const payloadLength = view.getUint32(0, false)
    const payload = new TextDecoder().decode(encoded.slice(4))

    expect(payloadLength).toBe(encoded.byteLength - 4)
    expect(JSON.parse(payload)).toEqual({
      type: 'input_events',
      events: [{ type: 'key', action: 'down', key_code: 87 }],
    })
  })

  it('reassembles chunked frames out of order', () => {
    const assembler = createBrowserStreamFrameAssembler()

    const first = assembler.push({
      kind: BrowserStreamKind.video,
      codec: BrowserStreamCodec.h264,
      flags: BrowserStreamFlags.keyframe,
      frameId: 12,
      timestampUs: 1000,
      chunkIndex: 1,
      chunkCount: 2,
      payload: new Uint8Array([3, 4]),
    }, 0)
    const second = assembler.push({
      kind: BrowserStreamKind.video,
      codec: BrowserStreamCodec.h264,
      flags: BrowserStreamFlags.keyframe,
      frameId: 12,
      timestampUs: 1000,
      chunkIndex: 0,
      chunkCount: 2,
      payload: new Uint8Array([1, 2]),
    }, 1)

    expect(first.frame).toBeNull()
    expect(second.droppedFrames).toBe(0)
    expect([...second.frame.payload]).toEqual([1, 2, 3, 4])
  })

  it('drops incomplete frames that arrive too late', () => {
    const assembler = createBrowserStreamFrameAssembler({ maxFrameAgeMs: 10 })

    assembler.push({
      kind: BrowserStreamKind.video,
      codec: BrowserStreamCodec.h264,
      flags: 0,
      frameId: 1,
      timestampUs: 1000,
      chunkIndex: 0,
      chunkCount: 2,
      payload: new Uint8Array([1]),
    }, 0)
    const result = assembler.push({
      kind: BrowserStreamKind.video,
      codec: BrowserStreamCodec.h264,
      flags: 0,
      frameId: 2,
      timestampUs: 2000,
      chunkIndex: 0,
      chunkCount: 1,
      payload: new Uint8Array([2]),
    }, 20)

    expect(result.droppedFrames).toBe(1)
    expect([...result.frame.payload]).toEqual([2])
    expect(assembler.pendingCount).toBe(0)
  })
})
