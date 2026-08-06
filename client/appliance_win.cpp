// M25: vkrender presentation appliance launcher.
//
// One process that starts the whole receiver stack and keeps it alive:
//
//   vkrenderer --serve      (the scene; presentation layout)
//   vkr_webrtc              (WHIP room: Flutter sender / browser / OBS)
//   vkr_miracast            (Windows + Android senders)
//   vkr_airplay             (Apple senders; optional, separate GPLv3 exe)
//   vkr_vcam                (the scene as a virtual camera)
//
// Children inherit our environment, so PIN mode (VKR_PIN) and the starting
// layout (VKR_LAYOUT) propagate to every receiver. Settings come from
// vkrender.ini next to the executable so the installer can write them
// without touching the registry:
//
//   [appliance]
//   pin=auto            ; auto | <digits> | off
//   layout=presentation
//   window=             ; empty = 1280x720+80+80, or WxH+X+Y (repeatable ;)
//   webrtc_port=8889
//   virtual_camera=1    ; 0 to skip
//   vcam_mode=dshow     ; dshow | mf
//   airplay=1           ; 0 to skip (or if vkr_airplay.exe is absent)
//   miracast=1
//
// Everything is killed as a group when this process exits (job object), so
// closing the launcher never leaves orphaned receivers behind.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <shlwapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace {

std::string g_dir; // directory of this executable (all binaries live here)
HANDLE g_job = nullptr;
std::vector<HANDLE> g_children;

std::string exeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecA(buf);
    return buf;
}

std::string iniGet(const char* key, const char* fallback) {
    char buf[512];
    std::string ini = g_dir + "\\vkrender.ini";
    GetPrivateProfileStringA("appliance", key, fallback, buf, sizeof(buf), ini.c_str());
    return buf;
}

bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Launch <exe> <args> from our directory, in our job (so it dies with us).
bool launch(const std::string& exe, const std::string& args, bool required) {
    std::string path = g_dir + "\\" + exe;
    if (!fileExists(path)) {
        if (required) fprintf(stderr, "appliance: MISSING %s\n", exe.c_str());
        else printf("appliance: %s not installed, skipping\n", exe.c_str());
        return false;
    }
    std::string cmd = "\"" + path + "\" " + args;
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, g_dir.c_str(), &si, &pi)) {
        fprintf(stderr, "appliance: failed to start %s (%lu)\n", exe.c_str(), GetLastError());
        return false;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    CloseHandle(pi.hThread);
    g_children.push_back(pi.hProcess);
    printf("appliance: started %s %s\n", exe.c_str(), args.c_str());
    return true;
}

BOOL WINAPI ctrlHandler(DWORD) {
    printf("appliance: shutting down\n");
    if (g_job) CloseHandle(g_job); // kills the whole group
    ExitProcess(0);
    return TRUE;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = exeDir();
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // Kill-on-close job: no orphaned receivers if this process goes away.
    g_job = CreateJobObjectA(nullptr, nullptr);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    const std::string pin = iniGet("pin", "auto");
    const std::string layout = iniGet("layout", "presentation");
    const std::string window = iniGet("window", "");
    const std::string webrtcPort = iniGet("webrtc_port", "8889");
    const bool wantVcam = iniGet("virtual_camera", "1") != "0";
    const std::string vcamMode = iniGet("vcam_mode", "dshow");
    const bool wantAirplay = iniGet("airplay", "1") != "0";
    const bool wantMiracast = iniGet("miracast", "1") != "0";
    const std::string name = iniGet("name", "vkrender");

    // Propagate policy to every child through the environment.
    SetEnvironmentVariableA("VKR_PIN", pin == "off" ? nullptr : pin.c_str());
    SetEnvironmentVariableA("VKR_LAYOUT", layout.c_str());
    // GStreamer runtime for the receivers (installed separately).
    {
        char pathBuf[32768];
        DWORD n = GetEnvironmentVariableA("PATH", pathBuf, sizeof(pathBuf));
        std::string p(pathBuf, n);
        for (const char* gst : {"C:\\gstreamer\\1.0\\msvc_x86_64\\bin",
                                "C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\\bin"}) {
            if (GetFileAttributesA(gst) != INVALID_FILE_ATTRIBUTES) {
                p = std::string(gst) + ";" + p;
                break;
            }
        }
        SetEnvironmentVariableA("PATH", p.c_str());
    }

    printf("vkrender appliance: PIN=%s layout=%s\n", pin.c_str(), layout.c_str());

    std::string rendererArgs = "--serve " + name;
    if (!window.empty()) {
        // "1280x720+0+0;1280x720+1280+0" -> repeated --window flags
        size_t start = 0;
        while (start < window.size()) {
            size_t sep = window.find(';', start);
            std::string one = window.substr(start, sep == std::string::npos ? sep : sep - start);
            if (!one.empty()) rendererArgs += " --window " + one;
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
    } else {
        rendererArgs += " --window 1280x720+80+80";
    }
    if (!launch("vkrenderer.exe", rendererArgs, true)) return 1;
    Sleep(2500); // let the service open its pipe before producers dial in

    if (wantMiracast) launch("vkr_miracast.exe", name + " " + name, false);
    launch("vkr_webrtc.exe", name + " " + webrtcPort + " " + name, false);
    if (wantAirplay) launch("vkr_airplay.exe", name + " " + name, false);
    if (wantVcam)
        launch("vkr_vcam.exe", (vcamMode == "mf" ? "--mf " : "") + name + " 1280 720 30", false);

    printf("appliance: %zu processes up - close this window to stop everything\n",
           g_children.size());

    // Exit as soon as the renderer does (the user closed the scene window).
    WaitForSingleObject(g_children[0], INFINITE);
    printf("appliance: renderer exited, stopping the stack\n");
    if (g_job) CloseHandle(g_job);
    return 0;
}

#endif // _WIN32
