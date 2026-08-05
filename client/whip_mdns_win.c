// See whip_mdns_win.h. Registers _vkrender-whip._tcp via DnsServiceRegister so
// the Flutter sender (and any DNS-SD browser) can find this receiver on the
// LAN. Windows 10+ ships a built-in mDNS responder -- no Bonjour needed.
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windns.h>

#include <stdio.h>
#include <wchar.h>

#include "whip_mdns_win.h"

#pragma comment(lib, "dnsapi.lib")

static PDNS_SERVICE_INSTANCE g_instance = NULL;
static int g_registered = 0;

static void widen(const char* s, wchar_t* out, int cap) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
}

static void CALLBACK register_cb(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    (void)ctx;
    if (status == ERROR_SUCCESS)
        printf("webrtc: dns-sd registered %ls\n", inst ? inst->pszInstanceName : L"?");
    else
        fprintf(stderr, "webrtc: dns-sd registration failed: %lu\n", status);
    if (inst) DnsServiceFreeInstance(inst);
}

int whip_mdns_register(const char* instanceName, const char* path,
                       unsigned short port) {
    if (g_registered) whip_mdns_unregister();

    wchar_t host[256];
    wchar_t comp[128];
    DWORD n = 128;
    if (GetComputerNameW(comp, &n)) {
        _snwprintf(host, 256, L"%ls.local", comp);
    } else {
        wcscpy(host, L"vkrender.local");
    }

    char full[512];
    snprintf(full, sizeof(full), "%s._vkrender-whip._tcp.local", instanceName);
    wchar_t wInstance[512];
    widen(full, wInstance, 512);

    // One TXT record: path=<path>.
    wchar_t keyBuf[8], valBuf[256];
    widen("path", keyBuf, 8);
    widen(path, valBuf, 256);
    PCWSTR keys[1] = {keyBuf};
    PCWSTR vals[1] = {valBuf};

    g_instance = DnsServiceConstructInstance(wInstance, host, NULL, NULL, port,
                                             0, 0, 1, keys, vals);
    if (!g_instance) {
        fprintf(stderr, "webrtc: DnsServiceConstructInstance failed\n");
        return -1;
    }

    DNS_SERVICE_REGISTER_REQUEST req;
    memset(&req, 0, sizeof(req));
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.pServiceInstance = g_instance;
    req.pRegisterCompletionCallback = register_cb;
    req.unicastEnabled = FALSE;

    DNS_STATUS st = DnsServiceRegister(&req, NULL);
    if (st != DNS_REQUEST_PENDING && st != ERROR_SUCCESS) {
        fprintf(stderr, "webrtc: DnsServiceRegister failed: %lu\n", st);
        DnsServiceFreeInstance(g_instance);
        g_instance = NULL;
        return -1;
    }
    g_registered = 1;
    return 0;
}

void whip_mdns_unregister(void) {
    if (g_registered && g_instance) {
        DNS_SERVICE_REGISTER_REQUEST req;
        memset(&req, 0, sizeof(req));
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.pServiceInstance = g_instance;
        req.pRegisterCompletionCallback = register_cb;
        DnsServiceDeRegister(&req, NULL);
    }
    g_registered = 0;
    g_instance = NULL;
}

#endif // _WIN32
