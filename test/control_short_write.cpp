// Test-only linker wrapper: transmit a real half-JPEG and report that short
// write to ControlApi. Subsequent requests must regain the streaming lease.
#include "civetweb.h"
#include <cstdio>
extern "C" int __real_mg_write(mg_connection*, const void*, size_t);
extern "C" int __wrap_mg_write(mg_connection* connection, const void* data, size_t size) {
    const auto* bytes=static_cast<const unsigned char*>(data);
    if(size>4 && bytes[0]==0xff && bytes[1]==0xd8) {
        const int sent=__real_mg_write(connection,data,size/2);
        std::fprintf(stderr,"injected short JPEG write: %d/%zu\n",sent,size);
        return sent;
    }
    return __real_mg_write(connection,data,size);
}
