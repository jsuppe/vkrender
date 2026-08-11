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
        FILE* f = fopen(path.c_str(), "wb");
        ok = f && PEM_write_X509(f, x509) && PEM_write_PrivateKey(f, pkey, nullptr, nullptr,
                                                                  0, nullptr, nullptr);
        if (f) fclose(f);
    }
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok;
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

    char host[256] = "localhost";
    DWORD hn = sizeof(host);
    GetComputerNameExA(ComputerNameDnsHostname, host, &hn);
    printf("vkr_web: serving '%s'\n", root.c_str());
    printf("vkr_web:   sender app   https://%s:8443/  (accept the certificate once)\n", host);
    printf("vkr_web:   downloads    http://%s:8080/downloads/\n", host);
    printf("vkr_web: screen share NEEDS the https address on other machines - "
           "getDisplayMedia only exists in a secure context\n");

    // Run until killed; the appliance's job object owns our lifetime.
    for (;;) Sleep(60000);
}

#else
int main() { return 0; }
#endif
