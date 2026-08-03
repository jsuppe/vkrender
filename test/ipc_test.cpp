// Self-test for the POSIX IpcChannel backend: message framing, SCM_RIGHTS fd
// passing (via memfd, standing in for a dma-buf), and disconnect detection.
// Runs without any GPU; exercised in CI and on dev boxes via `vkr_ipc_test`.

#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>

#include "ipc.h"
#include "log.h"

using namespace vkr;

static constexpr uint32_t kFillWord = 0xA5C3F00Du;
static constexpr size_t kBufSize = 4096;

static int childMain() {
    IpcChannel ch = IpcChannel::connect("ipc-test", 5000);
    if (!ch.connected()) return 10;

    proto::HelloMsg hello{};
    hello.rendererPid = (uint32_t)getpid();
    if (!ch.send(proto::MsgType::Hello, hello)) return 11;

    // memfd filled with a known pattern stands in for a dma-buf.
    int fd = memfd_create("vkr-ipc-test", MFD_CLOEXEC);
    if (fd < 0) return 12;
    if (ftruncate(fd, kBufSize) != 0) return 13;
    uint32_t* map = (uint32_t*)mmap(nullptr, kBufSize, PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) return 14;
    for (size_t i = 0; i < kBufSize / 4; ++i) map[i] = kFillWord;
    munmap(map, kBufSize);

    proto::SurfaceDescMsg desc{};
    desc.slotKey = 7;
    desc.numPlanes = 2;
    desc.numObjects = 1;
    desc.objectSize[0] = kBufSize;
    if (!ch.send(proto::MsgType::SurfaceDesc, desc, &fd, 1)) return 15;
    close(fd); // receiver owns its copy

    ch.send(proto::MsgType::Bye);
    return 0;
}

int main() {
    pid_t pid = fork();
    if (pid == 0) _exit(childMain());

    IpcChannel ch = IpcChannel::listen("ipc-test");
    if (!ch.connected()) { fprintf(stderr, "FAIL: listen\n"); return 1; }
    if (ch.peerPid() != (uint32_t)pid) { fprintf(stderr, "FAIL: SO_PEERCRED pid\n"); return 1; }

    auto hello = ch.recv();
    if (!hello || hello->type != proto::MsgType::Hello ||
        !hello->as<proto::HelloMsg>() ||
        hello->as<proto::HelloMsg>()->rendererPid != (uint32_t)pid) {
        fprintf(stderr, "FAIL: Hello\n");
        return 1;
    }

    auto surf = ch.recv();
    if (!surf || surf->type != proto::MsgType::SurfaceDesc || surf->fds.size() != 1) {
        fprintf(stderr, "FAIL: SurfaceDesc framing (fds=%zu)\n", surf ? surf->fds.size() : 0);
        return 1;
    }
    const auto* desc = surf->as<proto::SurfaceDescMsg>();
    if (!desc || desc->slotKey != 7 || desc->objectSize[0] != kBufSize) {
        fprintf(stderr, "FAIL: SurfaceDesc payload\n");
        return 1;
    }
    uint32_t* map = (uint32_t*)mmap(nullptr, kBufSize, PROT_READ, MAP_SHARED, surf->fds[0], 0);
    if (map == MAP_FAILED) { fprintf(stderr, "FAIL: mmap of passed fd\n"); return 1; }
    for (size_t i = 0; i < kBufSize / 4; ++i) {
        if (map[i] != kFillWord) { fprintf(stderr, "FAIL: pattern at %zu\n", i); return 1; }
    }
    munmap(map, kBufSize);
    close(surf->fds[0]);

    auto bye = ch.recv();
    if (!bye || bye->type != proto::MsgType::Bye) { fprintf(stderr, "FAIL: Bye\n"); return 1; }

    // Child exits -> next recv must report disconnect, not hang.
    auto end = ch.recv();
    if (end || ch.connected()) { fprintf(stderr, "FAIL: disconnect detection\n"); return 1; }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: child exit %d\n", WEXITSTATUS(status));
        return 1;
    }
    printf("PASS: framing, fd passing, peer pid, disconnect\n");
    return 0;
}
