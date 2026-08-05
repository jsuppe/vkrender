// Minimal, self-contained DNS-SD advertisement for the WHIP receiver on
// Windows. Deliberately NOT built on client/dnssd_win.c: that file includes
// the vendored UxPlay headers (GPLv3), and the WebRTC receiver is MPL/LGPL.
// This helper depends only on the Win32 DNS-SD API (dnsapi), so vkr_webrtc
// stays free of the AirPlay licensing.
#ifndef VKR_WHIP_MDNS_WIN_H
#define VKR_WHIP_MDNS_WIN_H

#ifdef __cplusplus
extern "C" {
#endif

// Advertise `_vkrender-whip._tcp` as `instanceName` on `port`, with a single
// TXT record `path=<path>` so senders can build the ingest URL. Returns 0 on
// success. Idempotent-ish: a second call replaces the first registration.
int whip_mdns_register(const char* instanceName, const char* path,
                       unsigned short port);

void whip_mdns_unregister(void);

#ifdef __cplusplus
}
#endif

#endif // VKR_WHIP_MDNS_WIN_H
