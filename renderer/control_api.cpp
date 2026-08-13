// M42: the renderer's moderation API. See control_api.h for the contract.

#ifdef _WIN32

#include "control_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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

} // namespace

bool ControlApi::start(int port) {
    char ports[64];
    // Loopback only - reachability from elsewhere is vkr_web's (authenticated)
    // business, never this server's.
    snprintf(ports, sizeof(ports), "127.0.0.1:%d", port);
    mg_init_library(0);
    const char* options[] = {"listening_ports", ports, "num_threads", "4", nullptr};
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
    return now - previewRequestedNs_ < 3000000000ll &&
           now - previewRenderedNs_ > 400000000ll;
}

void ControlApi::publishPreview(const uint8_t* bgra, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> hold(m_);
    previewBgra_.assign(bgra, bgra + (size_t)w * h * 4);
    previewW_ = w;
    previewH_ = h;
    previewRenderedNs_ = nowNs();
}

int ControlApi::handleRequest(mg_connection* conn) {
    const mg_request_info* ri = mg_get_request_info(conn);
    const std::string uri = ri->local_uri ? ri->local_uri : "";
    const bool post = ri->request_method && !strcmp(ri->request_method, "POST");

    if (uri == "/api/status" || uri == "/api/streams") {
        ControlSnapshot s;
        {
            std::lock_guard<std::mutex> hold(m_);
            s = snap_;
        }
        std::string j;
        char buf[256];
        if (uri == "/api/status") {
            snprintf(buf, sizeof(buf),
                     "{\"layout\":\"%s\",\"paused\":%s,\"maxVisible\":%u,"
                     "\"rotateSeconds\":%.1f,\"spotlightSlot\":%d,\"presenting\":%s,"
                     "\"streams\":%u}",
                     s.layout.c_str(), s.paused ? "true" : "false", s.maxVisible,
                     s.rotateSeconds, s.spotlightSlot, s.presenting ? "true" : "false",
                     (unsigned)s.streams.size());
            j = buf;
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
        // BGRA -> RGB, then JPEG on THIS thread - encoding must never stall
        // the render loop.
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
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                  "Cache-Control: no-store\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                  (unsigned)jpg.size());
        mg_write(conn, jpg.data(), jpg.size());
        return 200;
    }

    if (!post) {
        mg_send_http_error(conn, 405, "POST required");
        return 405;
    }

    ControlCommand c{};
    bool ok = true;
    if (uri == "/api/layout") {
        c.type = ControlCommand::Type::Layout;
        c.str = param(conn, "name");
        ok = !c.str.empty();
    } else if (uri == "/api/pause") {
        c.type = ControlCommand::Type::Pause;
        c.i = param(conn, "on") == "1" ? 1 : 0;
    } else if (uri == "/api/config") {
        c.type = ControlCommand::Type::Config;
        const std::string mv = param(conn, "max_visible");
        const std::string rs = param(conn, "rotate_seconds");
        c.i = mv.empty() ? -1 : atoi(mv.c_str());
        c.d = rs.empty() ? -1.0 : atof(rs.c_str());
    } else if (uri == "/api/stream") {
        const std::string slot = param(conn, "slot");
        const std::string action = param(conn, "action");
        c.slot = slot.empty() ? -1 : atoi(slot.c_str());
        if (action == "hide") c.type = ControlCommand::Type::Hide;
        else if (action == "show") c.type = ControlCommand::Type::Show;
        else if (action == "kick") c.type = ControlCommand::Type::Kick;
        else if (action == "pin") c.type = ControlCommand::Type::Pin;
        else if (action == "unpin") c.type = ControlCommand::Type::Unpin;
        else if (action == "spotlight") c.type = ControlCommand::Type::Spotlight;
        else if (action == "unspotlight") c.type = ControlCommand::Type::Unspotlight;
        else if (action == "front") c.type = ControlCommand::Type::Front;
        else ok = false;
        if (c.slot < 0 && c.type != ControlCommand::Type::Unspotlight) ok = false;
    } else {
        mg_send_http_error(conn, 404, "unknown endpoint");
        return 404;
    }
    if (!ok) {
        mg_send_http_error(conn, 400, "bad or missing parameters");
        return 400;
    }
    {
        std::lock_guard<std::mutex> hold(m_);
        pending_.push_back(std::move(c));
    }
    sendJson(conn, "{\"ok\":true}");
    return 200;
}

} // namespace vkr

#endif // _WIN32
