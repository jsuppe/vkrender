#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <thread>

#include "ipc.h"
#include "log.h"

namespace vkr {

namespace {
constexpr DWORD kPipeBufferSize = 64 * 1024;

std::wstring pipePath(const std::string& name) {
    std::wstring path = L"\\\\.\\pipe\\";
    path.append(name.begin(), name.end());
    return path;
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
        CloseHandle((HANDLE)handle_);
        handle_ = nullptr;
    }
    connected_ = false;
}

IpcChannel IpcChannel::listen(const std::string& name) {
    IpcChannel ch;
    HANDLE pipe = CreateNamedPipeW(
        pipePath(name).c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, kPipeBufferSize, kPipeBufferSize, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        VKR_ERR("CreateNamedPipe(%s) failed: %lu", name.c_str(), GetLastError());
        return ch;
    }
    VKR_LOG("ipc: waiting for renderer on \\\\.\\pipe\\%s ...", name.c_str());
    BOOL ok = ConnectNamedPipe(pipe, nullptr);
    if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
        VKR_ERR("ConnectNamedPipe failed: %lu", GetLastError());
        CloseHandle(pipe);
        return ch;
    }
    ULONG pid = 0;
    GetNamedPipeClientProcessId(pipe, &pid);
    ch.handle_ = pipe;
    ch.connected_ = true;
    ch.peerPid_ = pid;
    VKR_LOG("ipc: client connected, pid=%lu", pid);
    return ch;
}

IpcChannel IpcChannel::connect(const std::string& name, uint32_t timeoutMs) {
    IpcChannel ch;
    const std::wstring path = pipePath(name);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        HANDLE pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                VKR_ERR("SetNamedPipeHandleState failed: %lu", GetLastError());
                CloseHandle(pipe);
                return ch;
            }
            ch.handle_ = pipe;
            ch.connected_ = true;
            return ch;
        }
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(path.c_str(), 500);
        } else if (err == ERROR_FILE_NOT_FOUND) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else {
            VKR_ERR("ipc connect failed: %lu", err);
            return ch;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            VKR_ERR("ipc connect timed out after %u ms", timeoutMs);
            return ch;
        }
    }
}

bool IpcChannel::send(proto::MsgType type, const void* payload, size_t size) {
    if (!connected_) return false;
    // Message-mode pipe: one WriteFile == one message, so header and payload
    // must go out in a single write.
    std::vector<uint8_t> buf(sizeof(proto::MsgHeader) + size);
    auto* hdr = reinterpret_cast<proto::MsgHeader*>(buf.data());
    hdr->magic = proto::kProtocolMagic;
    hdr->type = type;
    hdr->payloadSize = (uint32_t)size;
    if (size) memcpy(buf.data() + sizeof(proto::MsgHeader), payload, size);

    DWORD written = 0;
    if (!WriteFile((HANDLE)handle_, buf.data(), (DWORD)buf.size(), &written, nullptr) ||
        written != buf.size()) {
        VKR_WARN("ipc send failed: %lu", GetLastError());
        connected_ = false;
        return false;
    }
    return true;
}

std::optional<IpcMessage> IpcChannel::recv() {
    if (!connected_) return std::nullopt;
    uint8_t buf[kPipeBufferSize];
    DWORD read = 0;
    if (!ReadFile((HANDLE)handle_, buf, sizeof(buf), &read, nullptr)) {
        connected_ = false;
        return std::nullopt;
    }
    if (read < sizeof(proto::MsgHeader)) {
        VKR_ERR("ipc: short message (%lu bytes)", read);
        connected_ = false;
        return std::nullopt;
    }
    auto* hdr = reinterpret_cast<const proto::MsgHeader*>(buf);
    if (hdr->magic != proto::kProtocolMagic ||
        sizeof(proto::MsgHeader) + hdr->payloadSize != read) {
        VKR_ERR("ipc: malformed message (magic=%08x size=%u read=%lu)",
                hdr->magic, hdr->payloadSize, read);
        connected_ = false;
        return std::nullopt;
    }
    IpcMessage msg;
    msg.type = hdr->type;
    msg.payload.assign(buf + sizeof(proto::MsgHeader), buf + read);
    return msg;
}

std::optional<IpcMessage> IpcChannel::tryRecv() {
    if (!connected_) return std::nullopt;
    DWORD avail = 0;
    if (!PeekNamedPipe((HANDLE)handle_, nullptr, 0, nullptr, &avail, nullptr)) {
        connected_ = false;
        return std::nullopt;
    }
    if (avail == 0) return std::nullopt;
    return recv();
}

} // namespace vkr
