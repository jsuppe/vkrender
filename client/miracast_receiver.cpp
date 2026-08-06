// M16: Miracast receiver -> vkrender producer (Windows).
//
// Uses the public Windows.Media.Miracast.MiracastReceiver API (1809+) to be a
// Wi-Fi Display sink: phones/laptops discover "vkrender" as a cast target,
// and each received frame is copied into a dequeued vkrender_client ring
// texture - so the cast shows up as one more stream on the scene, exactly
// like any other producer.
//
//   vkr_miracast [pipe] [friendlyName]
//
// Notes:
//  - Windows' own "Projecting to this PC" (Wireless Display) must be off or
//    not installed, or it claims the sink role first.
//  - Discovery runs over Wi-Fi Direct via the Microsoft Wi-Fi Direct Virtual
//    Adapter (roles negotiated dynamically over a WDI/WiFiCx driver - see
//    docs/MIRACAST.md); Windows senders can also use the LAN (MS-MICE).
//  - Video only; cast audio is discarded.
//  - MediaPlayer's frame server renders on its own D3D device;
//    CopyFrameToVideoSurface handles the cross-device copy into our shared
//    ring texture internally.

#ifdef _WIN32

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Miracast.h>
#include <winrt/Windows.Media.Playback.h>

#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <future>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "pin_overlay.h"
#include "vkrender_client.h"

using namespace winrt;
using namespace winrt::Windows::Media::Miracast;
using namespace winrt::Windows::Media::Playback;
namespace WGDX = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

struct CastState {
    vkrClient* client = nullptr;
    std::mutex mutex;
    MediaPlayer player{nullptr};
    std::atomic<uint64_t> frames{0};
};

WGDX::IDirect3DSurface surfaceFor(ID3D11Texture2D* texture) {
    com_ptr<IDXGISurface> dxgi;
    texture->QueryInterface(IID_PPV_ARGS(dxgi.put()));
    WGDX::IDirect3DSurface surface{nullptr};
    check_hresult(CreateDirect3D11SurfaceFromDXGISurface(
        dxgi.get(), reinterpret_cast<::IInspectable**>(put_abi(surface))));
    return surface;
}

const wchar_t* sessionStartStatusName(MiracastReceiverSessionStartStatus s) {
    switch (s) {
        case MiracastReceiverSessionStartStatus::Success: return L"success";
        case MiracastReceiverSessionStartStatus::MiracastNotSupported:
            return L"the OS reports Miracast receiving unavailable (stack-level, "
                   L"not necessarily the adapter - see docs/MIRACAST.md)";
        case MiracastReceiverSessionStartStatus::AccessDenied:
            return L"access denied (is 'Projecting to this PC' claiming the sink?)";
        default: return L"unknown failure";
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* pipe = argc > 1 ? argv[1] : "vkrender";
    std::wstring name = L"vkrender";
    if (argc > 2) {
        std::string n = argv[2];
        name.assign(n.begin(), n.end());
    }

    // Unbuffered so progress is visible when redirected to a log.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    init_apartment();

    CastState state;
    state.client = vkrConnect(pipe, nullptr, 1280, 720);
    if (!state.client) {
        fprintf(stderr, "miracast: cannot reach the vkrenderer service on pipe '%s'\n", pipe);
        return 1;
    }

    MiracastReceiver receiver;
    auto status = receiver.GetStatus();
    wprintf(L"miracast: listening status %d, max connections %d\n",
            (int)status.ListeningStatus(), (int)status.MaxSimultaneousConnections());

    auto settings = receiver.GetDefaultSettings();
    settings.FriendlyName(name);
    // M25: optional PIN. Miracast has it in-protocol - the sender's own UI
    // asks for the digits; we display them (PinDisplayRequired means the
    // RECEIVER shows a PIN the user types on the sending device).
    const bool usePin = vkr::pinEnabled();
    std::string pin = usePin ? vkr::pinForConnection() : "";
    settings.AuthorizationMethod(usePin ? MiracastReceiverAuthorizationMethod::PinDisplayRequired
                                        : MiracastReceiverAuthorizationMethod::None);
    settings.ModelName(L"vkrender");
    settings.ModelNumber(L"M16");
    settings.RequireAuthorizationFromKnownTransmitters(false);
    receiver.DisconnectAllAndApplySettings(settings);

    auto session = receiver.CreateSession(nullptr);
    session.AllowConnectionTakeover(true);

    session.ConnectionCreated(
        [&pin](MiracastReceiverSession const&,
               MiracastReceiverConnectionCreatedEventArgs const& args) {
            wprintf(L"miracast: connection from '%s'\n",
                    args.Connection().Transmitter().Name().c_str());
            if (!pin.empty()) {
                // Show the digits the user must type on the sending device.
                vkr::pinOverlayShow(pin, "Miracast");
                wprintf(L"miracast: PIN required - showing %hs on screen\n", pin.c_str());
            }
        });
    session.Disconnected([&pin](MiracastReceiverSession const&,
                                MiracastReceiverDisconnectedEventArgs const& args) {
        wprintf(L"miracast: '%s' disconnected\n",
                args.Connection().Transmitter().Name().c_str());
        if (!pin.empty()) vkr::pinOverlayHide();
    });

    session.MediaSourceCreated([&state](MiracastReceiverSession const&,
                                        MiracastReceiverMediaSourceCreatedEventArgs const& args) {
        wprintf(L"miracast: media stream up, feeding the scene\n");
        vkr::pinOverlayHide(); // authorized: media is flowing
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.player) state.player.Close();
        state.player = MediaPlayer();
        state.player.IsVideoFrameServerEnabled(true);
        state.player.RealTimePlayback(true);
        state.player.IsMuted(true);
        state.player.VideoFrameAvailable([&state](MediaPlayer const& mp, auto&&) {
            std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
            if (!lock.owns_lock()) return;
            vkrBuffer buf;
            if (vkrDequeueBuffer(state.client, &buf, 20) != 0) return; // drop frame
            try {
                mp.CopyFrameToVideoSurface(surfaceFor(buf.texture));
            } catch (winrt::hresult_error const& e) {
                fwprintf(stderr, L"miracast: frame copy failed: %s\n", e.message().c_str());
                return;
            }
            vkrQueueBuffer(state.client, &buf, -1);
            ++state.frames;
        });
        state.player.Source(args.MediaSource());
        state.player.Play();
    });

    // MiracastReceiverSession::Start() is documented as synchronous but can
    // BLOCK FOREVER instead of returning a status (reproduced on a machine
    // that reports "Wireless Display Supported: Yes" - so the hang is NOT a
    // hardware-capability verdict; see docs/MIRACAST.md). Run it under a
    // watchdog so the appliance never shows a receiver silently doing nothing.
    std::future<MiracastReceiverSessionStartResult> startFuture =
        std::async(std::launch::async, [&session] { return session.Start(); });
    if (startFuture.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
        // Do NOT claim the hardware is incapable: Windows negotiates the sink
        // role dynamically through the Microsoft Wi-Fi Direct Virtual Adapter
        // layered over the WDI/WiFiCx driver, so capability is a property of
        // the whole stack. Print what to check instead. See docs/MIRACAST.md.
        wprintf(L"miracast: Start() did not return within 15 s - giving up so the appliance "
                L"isn't left with a receiver that is silently doing nothing.\n"
                L"miracast: check, in this order:\n"
                L"miracast:   netsh wlan show driver | findstr \"Wireless Display\"\n"
                L"miracast:   (elevated) Get-WindowsCapability -Online -Name "
                L"App.WirelessDisplay.Connect*\n"
                L"miracast:   Wi-Fi radio on? 'Projecting to this PC' off (it owns the sink)?\n"
                L"miracast: details + our findings: docs/MIRACAST.md\n"
                L"miracast: AirPlay and WebRTC receiving are unaffected - set miracast=0 in "
                L"vkrender.ini to skip this.\n");
        fflush(stdout);
        vkrDisconnect(state.client);
        // The WinRT call is still stuck on its own thread; leave abruptly
        // rather than block the appliance shutting down.
        ExitProcess(0);
    }
    auto result = startFuture.get();
    if (result.Status() != MiracastReceiverSessionStartStatus::Success) {
        if (result.Status() == MiracastReceiverSessionStartStatus::MiracastNotSupported) {
            // The OS reports the sink path unavailable. That is a stack-level
            // verdict (virtual adapter + WDI/WiFiCx driver + graphics driver +
            // feature state), so point at the checks rather than blaming the
            // adapter. Not an error for the appliance: exit 0.
            wprintf(L"miracast: the OS reports Miracast receiving unavailable right now.\n"
                    L"miracast: check 'netsh wlan show driver | findstr \"Wireless Display\"', "
                    L"that a Wi-Fi adapter is present and enabled, and that the 'Wireless "
                    L"display' optional feature is installed. See docs/MIRACAST.md.\n"
                    L"miracast: AirPlay and WebRTC receiving are unaffected; set miracast=0 in "
                    L"vkrender.ini to skip this check.\n");
            vkrDisconnect(state.client);
            return 0;
        }
        fwprintf(stderr, L"miracast: session start failed: %s\n",
                 sessionStartStatusName(result.Status()));
        if (result.Status() == MiracastReceiverSessionStartStatus::AccessDenied)
            fwprintf(stderr, L"miracast: another app owns the sink role - turn off Windows' "
                             L"'Projecting to this PC' and retry.\n");
        vkrDisconnect(state.client);
        return 1;
    }
    wprintf(L"miracast: receiving as '%s' - cast from a phone/laptop to see it on the scene\n",
            name.c_str());

    uint64_t lastFrames = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        uint64_t f = state.frames.load();
        if (f != lastFrames) {
            wprintf(L"miracast: %llu frames into the scene (+%llu in 5s)\n",
                    (unsigned long long)f, (unsigned long long)(f - lastFrames));
            lastFrames = f;
        }
    }
}

#else
int main() { return 0; }
#endif
