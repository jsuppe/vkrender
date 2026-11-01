// M42: the renderer's moderation API. See control_api.h for the contract.


#include "control_api.h"


#include <cctype>
#include <cstdlib>
#include <cmath>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "civetweb.h"
#include "log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace vkr {
namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Query-string parameter, decoded. civetweb's helper wants the raw query.
std::string param(mg_connection* conn, const char* name) {
    const mg_request_info* ri = mg_get_request_info(conn);
    if (!ri->query_string) return "";
    char buf[256] = {};
    if (mg_get_var(ri->query_string, strlen(ri->query_string), name, buf, sizeof(buf)) < 0)
        return "";
    return buf;
}

void sendJson(mg_connection* conn, const std::string& body) {
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
              "Cache-Control: no-store\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
              (unsigned)body.size());
    mg_write(conn, body.data(), body.size());
}

int trampoline(mg_connection* conn, void* cbdata) {
    return static_cast<ControlApi*>(cbdata)->handleRequest(conn);
}

// BGRA -> RGB -> JPEG, on the calling (HTTP) thread - encoding must never
// stall the render loop. Shared by the snapshot and the MJPEG stream.
std::vector<uint8_t> encodeJpeg(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }
    std::vector<uint8_t> jpg;
    jpg.reserve(128 * 1024);
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            auto* v = static_cast<std::vector<uint8_t>*>(ctx);
            v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &jpg, (int)w, (int)h, 3, rgb.data(), 70);
    return jpg;
}

} // namespace

bool ControlApi::start(int port) {
    if(port<1 || port>65535)return false;
    stopping_.store(false);
    char ports[64];
    // Loopback only - reachability from elsewhere is vkr_web's (authenticated)
    // business, never this server's.
    snprintf(ports, sizeof(ports), "127.0.0.1:%d", port);
    mg_init_library(0);
    // Each MJPEG stream parks on a worker thread for its whole lifetime;
    // kMaxPreviewStreams of them must still leave workers for commands.
    // request_timeout_ms also bounds a single blocking send to a wedged
    // client - the stream handler's last line of defence.
    const char* options[] = {"listening_ports", ports, "num_threads", "8",
                             "request_timeout_ms", "10000", nullptr};
    ctx_ = mg_start(nullptr, nullptr, options);
    if (!ctx_) {
        VKR_ERR("control: cannot listen on %s", ports);
        return false;
    }
    mg_set_request_handler(ctx_, "/api/", trampoline, this);
    VKR_LOG("control: moderation API on http://%s/api/", ports);
    return true;
}

void ControlApi::stop() {
    // Wake parked stream handlers first: mg_stop waits for every request
    // handler to return, and a stream sitting in its cv wait would hold the
    // shutdown for a timeout cycle.
    stopping_.store(true);
    previewCv_.notify_all();
    if (ctx_) mg_stop(ctx_);
    ctx_ = nullptr;
}

std::vector<ControlCommand> ControlApi::drainCommands() {
    std::lock_guard<std::mutex> hold(m_);
    std::vector<ControlCommand> out;
    out.swap(pending_);
    return out;
}

void ControlApi::publish(const ControlSnapshot& snap) {
    std::lock_guard<std::mutex> hold(m_);
    snap_ = snap;
}

bool ControlApi::previewDue() const {
    std::lock_guard<std::mutex> hold(m_);
    const int64_t now = nowNs();
    // ~14 fps while someone is streaming, ~2.5 fps for snapshot polling.
    const int64_t interval = previewStreamers_.load() > 0 ? 70000000ll : 400000000ll;
    return now - previewRequestedNs_ < 3000000000ll && now - previewRenderedNs_ > interval;
}

void ControlApi::publishPreview(const uint8_t* bgra, uint32_t w, uint32_t h) {
    {
        std::lock_guard<std::mutex> hold(m_);
        previewBgra_.assign(bgra, bgra + (size_t)w * h * 4);
        previewW_ = w;
        previewH_ = h;
        previewRenderedNs_ = nowNs();
        ++previewSeq_;
    }
    previewCv_.notify_all();
}

int ControlApi::handleRequest(mg_connection* conn) {
    const mg_request_info* ri = mg_get_request_info(conn);
    const std::string uri = ri->local_uri ? ri->local_uri : "";
    const bool post = ri->request_method && !strcmp(ri->request_method, "POST");

    if (uri == "/api/capabilities") {
        ControlSnapshot capability;
        {std::lock_guard<std::mutex> hold(m_);capability=snap_;}
        sendJson(conn, std::string("{\"camera\":")+(capability.cameraAvailable?"true":"false")+
            ",\"preview\":"+(capability.previewAvailable?"true":"false")+
            ",\"displays\":"+(capability.displaysAvailable?"true":"false")+",\"maxSlots\":"+std::to_string(capability.maxSlots)+"}");
        return 200;
    }
    if (uri == "/api/status" || uri == "/api/streams" || uri == "/api/displays") {
        ControlSnapshot s;
        {
            std::lock_guard<std::mutex> hold(m_);
            s = snap_;
        }
        std::string j;
        char buf[384];
        if (uri == "/api/status") {
            snprintf(buf, sizeof(buf),
                     "{\"layout\":\"%s\",\"paused\":%s,\"maxVisible\":%u,"
                     "\"rotateSeconds\":%.1f,\"spotlightSlot\":%d,\"presenting\":%s,"
                     "\"cameraOn\":%s,\"displaysAvailable\":%s,\"activeDisplays\":%u,"
                     "\"streams\":%u}",
                     s.layout.c_str(), s.paused ? "true" : "false", s.maxVisible,
                     s.rotateSeconds, s.spotlightSlot, s.presenting ? "true" : "false",
                     s.cameraOn ? "true" : "false",
                     s.displaysAvailable ? "true" : "false",
                     [&] {
                         unsigned n = 0;
                         for (const auto& d : s.displays) n += d.active ? 1 : 0;
                         return n;
                     }(),
                     (unsigned)s.streams.size());
            j = buf;
            j.pop_back();
            j += ",\"presentation_submission\":{\"calls\":"+std::to_string(s.presentCalls)+
                ",\"errors\":"+std::to_string(s.presentErrors)+",\"first_monotonic_ns\":"+std::to_string(s.presentFirstNs)+
                ",\"last_monotonic_ns\":"+std::to_string(s.presentLastNs)+",\"physical_scanout_measured\":false}";
            j += ",\"av\":{\"active\":"+std::string(s.avActive?"true":"false")+
                ",\"audio_master\":"+(s.avAudioMaster?"true":"false")+
                ",\"muted\":"+(s.avMuted?"true":"false")+
                ",\"paused\":"+(s.avPaused?"true":"false")+
                ",\"session_resets\":"+std::to_string(s.avResets)+
                ",\"due_decisions\":"+std::to_string(s.avPresented)+
                ",\"sequence_gaps\":"+std::to_string(s.avSkipped)+
                ",\"queue_depth\":"+std::to_string(s.avQueued)+
                ",\"max_queue_depth\":"+std::to_string(s.avMaxQueued)+
                ",\"hold_checks\":"+std::to_string(s.avHeld)+
                ",\"late_drops\":"+std::to_string(s.avLate)+
                ",\"import_failures\":"+std::to_string(s.avImportFailures)+
                ",\"invalid_pts\":"+std::to_string(s.avInvalid)+
                ",\"schedule_error_ns\":"+std::to_string(s.avErrorNs)+
                ",\"session_generation\":"+std::to_string(s.avGeneration)+
                ",\"estimated_av_offset_ns\":null,\"device_underruns\":null,\"clock_drift_ppm\":null"+
                ",\"audio_metrics_valid\":"+(s.avAudioMetricsValid?"true":"false")+
                ",\"audio_blocks\":"+std::to_string(s.avAudioBlocks)+
                ",\"audio_samples\":"+std::to_string(s.avAudioSamples)+
                ",\"audio_queue_buffers\":"+std::to_string(s.avAudioQueueBuffers)+
                ",\"audio_queue_ns\":"+std::to_string(s.avAudioQueueNs)+
                ",\"audio_peak_queue_buffers\":"+std::to_string(s.avAudioPeakQueueBuffers)+
                ",\"audio_peak_queue_ns\":"+std::to_string(s.avAudioPeakQueueNs)+
                ",\"audio_queue_empty_events\":"+std::to_string(s.avAudioQueueEmpty)+
                ",\"audio_queue_full_events\":"+std::to_string(s.avAudioQueueFull)+
                ",\"late_audio_blocks\":"+std::to_string(s.avAudioLate)+
                ",\"audio_arrival_drift_ns\":"+std::to_string(s.avAudioArrivalDriftNs)+"}}";
        } else if (uri == "/api/displays") {
            j = "[";
            for (size_t i = 0; i < s.displays.size(); ++i) {
                const ControlDisplayInfo& d = s.displays[i];
                // JSON-safe monitor name: keep it boring (alnum + space).
                std::string name;
                for (char ch : d.name)
                    if (isalnum((unsigned char)ch) || ch == ' ' || ch == '-' || ch == '(' ||
                        ch == ')')
                        name += ch;
                snprintf(buf, sizeof(buf),
                         "%s{\"id\":%d,\"name\":\"%s\",\"x\":%d,\"y\":%d,\"width\":%d,"
                         "\"height\":%d,\"primary\":%s,\"active\":%s,"
                         "\"u0\":%.4f,\"v0\":%.4f,\"u1\":%.4f,\"v1\":%.4f}",
                         i ? "," : "", d.id, name.c_str(), d.x, d.y, d.width, d.height,
                         d.primary ? "true" : "false", d.active ? "true" : "false", d.u0,
                         d.v0, d.u1, d.v1);
                j += buf;
            }
            j += "]";
        } else {
            j = "[";
            for (size_t i = 0; i < s.streams.size(); ++i) {
                const ControlStreamInfo& st = s.streams[i];
                snprintf(buf, sizeof(buf),
                         "%s{\"slot\":%u,\"width\":%u,\"height\":%u,\"fps\":%.1f,"
                         "\"ageMs\":%lld,\"connected\":%s,\"hidden\":%s,\"pinned\":%s,"
                         "\"spotlit\":%s,\"visible\":%s}",
                         i ? "," : "", st.slot, st.width, st.height, st.fps,
                         (long long)st.ageMs, st.connected ? "true" : "false",
                         st.hidden ? "true" : "false", st.pinned ? "true" : "false",
                         st.spotlit ? "true" : "false", st.visible ? "true" : "false");
                j += buf;
            }
            j += "]";
        }
        sendJson(conn, j);
        return 200;
    }

    if (uri == "/api/preview.jpg" || uri == "/api/preview.mjpg") {
        bool available;
        {std::lock_guard<std::mutex> hold(m_);available=snap_.previewAvailable;}
        if(!available){mg_send_http_error(conn,501,"preview unavailable");return 501;}
    }
    if (uri == "/api/preview.jpg") {
        std::vector<uint8_t> bgra;
        uint32_t w = 0, h = 0;
        {
            std::lock_guard<std::mutex> hold(m_);
            previewRequestedNs_ = nowNs(); // the render loop's cue to refresh
            bgra = previewBgra_;
            w = previewW_;
            h = previewH_;
        }
        if (bgra.empty()) {
            mg_send_http_error(conn, 503, "no preview yet - ask again shortly");
            return 503;
        }
        const std::vector<uint8_t> jpg = encodeJpeg(bgra, w, h);
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                  "Cache-Control: no-store\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                  (unsigned)jpg.size());
        mg_write(conn, jpg.data(), jpg.size());
        return 200;
    }

    if (uri == "/api/preview.mjpg") {
        // The live stream: this handler PARKS on its worker thread, encoding
        // and writing one part per published frame until the client goes
        // away. Each part carries Content-Length so a reader never has to
        // scan for the boundary.
        // Reserve atomically: load-then-increment lets concurrent handlers all
        // pass the cap and exhaust the worker pool needed for control requests.
        if (previewStreamers_.fetch_add(1) >= kMaxPreviewStreams) {
            --previewStreamers_;
            mg_send_http_error(conn, 503, "too many preview streams");
            return 503;
        }
        struct Admission {
            std::atomic<int>& count;
            ~Admission() { --count; }
        } admission{previewStreamers_};
        static constexpr char header[] =
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: multipart/x-mixed-replace; boundary=vkrframe\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
        if (mg_write(conn, header, sizeof(header)-1) != (int)(sizeof(header)-1)) return 200;
        uint64_t seen = 0;
        bool alive = true;
        while (alive && !stopping_.load()) {
            std::vector<uint8_t> bgra;
            uint32_t w = 0, h = 0;
            {
                std::unique_lock<std::mutex> hold(m_);
                previewRequestedNs_ = nowNs(); // keep the readback flowing
                previewCv_.wait_for(hold, std::chrono::milliseconds(500),
                                    [&] { return previewSeq_ != seen || stopping_.load(); });
                if (previewSeq_ == seen) {
                    // Timeout with no new frame. A client that closed (FIN)
                    // while we were parked is only discoverable by touching
                    // the socket - two zombie handlers once filled the
                    // stream cap exactly this way. A stray CRLF between
                    // multipart parts is harmless to readers.
                    hold.unlock();
                    alive = mg_write(conn, "\r\n", 2) == 2;
                    continue;
                }
                seen = previewSeq_;
                bgra = previewBgra_;
                w = previewW_;
                h = previewH_;
            }
            if (bgra.empty()) continue;
            const std::vector<uint8_t> jpg = encodeJpeg(bgra, w, h);
            char part[128];
            const int pn = snprintf(part, sizeof(part),
                                    "--vkrframe\r\nContent-Type: image/jpeg\r\n"
                                    "Content-Length: %u\r\n\r\n",
                                    (unsigned)jpg.size());
            const int64_t t0 = nowNs();
            alive = mg_write(conn, part, (size_t)pn) == pn &&
                    mg_write(conn, jpg.data(), jpg.size()) == (int)jpg.size() &&
                    mg_write(conn, "\r\n", 2) == 2;
            // A frozen client (a background tab Chrome stopped scheduling)
            // trickle-ACKs against a full TCP window: writes "succeed" after
            // blocking for ages and the handler wedges holding its slot.
            // Slower than a second per 40 KB frame means nobody is watching.
            if (alive && nowNs() - t0 > 1000000000ll) alive = false;
        }
        return 200;
    }

    if (!post) {
        mg_send_http_error(conn, 405, "POST required");
        return 405;
    }

    ControlSnapshot current;
    {std::lock_guard<std::mutex> hold(m_);current=snap_;}
    auto integer=[](const std::string& value,int& out){
        if(value.empty())return false;char* end=nullptr;long n=std::strtol(value.c_str(),&end,10);
        if(*end || n<0 || n>INT_MAX)return false;out=(int)n;return true;
    };
    auto toggle=[&](int& out){auto value=param(conn,"on");if(value!="0" && value!="1")return false;out=value=="1";return true;};
    ControlCommand c{};
    bool ok = true;
    if (uri == "/api/layout") {
        c.type = ControlCommand::Type::Layout;
        c.str = param(conn, "name");
        ok = c.str=="presentation" || c.str=="two-cubes" || c.str=="single-cube" || c.str=="wall";
    } else if (uri == "/api/pause") {
        c.type = ControlCommand::Type::Pause;
        ok = toggle(c.i);
    } else if (uri == "/api/config") {
        c.type = ControlCommand::Type::Config;
        const std::string mv = param(conn, "max_visible");
        const std::string rs = param(conn, "rotate_seconds");
        c.i=-1;c.d=-1;
        ok=!mv.empty() || !rs.empty();
        if(!mv.empty())ok=ok && integer(mv,c.i) && c.i>=1 && (unsigned)c.i<=current.maxSlots;
        if(!rs.empty()){char* end=nullptr;c.d=std::strtod(rs.c_str(),&end);ok=ok && !*end && std::isfinite(c.d) && c.d>=1.0 && c.d<=86400.;}
    } else if (uri == "/api/stream") {
        const std::string slot = param(conn, "slot");
        const std::string action = param(conn, "action");
        bool validSlot=integer(slot,c.slot) && (unsigned)c.slot<current.maxSlots;
        if (action == "hide") c.type = ControlCommand::Type::Hide;
        else if (action == "show") c.type = ControlCommand::Type::Show;
        else if (action == "kick") c.type = ControlCommand::Type::Kick;
        else if (action == "pin") c.type = ControlCommand::Type::Pin;
        else if (action == "unpin") c.type = ControlCommand::Type::Unpin;
        else if (action == "spotlight") c.type = ControlCommand::Type::Spotlight;
        else if (action == "unspotlight") c.type = ControlCommand::Type::Unspotlight;
        else if (action == "front") c.type = ControlCommand::Type::Front;
        else ok = false;
        if(current.avActive && c.type==ControlCommand::Type::Kick && c.slot==0){mg_send_http_error(conn,409,"stop selected A/V session instead of kicking its video track");return 409;}
        if(c.type!=ControlCommand::Type::Unspotlight) {
            ok=ok && validSlot;
            bool known=false;for(const auto& stream:current.streams)if((int)stream.slot==c.slot)known=true;
            ok=ok && known;
        }
    } else if (uri == "/api/audio/select") {
        if(!current.avConfigured){mg_send_http_error(conn,501,"A/V plugin not configured");return 501;}
        c.type=ControlCommand::Type::AudioSelect;c.str=param(conn,"session");
        ok=!c.str.empty() && c.str.size()<=1024;
    } else if (uri == "/api/audio") {
        if(!current.avConfigured){mg_send_http_error(conn,501,"no selected A/V session");return 501;}
        c.type=ControlCommand::Type::AudioMute;ok=toggle(c.i);
    } else if (uri == "/api/camera") {
        if(!current.cameraAvailable){mg_send_http_error(conn,501,"camera unavailable");return 501;}
        c.type = ControlCommand::Type::Camera;
        ok = toggle(c.i);
    } else if (uri == "/api/display") {
        if(!current.displaysAvailable){mg_send_http_error(conn,501,"display control unavailable");return 501;}
        c.type = ControlCommand::Type::Display;
        const std::string id = param(conn, "id");
        bool validId=integer(id,c.slot);
        ok = toggle(c.i);
        bool known=false;for(const auto& display:current.displays)if(display.id==c.slot)known=true;
        ok = ok && validId && known;
    } else {
        mg_send_http_error(conn, 404, "unknown endpoint");
        return 404;
    }
    if (!ok) {
        mg_send_http_error(conn, 400, "bad or missing parameters");
        return 400;
    }
    bool full;
    {
        std::lock_guard<std::mutex> hold(m_);
        full=pending_.size()>=1024;
        if(!full)pending_.push_back(std::move(c));
    }
    if(full){mg_send_http_error(conn,503,"command queue full");return 503;}
    sendJson(conn, "{\"ok\":true}");
    return 200;
}

} // namespace vkr
