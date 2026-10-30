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
//   GET  /api/preview.jpg                     live composite snapshot
//   GET  /api/preview.mjpg                    live composite STREAM (~14 fps,
//                                             multipart/x-mixed-replace)
//   GET  /api/displays                        monitors + active state + canvas rects (M43)
//   POST /api/layout?name=<layout>            presentation|two-cubes|single-cube|wall
//   POST /api/pause?on=0|1
//   POST /api/config?max_visible=N&rotate_seconds=S
//   POST /api/stream?slot=N&action=<action>   hide|show|kick|pin|unpin|
//                                             spotlight|unspotlight|front
//   POST /api/camera?on=0|1                   gate the virtual-camera feed (M43)
//   POST /api/display?id=N&on=0|1             attach/detach a monitor (M43)
#pragma once


#include <atomic>
#include <condition_variable>
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

// M43: one monitor as the moderator sees it. The uv rect is the display's
// canvas region while active - the console crops the composite preview with
// it, which IS what the display shows (same off-center projection).
struct ControlDisplayInfo {
    int id = 0;
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    bool primary = false;
    bool active = false;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

struct ControlSnapshot {
    bool avConfigured=false,avActive=false,avAudioMaster=false,avMuted=false,avPaused=false;
    uint64_t avImportFailures=0;
    uint64_t avResets=0,avPresented=0,avHeld=0,avLate=0,avInvalid=0,avSkipped=0,avQueued=0,avMaxQueued=0;
    int64_t avErrorNs=0, avAudioArrivalDriftNs=0;
    bool avAudioMetricsValid=false;
    uint64_t avGeneration=0,avAudioBlocks=0,avAudioSamples=0,avAudioQueueNs=0,avAudioPeakQueueNs=0;
    uint64_t avAudioQueueBuffers=0,avAudioPeakQueueBuffers=0,avAudioQueueEmpty=0,avAudioQueueFull=0,avAudioLate=0;
    std::string layout;
    bool paused = false;
    uint32_t maxVisible = 4;
    double rotateSeconds = 8.0;
    int spotlightSlot = -1;
    bool presenting = false;
    bool cameraAvailable = true;
    bool previewAvailable = true;
    uint32_t maxSlots = 12;
    bool cameraOn = true;         // M43
    bool displaysAvailable = false; // M43: false = no window system (true server)
    std::vector<ControlStreamInfo> streams;
    std::vector<ControlDisplayInfo> displays; // M43
};

struct ControlCommand {
    enum class Type {
        AudioSelect, // str=protected session directory; switch both tracks atomically
        AudioMute, // i=0/1; one selected negotiated session (slot0)
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
        Camera,     // i = 0/1 - gate the vcam feed (M43)
        Display,    // slot = display id, i = 0/1 attach/detach (M43)
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
    // readback is older than the refresh interval - the render loop's cue to
    // refresh. Interval: ~400 ms for snapshot polling; ~70 ms (≈14 fps) while
    // at least one MJPEG stream is attached.
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
    // MJPEG streaming: publishPreview bumps the sequence and wakes every
    // stream handler; each handler encodes + writes one part per frame. Each
    // stream occupies a civetweb worker thread for its lifetime, hence the
    // hard cap (and num_threads sized to leave workers free for commands).
    uint64_t previewSeq_ = 0;
    std::condition_variable previewCv_;
    std::atomic<int> previewStreamers_{0};
    std::atomic<bool> stopping_{false}; // unparks stream handlers before mg_stop
    static constexpr int kMaxPreviewStreams = 3;
};

} // namespace vkr
