// M42: the renderer's moderation API - the control plane the architecture doc
// said would be the next real component (docs/ARCHITECTURE.md: "a local HTTP
// control socket, versioned, with the frame path untouched").
//
// An HTTP server on 127.0.0.1 only. It is deliberately NOT reachable off-box:
// vkr_web reverse-proxies /api/* to it behind moderator authentication, the
// same pattern that carries WHIP through the https origin. Localhost binding
// is the renderer's entire security model; authentication is the web tier's
// job.
//
// Threading contract, which is the whole design: HTTP threads never touch
// Vulkan or the scene. They enqueue Commands and read published Snapshots;
// the render loop drains the queue and publishes state once per frame. The
// live preview follows the same rule - the render loop performs the GPU
// readback (only when someone asked recently), HTTP threads JPEG-encode from
// the latest raw copy.
//
//   GET  /api/status                          appliance + moderation state
//   GET  /api/streams                         the roster
//   GET  /api/preview.jpg                     live composite (~2 fps refresh)
//   POST /api/layout?name=<layout>            presentation|two-cubes|single-cube|wall
//   POST /api/pause?on=0|1
//   POST /api/config?max_visible=N&rotate_seconds=S
//   POST /api/stream?slot=N&action=<action>   hide|show|kick|pin|unpin|
//                                             spotlight|unspotlight|front
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct mg_context;
struct mg_connection;

namespace vkr {

struct ControlStreamInfo {
    uint32_t slot = 0; // in.N - the moderator-facing handle
    uint32_t width = 0, height = 0;
    float fps = 0.0f;
    int64_t ageMs = -1; // since the last adopted frame; -1 = never
    bool connected = false;
    bool hidden = false;
    bool pinned = false;
    bool spotlit = false;
    bool visible = false; // actually on screen this frame
};

struct ControlSnapshot {
    std::string layout;
    bool paused = false;
    uint32_t maxVisible = 4;
    double rotateSeconds = 8.0;
    int spotlightSlot = -1;
    bool presenting = false;
    std::vector<ControlStreamInfo> streams;
};

struct ControlCommand {
    enum class Type {
        Layout,     // str = layout name
        Pause,      // i = 0/1
        Config,     // i = maxVisible (-1 = unchanged), d = rotateSeconds (<0 = unchanged)
        Hide,       // slot
        Show,       // slot
        Kick,       // slot
        Pin,        // slot
        Unpin,      // slot
        Spotlight,  // slot
        Unspotlight,
        Front,      // slot -> head of the order
    };
    Type type;
    int slot = -1;
    int i = 0;
    double d = 0.0;
    std::string str;
};

class ControlApi {
  public:
    bool start(int port);
    void stop();

    // ---- render-loop side --------------------------------------------------
    std::vector<ControlCommand> drainCommands();
    void publish(const ControlSnapshot& snap);
    // True when a preview was requested within the last 3 s AND the last
    // readback is older than ~400 ms - the render loop's cue to refresh.
    bool previewDue() const;
    void publishPreview(const uint8_t* bgra, uint32_t w, uint32_t h);

    // ---- HTTP side (internal) ----------------------------------------------
    int handleRequest(mg_connection* conn);

  private:
    mg_context* ctx_ = nullptr;

    mutable std::mutex m_;
    std::vector<ControlCommand> pending_;
    ControlSnapshot snap_;
    std::vector<uint8_t> previewBgra_;
    uint32_t previewW_ = 0, previewH_ = 0;
    mutable int64_t previewRequestedNs_ = 0;
    int64_t previewRenderedNs_ = 0;
};

} // namespace vkr

#endif // _WIN32
