// M38: attaching a receiver to the renderer's framelink input slots.
//
// The renderer pre-creates `vkrender.in.0 .. .in.11` and receivers take the
// first free one. This is the whole of the client side of the migration, and
// it lives in one header because every receiver needs exactly it - a copy per
// receiver is how the four of them drifted apart under the old protocol.
//
// Why walking rather than asking: a framelink producer cannot create a
// channel, deliberately (the ring belongs to the consumer, which is what
// Android's BufferQueue forces). So something must create channels before any
// producer exists, and slot channels are that something. FL_BUSY - as distinct
// from FL_NOT_FOUND - is what makes the walk possible: "taken" and "no such
// channel" have to be told apart or an exhausted set looks like a
// misconfigured name.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <cstdio>
#include <string>

#include "framelink.h"

namespace vkr {

// Takes the first free slot, or returns nullptr. Retries the whole sweep until
// `timeoutMs` elapses, because the renderer may simply not be up yet - the
// normal case for a receiver started by the appliance at boot.
//
// `width`/`height` are what this producer intends to send. The renderer's ring
// starts at a placeholder size and reallocates to match, so callers get the
// geometry they asked for - but flQuery afterwards is still the truth, since
// the consumer may decline.
inline flChannel* flSlotAttach(const char* pipeName, uint32_t width, uint32_t height,
                               uint32_t slots, uint32_t timeoutMs, std::string* nameOut) {
    const uint32_t deadline = GetTickCount() + timeoutMs;
    for (;;) {
        bool anyExisted = false;
        for (uint32_t i = 0; i < slots; ++i) {
            const std::string name = std::string(pipeName) + ".in." + std::to_string(i);
            flChannel* ch = nullptr;
            const flResult r = flOpenProducer(name.c_str(), &ch);
            if (r == FL_BUSY) {
                anyExisted = true; // occupied, not missing - keep walking
                continue;
            }
            if (r == FL_NOT_FOUND) continue;
            if (r != FL_OK) {
                fprintf(stderr, "slot: '%s' -> %s\n", name.c_str(), flResultString(r));
                continue;
            }
            anyExisted = true;
            if (width && height) flRequestGeometry(ch, width, height, FL_FORMAT_BGRA8);
            if (nameOut) *nameOut = name;
            return ch;
        }
        if (GetTickCount() >= deadline) {
            fprintf(stderr, "slot: no free input slot on '%s' (%s)\n", pipeName,
                    anyExisted ? "all occupied" : "renderer not running");
            return nullptr;
        }
        Sleep(200);
    }
}

} // namespace vkr

#endif // _WIN32
