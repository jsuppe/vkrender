// M41: vkr_web - the appliance's web front door.
//
// Serves the browser sender application (the Flutter WHIP sender's web build)
// and the desktop-app downloads, so joining a room is: open the appliance's
// address, share your screen - or grab the installer for your OS from
// /downloads/ and keep it.
//
//   vkr_web [--root DIR] [--ports LIST] [--pem FILE] [--name NAME]
//
//     --root   webroot directory (default: "webroot" beside the exe)
//     --ports  civetweb listener list (default "8080,8443s" - HTTP + HTTPS)
//     --pem    combined cert+key PEM (default: "web.pem" beside the exe,
//              GENERATED self-signed on first run if missing)
//     --name   appliance name shown in logs/cert CN (default "vkrender")
//
// HTTPS is not decoration. getDisplayMedia - the browser API the sender's
// screen share stands on - only exists in a SECURE CONTEXT: https, or
// localhost. A page served over plain http to another machine simply has no
// screen-share button to offer. So the appliance serves https with a
// self-signed certificate (one accept-the-risk click per browser), and keeps
// plain http alongside for the downloads page and for localhost testing.
//
// The certificate is generated in-process with the OpenSSL that already ships
// with the GStreamer runtime - no openssl.exe, no extra install step.
//
// Vendored civetweb (third_party/civetweb, MIT) does the serving; this file is
// only the cert bootstrap and configuration.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "civetweb.h"

namespace {

std::string exeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

bool fileExists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Self-signed cert + key, both PEM-appended into one file (civetweb wants the
// pair combined). 10 years, CN=<name>, SAN covering the machine's hostname -
// browsers will still warn (self-signed is self-signed), but one accepted
// exception per browser is the entire ceremony.
bool generatePem(const std::string& path, const std::string& name) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (!pkey) return false;

    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), (long)GetTickCount64());
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 60L * 60 * 24 * 3650);
    X509_set_pubkey(x509, pkey);

    X509_NAME* sub = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(sub, "CN", MBSTRING_ASC, (const unsigned char*)name.c_str(),
                               -1, -1, 0);
    X509_set_issuer_name(x509, sub); // self-signed: issuer == subject

    char host[256] = "localhost";
    DWORD hn = sizeof(host);
    GetComputerNameExA(ComputerNameDnsHostname, host, &hn);
    std::string san = std::string("DNS:") + host + ",DNS:" + host + ".local,DNS:localhost";
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, x509, x509, nullptr, nullptr, 0);
    if (X509_EXTENSION* ext =
            X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san.c_str())) {
        X509_add_ext(x509, ext, -1);
        X509_EXTENSION_free(ext);
    }

    bool ok = X509_sign(x509, pkey, EVP_sha256()) > 0;
    if (ok) {
        // BIO, never FILE*: a FILE* from this exe's CRT crossing into the
        // OpenSSL DLL trips the OPENSSL_APPLINK abort (observed: the process
        // dies right after fopen leaves a 0-byte pem). BIO_new_file opens the
        // file inside OpenSSL's own CRT, so no applink shim is involved.
        BIO* bio = BIO_new_file(path.c_str(), "wb");
        ok = bio && PEM_write_bio_X509(bio, x509) &&
             PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        if (bio) BIO_free(bio);
    }
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok;
}

int g_whipPort = 8889;
int g_controlPort = 8875;    // the renderer's moderation API (localhost)
std::string g_adminPin;      // empty = moderator console disabled

// Which local port a proxied route forwards to, and whether it demands the
// moderator token first. Handed to civetweb as the handler's cbdata.
struct ProxyTarget {
    int* port;
    bool requireAdmin;
};
ProxyTarget g_whipTarget{&g_whipPort, false};
ProxyTarget g_apiTarget{&g_controlPort, true};

// Reverse-proxy one request to the local WHIP room. THE PROXY IS LOAD-BEARING:
// the sender page is https, the room speaks plain http on its own port, and a
// browser refuses an https page's request to an http endpoint as mixed
// content. Same https origin for page and API, and the browser never sees a
// second port or a second certificate.
//
// Forwards the method, path, body, Content-Type and Authorization (the PIN
// rides as a Bearer token); relays the status, body, and the headers WHIP
// semantics live in (Location names the session; WWW-Authenticate carries the
// PIN challenge).
int proxyHandler(struct mg_connection* conn, void* cbdata) {
    const ProxyTarget& target = *static_cast<const ProxyTarget*>(cbdata);
    const mg_request_info* ri = mg_get_request_info(conn);
    if (target.requireAdmin) {
        // The moderator console's gate. The renderer's API itself is
        // localhost-only and unauthenticated by design; THIS check is what
        // stands between the network and it.
        if (g_adminPin.empty()) {
            mg_send_http_error(conn, 403, "moderator console disabled (no admin_pin set)");
            return 403;
        }
        const char* auth = mg_get_header(conn, "Authorization");
        const std::string want = "Bearer " + g_adminPin;
        if (!auth || want != auth) {
            mg_printf(conn,
                      "HTTP/1.1 401 Unauthorized\r\n"
                      "WWW-Authenticate: Bearer realm=\"vkrender-moderator\"\r\n"
                      "Content-Length: 0\r\nConnection: close\r\n\r\n");
            return 401;
        }
    }
    std::string body;
    {
        char buf[8192];
        int n;
        while ((n = mg_read(conn, buf, sizeof buf)) > 0) body.append(buf, (size_t)n);
    }
    char err[256] = {};
    mg_connection* up = mg_connect_client("127.0.0.1", *target.port, 0, err, sizeof err);
    if (!up) {
        mg_send_http_error(conn, 502, "upstream unreachable on port %d", *target.port);
        return 502;
    }
    const char* ct = mg_get_header(conn, "Content-Type");
    const char* auth = mg_get_header(conn, "Authorization");
    // local_uri is the PATH only - the query string travels separately and
    // dropping it turns every parameterised command into a 400 upstream.
    std::string uri = ri->local_uri ? ri->local_uri : "/";
    if (ri->query_string && *ri->query_string) uri += std::string("?") + ri->query_string;
    mg_printf(up, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n",
              ri->request_method, uri.c_str());
    mg_printf(up, "Content-Length: %u\r\n", (unsigned)body.size());
    if (ct) mg_printf(up, "Content-Type: %s\r\n", ct);
    if (auth) mg_printf(up, "Authorization: %s\r\n", auth);
    mg_printf(up, "\r\n");
    if (!body.empty()) mg_write(up, body.data(), body.size());

    if (mg_get_response(up, err, sizeof err, 10000) < 0) {
        mg_close_connection(up);
        mg_send_http_error(conn, 504, "WHIP room did not answer");
        return 504;
    }
    // COPY the status line and the headers we relay BEFORE reading the body.
    // mg_get_response_info hands back pointers into the connection's receive
    // buffer, and mg_read of a large body (the preview JPEG) recycles that
    // buffer - after which status_text and every header pointer dangle. This
    // crashed intermittently under the moderator page's polling, in mg_printf,
    // at exactly the moment the buffer happened to be unmapped rather than
    // merely stale.
    const mg_response_info* rsp = mg_get_response_info(up);
    if (!rsp) {
        mg_close_connection(up);
        mg_send_http_error(conn, 502, "no upstream response");
        return 502;
    }
    const int scode = rsp->status_code;
    const std::string stext = rsp->status_text ? rsp->status_text : "";
    std::string relayHeaders;
    for (int i = 0; i < rsp->num_headers; ++i) {
        const char* hn = rsp->http_headers[i].name;
        const char* hv = rsp->http_headers[i].value;
        if (!hn || !hv) continue;
        if (!_stricmp(hn, "Content-Type") || !_stricmp(hn, "Location") ||
            !_stricmp(hn, "WWW-Authenticate"))
            relayHeaders += std::string(hn) + ": " + hv + "\r\n";
    }
    std::string rbody;
    {
        char buf[8192];
        int n;
        while ((n = mg_read(up, buf, sizeof buf)) > 0) rbody.append(buf, (size_t)n);
    }
    mg_printf(conn, "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Length: %u\r\n%s\r\n",
              scode, stext.c_str(), (unsigned)rbody.size(), relayHeaders.c_str());
    if (!rbody.empty()) mg_write(conn, rbody.data(), rbody.size());
    mg_close_connection(up);
    return scode;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = exeDir() + "\\webroot";
    std::string ports = "8080,8443s";
    std::string pem = exeDir() + "\\web.pem";
    std::string name = "vkrender";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--root")) root = argv[i + 1];
        else if (!strcmp(argv[i], "--ports")) ports = argv[i + 1];
        else if (!strcmp(argv[i], "--pem")) pem = argv[i + 1];
        else if (!strcmp(argv[i], "--name")) name = argv[i + 1];
        else if (!strcmp(argv[i], "--whip-port")) g_whipPort = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--control-port")) g_controlPort = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--admin-pin")) g_adminPin = argv[i + 1];
    }

    if (!fileExists(root)) {
        fprintf(stderr, "vkr_web: webroot '%s' does not exist\n", root.c_str());
        return 1;
    }
    const bool wantTls = ports.find('s') != std::string::npos;
    if (wantTls && !fileExists(pem)) {
        printf("vkr_web: generating self-signed certificate '%s'\n", pem.c_str());
        if (!generatePem(pem, name)) {
            fprintf(stderr, "vkr_web: certificate generation failed\n");
            return 1;
        }
    }

    mg_init_library(wantTls ? MG_FEATURES_TLS : MG_FEATURES_DEFAULT);
    const char* options[] = {
        "document_root", root.c_str(),
        "listening_ports", ports.c_str(),
        "ssl_certificate", pem.c_str(),
        "enable_directory_listing", "yes", // /downloads/ is a plain listing
        "num_threads", "8",
        nullptr};
    mg_context* ctx = mg_start(nullptr, nullptr, wantTls ? options : options + 0);
    if (!ctx) {
        fprintf(stderr, "vkr_web: mg_start failed (ports '%s' in use?)\n", ports.c_str());
        return 1;
    }
    mg_set_request_handler(ctx, "/whip/", proxyHandler, &g_whipTarget);
    mg_set_request_handler(ctx, "/session/", proxyHandler, &g_whipTarget);
    // M42: the moderator console's API, behind the admin PIN.
    mg_set_request_handler(ctx, "/api/", proxyHandler, &g_apiTarget);

    char host[256] = "localhost";
    DWORD hn = sizeof(host);
    GetComputerNameExA(ComputerNameDnsHostname, host, &hn);
    printf("vkr_web: serving '%s'\n", root.c_str());
    printf("vkr_web:   sender app   https://%s:8443/  (accept the certificate once)\n", host);
    printf("vkr_web:   downloads    http://%s:8080/downloads/\n", host);
    printf("vkr_web:   whip proxy   /whip/* + /session/* -> 127.0.0.1:%d (same-origin)\n",
           g_whipPort);
    if (!g_adminPin.empty())
        printf("vkr_web:   moderator    https://%s:8443/admin/  (/api/* -> 127.0.0.1:%d, "
               "Bearer-auth)\n",
               host, g_controlPort);
    printf("vkr_web: screen share NEEDS the https address on other machines - "
           "getDisplayMedia only exists in a secure context\n");

    // Run until killed; the appliance's job object owns our lifetime.
    for (;;) Sleep(60000);
}

#else
int main() { return 0; }
#endif
