#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protocol.h"

// Platform-agnostic IPC channel carrying length-prefixed protocol messages.
// Windows backend: named pipe (message mode). Linux future: Unix domain socket
// (which additionally passes fds via SCM_RIGHTS instead of raw handle values).

namespace vkr {

struct IpcMessage {
    proto::MsgType type{};
    std::vector<uint8_t> payload;
    // POSIX only: file descriptors received as SCM_RIGHTS ancillary data with
    // this message (dma-buf fds for SurfaceDesc). Ownership transfers to the
    // receiver; empty on Windows, where handles travel as duplicated values
    // inside the payload instead.
    std::vector<int> fds;

    template <typename T>
    const T* as() const {
        return payload.size() >= sizeof(T) ? reinterpret_cast<const T*>(payload.data()) : nullptr;
    }
};

class IpcChannel {
public:
    IpcChannel() = default;
    ~IpcChannel();
    IpcChannel(IpcChannel&& other) noexcept;
    IpcChannel& operator=(IpcChannel&& other) noexcept;
    IpcChannel(const IpcChannel&) = delete;
    IpcChannel& operator=(const IpcChannel&) = delete;

    // Server: create the endpoint and block until one client connects.
    static IpcChannel listen(const std::string& name);
    // Client: connect to an existing endpoint, retrying up to timeoutMs.
    // quiet suppresses failure logging (for periodic reconnect polling).
    static IpcChannel connect(const std::string& name, uint32_t timeoutMs, bool quiet = false);

    // fds/fdCount attach SCM_RIGHTS ancillary descriptors (POSIX only; must be
    // zero on Windows, where handle values are duplicated into the payload).
    bool send(proto::MsgType type, const void* payload, size_t size,
              const int* fds = nullptr, size_t fdCount = 0);
    template <typename T>
    bool send(proto::MsgType type, const T& value) { return send(type, &value, sizeof(T)); }
    template <typename T>
    bool send(proto::MsgType type, const T& value, const int* fds, size_t fdCount) {
        return send(type, &value, sizeof(T), fds, fdCount);
    }
    bool send(proto::MsgType type) { return send(type, nullptr, 0); }

    // Blocking receive. nullopt on disconnect or protocol error.
    std::optional<IpcMessage> recv();
    // Non-blocking. nullopt when no message is pending OR on disconnect;
    // check connected() to distinguish.
    std::optional<IpcMessage> tryRecv();

    bool connected() const { return connected_; }
    uint32_t peerPid() const { return peerPid_; } // server side only

    // POSIX only: the raw socket fd, for platform APIs that transfer objects
    // over an existing socket (Android's AHardwareBuffer_sendHandleToUnixSocket).
    // -1 on Windows or when disconnected.
    int nativeSocket() const;

private:
    void close();

    void* handle_ = nullptr; // HANDLE (Windows) / int fd (POSIX future)
    bool connected_ = false;
    uint32_t peerPid_ = 0;
};

} // namespace vkr
