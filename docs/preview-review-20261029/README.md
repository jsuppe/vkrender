# Independent preview review fixes

## Headless memory dependency

The new pre-present readback invoked `vkrReadImage` directly after rendering.
Its GENERAL-layout path did not emit any memory barrier: queue submission order
alone does not make previous color/layout writes visible to the transfer read.

A 12-preview headless test on RTX 3090, with Khronos synchronization validation
forced on, reproduced repeated `SYNC-HAZARD-READ-AFTER-WRITE` diagnostics. The
same test passed without validation errors after adding explicit transfer-read
and restore dependencies for all source layouts, including unchanged GENERAL.
The PRESENT path continues to transition through TRANSFER_SRC and restore its
original layout. No additional queue idle was introduced; this helper already
synchronously waits for requested preview readback, not every frame.

Run `python3 test/preview_sync_validation.py BUILD LOG`. This is an opt-in GPU
integration test, not part of CPU-only CI. Hosts without the Khronos layer exit77.
The before/after raw logs are adjacent. These checks validate headless GENERAL;
physical windowed PRESENT was reviewed, not separately exercised here.

## Concurrent MJPEG admission

A load-then-increment stream cap allowed simultaneous request handlers to exceed
three streams and potentially consume all eight control-server workers. Atomic
reservation with rollback now enforces admission; scope-based release covers
handler exit. The CPU HTTP regression launches eight clients simultaneously,
asserts exactly three streams and five503 responses, checks control requests
remain responsive, disconnects all streams, and repeats eight batches to check
lease reclamation. This is not a GPU/video performance test.

## Partial multipart socket writes

Every multipart header, JPEG and separator write now requires the full requested
byte count; a short write ends the handler and releases admission. A Linux-only
link-wrapper regression transmits half a real JPEG and returns the actual short
count. Eight sequential requests must receive exactly the truncated frame then
EOF, remain able to query status, and reclaim their stream leases. The old code
fails this test by continuing to send more multipart fragments; fixed code and
both existing HTTP/admission tests pass. Before/after CTest logs are included.
