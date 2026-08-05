cmake_minimum_required(VERSION 3.22)
project(vkrender_airplay LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (this file lives at android/airplay/src/main/cpp)
get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)
set(UX ${REPO}/third_party/uxplay/lib)
set(PLIST ${REPO}/third_party/libplist)

# libcrypto: reuse the OpenSSL static lib from the GStreamer Android prebuilts
# (already fetched for the decoder APK). Header tree lives under include/.
set(GST_ANDROID ${REPO}/android/gstreamer-android/${ANDROID_ABI})
if(NOT EXISTS ${GST_ANDROID}/lib/libcrypto.a)
    message(FATAL_ERROR "libcrypto.a not found in the GStreamer Android prebuilts at "
                        "${GST_ANDROID}/lib. Run scripts/fetch_gstreamer_android.sh (unpruned).")
endif()

# --- libplist (bundled parser used by the airplay handlers) ------------------
add_library(vkr_plist STATIC
    ${PLIST}/src/base64.c ${PLIST}/src/bytearray.c ${PLIST}/src/bplist.c
    ${PLIST}/src/hashtable.c ${PLIST}/src/jplist.c ${PLIST}/src/jsmn.c
    ${PLIST}/src/oplist.c ${PLIST}/src/plist.c ${PLIST}/src/ptrarray.c
    ${PLIST}/src/time64.c ${PLIST}/src/xplist.c
    ${PLIST}/src/out-default.c ${PLIST}/src/out-limd.c ${PLIST}/src/out-plutil.c
    ${PLIST}/libcnary/cnary.c ${PLIST}/libcnary/node.c ${PLIST}/libcnary/node_list.c)
target_include_directories(vkr_plist PUBLIC
    ${PLIST}/include ${PLIST}/src ${PLIST}/libcnary/include)
# Android's bionic provides strndup (HAVE_STRNDUP suppresses the bundled
# fallback), and PACKAGE_VERSION replaces the autotools config.h value.
target_compile_definitions(vkr_plist PRIVATE
    PACKAGE_VERSION="2.6.0" HAVE_STRNDUP=1 HAVE_VASPRINTF=1 HAVE_ASPRINTF=1)

# --- UxPlay raop server core (LGPL/GPL; see third_party/uxplay/LICENSE) -------
add_library(vkr_raop STATIC
    ${UX}/byteutils.c ${UX}/crypto.c ${UX}/fairplay_playfair.c
    ${UX}/http_request.c ${UX}/http_response.c ${UX}/httpd.c ${UX}/logger.c
    ${UX}/mirror_buffer.c ${UX}/netutils.c ${UX}/pairing.c ${UX}/raop.c
    ${UX}/raop_buffer.c ${UX}/raop_ntp.c ${UX}/raop_rtp.c
    ${UX}/raop_rtp_mirror.c ${UX}/srp.c ${UX}/utils.c
    ${UX}/llhttp/api.c ${UX}/llhttp/http.c ${UX}/llhttp/llhttp.c
    ${UX}/playfair/hand_garble.c ${UX}/playfair/modified_md5.c
    ${UX}/playfair/omg_hax.c ${UX}/playfair/playfair.c ${UX}/playfair/sap_hash.c
    # Android dnssd shim (NsdManager does the actual registration).
    ${CMAKE_CURRENT_SOURCE_DIR}/dnssd_android.c)
target_include_directories(vkr_raop PUBLIC
    ${UX} ${UX}/llhttp ${UX}/playfair ${GST_ANDROID}/include)
target_link_libraries(vkr_raop PUBLIC vkr_plist
    ${GST_ANDROID}/lib/libssl.a ${GST_ANDROID}/lib/libcrypto.a)
# The vendored core has warnings we don't want to fix in third-party code.
target_compile_options(vkr_raop PRIVATE -Wno-everything)

# --- the app's native lib ----------------------------------------------------
add_library(vkrender_airplay SHARED
    airplay_jni.cpp
    airplay_video.cpp)
target_include_directories(vkrender_airplay PRIVATE ${UX})
target_link_libraries(vkrender_airplay PRIVATE
    vkr_raop
    android log mediandk nativewindow)
