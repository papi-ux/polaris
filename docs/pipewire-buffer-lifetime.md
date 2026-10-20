# PipeWire buffer delivery ownership

A buffer pointer and the encoder import key identify storage. Neither identifies
which delivery owns the right to return that storage. A delayed consumer can
outlive a resend, buffer removal or allocation at the same address.

Each dequeued buffer acquires a monotonically increasing delivery generation.
Consumer completion, dropped frames, front replacement and format callbacks only
return the matching live generation. The admission check and enqueue run under
the PipeWire thread-loop lock. Removal and re-addition invalidate old deliveries;
format reinitialization and shutdown clear them without resetting the serial.
Encoder import keys remain stable for a storage allocation and change on removal
and re-addition. They are independent of delivery generations.

The implementation adapts the preserved preliminary PR3 / commit
`b6372309917bc1eb7df71c3b6a8f6321483fb199` for the current publication and shutdown
path. Issue6 remains the acceptance record, alongside the beta.2 capture work in
issue251.

Regressions use synthetic DMA-BUF metadata, actual capture publication, the real
isolated-consumer clone/return callback and a real thread-loop lock. A test-only
observer replaces the final PipeWire enqueue call; it does not replace return
admission. An in-process PipeWire core permits the real format callback to run without
connecting to a user daemon, compositor or producer. No GPU pixel read is made.

Generation checks prevent stale returns; they cannot repair pixels a producer
has already overwritten while an older GPU consumer reads them. Real producer
pool pressure, format renegotiation, GPU lifetime/overwrite recovery and physical
stream acceptance remain required. This change does not enable a capture route
or establish its performance or picture quality.
