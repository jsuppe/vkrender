// POSIX backend for IpcChannel: SOCK_SEQPACKET Unix domain socket.
// SEQPACKET preserves message boundaries (one sendmsg == one recvmsg), the
// same property the Windows backend gets from message-mode named pipes, and
// SCM_RIGHTS ancillary data carries dma-buf fds with SurfaceDesc messages.

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "ipc.h"
#include "log.h"

namespace vkr {

namespace {

constexpr size_t kMaxMessageSize = 64 * 1024;
constexpr size_t kMaxFds = 8;

std::string socketPath(const std::string& name) {
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    std::string dir = runtime && runtime[0] ? runtime : "/tmp";
    return dir + "/vkrender-" + name + ".sock";
}

// The listening socket is not the channel; keep it just long enough to accept
// one client, mirroring the single-instance named pipe on Windows.
int acceptOne(const std::string& path) {
    int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (server < 0) {
        VKR_ERR("socket(AF_UNIX) failed: %s", strerror(errno));
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        VKR_ERR("ipc: socket path too long: %s", path.c_str());
        close(server);
        return -1;
    }
    strcpy(addr.sun_path, path.c_str());
    unlink(path.c_str()); // stale socket from a previous run
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(server, 1) < 0) {
        VKR_ERR("bind/listen(%s) failed: %s", path.c_str(), strerror(errno));
        close(server);
        return -1;
    }
    VKR_LOG("ipc: waiting for renderer on %s ...", path.c_str());
    int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) VKR_ERR("accept failed: %s", strerror(errno));
    close(server);
    unlink(path.c_str());
    return client;
}

} // namespace

IpcChannel::~IpcChannel() { close(); }

IpcChannel::IpcChannel(IpcChannel&& other) noexcept {
    handle_ = other.handle_;
    connected_ = other.connected_;
    peerPid_ = other.peerPid_;
    other.handle_ = nullptr;
    other.connected_ = false;
}

IpcChannel& IpcChannel::operator=(IpcChannel&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        connected_ = other.connected_;
        peerPid_ = other.peerPid_;
        other.handle_ = nullptr;
        other.connected_ = false;
    }
    return *this;
}

void IpcChannel::close() {
    if (handle_) {
        ::close((int)(intptr_t)handle_ - 1);
        handle_ = nullptr;
    }
    connected_ = false;
}

// fd 0 is valid on POSIX, so store fd+1 in the void* handle slot.
static void* packFd(int fd) { return (void*)(intptr_t)(fd + 1); }
static int unpackFd(void* h) { return (int)(intptr_t)h - 1; }

IpcChannel IpcChannel::listen(const std::string& name) {
    IpcChannel ch;
    int fd = acceptOne(socketPath(name));
    if (fd < 0) return ch;
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
        ch.peerPid_ = (uint32_t)cred.pid;
    ch.handle_ = packFd(fd);
    ch.connected_ = true;
    VKR_LOG("ipc: client connected, pid=%u", ch.peerPid_);
    return ch;
}

IpcChannel IpcChannel::connect(const std::string& name, uint32_t timeoutMs, bool quiet) {
    IpcChannel ch;
    const std::string path = socketPath(name);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            if (!quiet) VKR_ERR("socket(AF_UNIX) failed: %s", strerror(errno));
            return ch;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) {
            ch.handle_ = packFd(fd);
            ch.connected_ = true;
            return ch;
        }
        ::close(fd);
        if (std::chrono::steady_clock::now() >= deadline) {
            if (!quiet) VKR_ERR("ipc connect timed out after %u ms", timeoutMs);
            return ch;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

bool IpcChannel::send(proto::MsgType type, const void* payload, size_t size,
                      const int* fds, size_t fdCount) {
    if (!connected_) return false;
    if (fdCount > kMaxFds) {
        VKR_ERR("ipc: too many fds (%zu)", fdCount);
        return false;
    }

    std::vector<uint8_t> buf(sizeof(proto::MsgHeader) + size);
    auto* hdr = reinterpret_cast<proto::MsgHeader*>(buf.data());
    hdr->magic = proto::kProtocolMagic;
    hdr->type = type;
    hdr->payloadSize = (uint32_t)size;
    if (size) memcpy(buf.data() + sizeof(proto::MsgHeader), payload, size);

    iovec iov{buf.data(), buf.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsgBuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
    if (fdCount) {
        msg.msg_control = cmsgBuf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fdCount);
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fdCount);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fdCount);
    }

    ssize_t sent = sendmsg(unpackFd(handle_), &msg, MSG_NOSIGNAL);
    if (sent != (ssize_t)buf.size()) {
        VKR_WARN("ipc send failed: %s", strerror(errno));
        connected_ = false;
        return false;
    }
    return true;
}

std::optional<IpcMessage> IpcChannel::recv() {
    if (!connected_) return std::nullopt;

    std::vector<uint8_t> buf(kMaxMessageSize);
    iovec iov{buf.data(), buf.size()};
    char cmsgBuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);

    ssize_t n = recvmsg(unpackFd(handle_), &msg, MSG_CMSG_CLOEXEC);
    if (n <= 0) { // 0 == orderly shutdown
        connected_ = false;
        return std::nullopt;
    }

    IpcMessage out;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const int* received = (const int*)CMSG_DATA(cmsg);
            out.fds.assign(received, received + count);
        }
    }

    if ((size_t)n < sizeof(proto::MsgHeader)) {
        VKR_ERR("ipc: short message (%zd bytes)", n);
        for (int fd : out.fds) ::close(fd);
        connected_ = false;
        return std::nullopt;
    }
    auto* hdr = reinterpret_cast<const proto::MsgHeader*>(buf.data());
    if (hdr->magic != proto::kProtocolMagic ||
        sizeof(proto::MsgHeader) + hdr->payloadSize != (size_t)n) {
        VKR_ERR("ipc: malformed message (magic=%08x size=%u read=%zd)", hdr->magic,
                hdr->payloadSize, n);
        for (int fd : out.fds) ::close(fd);
        connected_ = false;
        return std::nullopt;
    }
    out.type = hdr->type;
    out.payload.assign(buf.begin() + sizeof(proto::MsgHeader), buf.begin() + n);
    return out;
}

std::optional<IpcMessage> IpcChannel::tryRecv() {
    if (!connected_) return std::nullopt;
    pollfd pfd{unpackFd(handle_), POLLIN, 0};
    int r = poll(&pfd, 1, 0);
    if (r < 0) {
        connected_ = false;
        return std::nullopt;
    }
    if (r == 0 || !(pfd.revents & (POLLIN | POLLHUP))) return std::nullopt;
    return recv();
}

} // namespace vkr
