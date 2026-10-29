// M13 test client: feeds the vkrenderer Wayland compositor an ARGB8888
// dma-buf (allocated via /dev/udmabuf, LINEAR layout) through the
// zwp_linux_dmabuf_v1 protocol - exercising the zero-copy import path
// without needing a VAAPI driver that exports dma-bufs.
//
//   WAYLAND_DISPLAY=vkrender-0 ./vkr_wl_dmabuf_client [seconds]
//
// Draws an animated diagonal gradient. CPU writes land in the same pages the
// compositor's GPU samples (udmabuf = memfd pages as a dma-buf).

#define _GNU_SOURCE // memfd_create, F_ADD_SEALS
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/memfd.h>
#include <linux/udmabuf.h>

#include <wayland-client.h>
#include "framelink.h"
#include "framelink_dmabuf.h"

#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define WIDTH 640
#define HEIGHT 360
#define STRIDE (WIDTH * 4)
#define FOURCC_ARGB8888 0x34325241u
#define MOD_LINEAR 0ull

static struct wl_compositor* compositor;
static struct xdg_wm_base* wmBase;
static struct zwp_linux_dmabuf_v1* dmabuf;
static int configured;
static int frameDue = 1;

static void registryGlobal(void* data, struct wl_registry* reg, uint32_t name,
                           const char* iface, uint32_t version) {
    (void)data;
    (void)version;
    if (!strcmp(iface, wl_compositor_interface.name))
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, xdg_wm_base_interface.name))
        wmBase = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name))
        dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 3);
}
static void registryRemove(void* d, struct wl_registry* r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener registryListener = {registryGlobal, registryRemove};

static void xdgSurfaceConfigure(void* data, struct xdg_surface* s, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}
static const struct xdg_surface_listener xdgSurfaceListener = {xdgSurfaceConfigure};

static void toplevelConfigure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h,
                              struct wl_array* s) {
    (void)d; (void)t; (void)w; (void)h; (void)s;
}
static void toplevelClose(void* d, struct xdg_toplevel* t) {
    (void)d; (void)t;
    exit(0);
}
static const struct xdg_toplevel_listener toplevelListener = {toplevelConfigure, toplevelClose};

static void frameDone(void* data, struct wl_callback* cb, uint32_t t) {
    (void)data; (void)t;
    wl_callback_destroy(cb);
    frameDue = 1;
}
static const struct wl_callback_listener frameListener = {frameDone};

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int nv12 = argc > 2 && !strcmp(argv[2],"nv12");
    unsigned stride = nv12 ? WIDTH : STRIDE;

    // Use real driver-allocated DMA-BUFs; NVIDIA cannot import arbitrary
    // udmabuf pages as Vulkan image memory on every driver.
    flChannel *consumer=NULL,*producer=NULL;
    char channel[80];snprintf(channel,sizeof(channel),"wl-test-%d",getpid());
    if(flCreateChannelEx(channel,WIDTH,HEIGHT,nv12?FL_FORMAT_NV12:FL_FORMAT_BGRA8,1,FL_MAP_CPU,&consumer)!=FL_OK)return 1;
    for(int i=0;i<100&&!producer;i++){flOpenProducer(channel,&producer);usleep(10000);}
    flBuffer frameBuffer={0};
    if(!producer || flAcquireBuffer(producer,&frameBuffer,1000)!=FL_OK)return 1;
    flDmabuf desc={0};if(flImageDmabuf(frameBuffer.image,&desc)!=FL_OK)return 1;

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect (WAYLAND_DISPLAY set?)\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !wmBase || !dmabuf) {
        fprintf(stderr, "missing globals (compositor=%p wm=%p dmabuf=%p)\n", (void*)compositor,
                (void*)wmBase, (void*)dmabuf);
        return 1;
    }

    struct wl_surface* surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
    xdg_surface_add_listener(xdgSurface, &xdgSurfaceListener, NULL);
    struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
    xdg_toplevel_add_listener(toplevel, &toplevelListener, NULL);
    xdg_toplevel_set_title(toplevel, "vkr dmabuf test");
    wl_surface_commit(surface);
    while (!configured && wl_display_dispatch(display) != -1) {
    }

    struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    for(unsigned plane=0;plane<desc.planes;plane++)
        zwp_linux_buffer_params_v1_add(params,desc.fd[plane],plane,desc.offset[plane],desc.stride[plane],
            (uint32_t)(desc.modifier>>32),(uint32_t)desc.modifier);
    struct wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
        params, WIDTH, HEIGHT, nv12 ? 0x3231564eu : FOURCC_ARGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    fprintf(stderr, "dmabuf client: streaming %dx%d ARGB LINEAR for %ds\n", WIDTH, HEIGHT,
            seconds);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint32_t frame = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= seconds) break;
        if (frameDue) {
            frameDue = 0;
            if(frame==0) { // Immutable fixture: never overwrite an in-flight wl_buffer.
            flMappedImage mapped={0};if(flImageMapPlanes(frameBuffer.image,&mapped)!=FL_OK)return 1;
            if(nv12) {
                for(unsigned y=0;y<HEIGHT;y++)memset((char*)mapped.plane[0].pixels+y*mapped.plane[0].stride,81+frame%40,WIDTH);
                for(unsigned y=0;y<HEIGHT/2;y++)for(unsigned x=0;x<WIDTH;x+=2){
                    unsigned char* uv=(unsigned char*)mapped.plane[1].pixels+y*mapped.plane[1].stride+x;
                    uv[0]=90;uv[1]=240;
                }
            } else for (int y = 0; y < HEIGHT; ++y)
                for (int x = 0; x < WIDTH; ++x) {
                    uint8_t r = (uint8_t)(x + frame * 2);
                    uint8_t g = (uint8_t)(y + frame);
                    uint8_t b = (uint8_t)(x + y);
                    ((uint32_t*)((char*)mapped.plane[0].pixels+y*mapped.plane[0].stride))[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            flImageUnmap(frameBuffer.image);
            }
            ++frame;
            struct wl_callback* cb = wl_surface_frame(surface);
            wl_callback_add_listener(cb, &frameListener, NULL);
            wl_surface_attach(surface, buffer, 0, 0);
            wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
            wl_surface_commit(surface);
            wl_display_flush(display);
        }
        if (wl_display_dispatch(display) == -1) break;
    }
    fprintf(stderr, "dmabuf client: sent %u frames\n", frame);
    wl_display_disconnect(display);
    flClose(producer);flClose(consumer);
    return 0;
}
