// M25: vkrender presentation appliance launcher.
//
// One process that starts the whole receiver stack and keeps it alive:
//
//   vkrenderer --serve      (the scene; presentation layout)
//   vkr_webrtc              (WHIP room: Flutter sender / browser / OBS)
//   vkr_miracast            (Windows + Android senders)
//   vkr_airplay             (Apple senders; optional, separate GPLv3 exe)
//   vkr_rtsp                (IP cameras / encoders we DIAL OUT to; opt-in)
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace {

std::string g_dir; // directory of this executable (all binaries live here)
HANDLE g_job = nullptr;
std::vector<HANDLE> g_children;

// Every child writes to its own pipe; the launcher prints WHOLE LINES under
// one lock, tagged with the source. Without this the children share our
// console and their unbuffered writes interleave mid-word (observed:
// "applian[INF] renderer: imported...ce: started vkr_airplay.exe").
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
std::vector<std::thread> g_pumps;

void logLine(const char* tag, const std::string& line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard<std::mutex> hold(g_logMutex);
    printf("%02d:%02d:%02d [%-9s] %s\n", st.wHour, st.wMinute, st.wSecond, tag, line.c_str());
    if (g_logFile) {
        fprintf(g_logFile, "%02d:%02d:%02d [%-9s] %s\n", st.wHour, st.wMinute, st.wSecond, tag,
                line.c_str());
        fflush(g_logFile);
    }
}

// Reads a child's output pipe, emitting one log line per '\n'.
void pumpChildOutput(HANDLE readEnd, std::string tag) {
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        for (DWORD i = 0; i < got; ++i) {
            char c = buf[i];
            if (c == '\n') {
                while (!pending.empty() && (pending.back() == '\r' || pending.back() == ' '))
                    pending.pop_back();
                if (!pending.empty()) logLine(tag.c_str(), pending);
                pending.clear();
            } else if (pending.size() < 4096) {
                pending.push_back(c);
            }
        }
    }
    if (!pending.empty()) logLine(tag.c_str(), pending);
    CloseHandle(readEnd);
}

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

// Launch <exe> <args> from our directory, in our job (so it dies with us),
// with its output redirected into our line-serializing log pump.
bool launch(const std::string& exe, const std::string& args, const std::string& tag,
            bool required) {
    std::string path = g_dir + "\\" + exe;
    if (!fileExists(path)) {
        if (required) logLine("appliance", "MISSING " + exe);
        else logLine("appliance", exe + " not installed, skipping");
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; // inheritable pipe ends
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0); // keep our end private

    std::string cmd = "\"" + path + "\" " + args;
    STARTUPINFOA si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, g_dir.c_str(), &si, &pi)) {
        logLine("appliance", "failed to start " + exe + " (" +
                                 std::to_string(GetLastError()) + ")");
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return false;
    }
    // Drop OUR copy of the write end, else the read never sees EOF.
    CloseHandle(writeEnd);
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    CloseHandle(pi.hThread);
    g_children.push_back(pi.hProcess);
    g_pumps.emplace_back(pumpChildOutput, readEnd, tag);
    logLine("appliance", "started " + exe + " " + args);
    return true;
}

BOOL WINAPI ctrlHandler(DWORD) {
    logLine("appliance", "shutting down");
    if (g_job) CloseHandle(g_job); // kills the whole group
    ExitProcess(0);
    return TRUE;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = exeDir();
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // Program Files is not writable, so the combined log lives in LOCALAPPDATA.
    {
        char local[MAX_PATH];
        if (GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) {
            std::string dir = std::string(local) + "\\vkrender";
            CreateDirectoryA(dir.c_str(), nullptr);
            std::string logPath = dir + "\\vkrender.log";
            g_logFile = fopen(logPath.c_str(), "w");
            printf("vkrender appliance: logging to %s\n", logPath.c_str());
        }
    }

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
    // M27: RTSP is PULL - we dial out to cameras/encoders. Off unless a
    // control port is configured, since unlike the other receivers there is
    // nothing to advertise and nothing to wait for.
    const bool wantRtsp = iniGet("rtsp", "0") != "0";
    const std::string rtspPort = iniGet("rtsp_port", "8890");
    const std::string rtspUrls = iniGet("rtsp_urls", "");

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
    if (!launch("vkrenderer.exe", rendererArgs, "renderer", true)) return 1;
    Sleep(2500); // let the service open its pipe before producers dial in

    if (wantMiracast) launch("vkr_miracast.exe", name + " " + name, "miracast", false);
    launch("vkr_webrtc.exe", name + " " + webrtcPort + " " + name, "webrtc", false);
    if (wantAirplay) launch("vkr_airplay.exe", name + " " + name, "airplay", false);
    if (wantRtsp) {
        // rtsp_urls is a semicolon-separated list -> repeated --url flags.
        std::string rtspArgs = "--pipe " + name + " --port " + rtspPort;
        size_t start = 0;
        while (start < rtspUrls.size()) {
            size_t sep = rtspUrls.find(';', start);
            std::string one =
                rtspUrls.substr(start, sep == std::string::npos ? sep : sep - start);
            if (!one.empty()) rtspArgs += " --url " + one;
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        launch("vkr_rtsp.exe", rtspArgs, "rtsp", false);
    }
    if (wantVcam)
        launch("vkr_vcam.exe", (vcamMode == "mf" ? "--mf " : "") + name + " 1280 720 30",
               "vcam", false);

    printf("appliance: %zu processes up - close this window to stop everything\n",
           g_children.size());

    // Exit as soon as the renderer does (the user closed the scene window).
    WaitForSingleObject(g_children[0], INFINITE);
    printf("appliance: renderer exited, stopping the stack\n");
    if (g_job) CloseHandle(g_job);
    return 0;
}

#endif // _WIN32
