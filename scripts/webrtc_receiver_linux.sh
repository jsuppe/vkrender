#!/usr/bin/env bash
# M20/M21 Linux WHIP receiver + mDNS advertisement.
#
# The Linux WebRTC receiver is one gst-launch line: whipserversrc decoded into
# waylandsink, landing on the M13 compositor exactly like AirPlay via uxplay.
# This wrapper adds the `_vkrender-whip._tcp` advertisement (via avahi) so the
# Flutter sender auto-discovers it, matching the Windows (DnsServiceRegister)
# and Android (NsdManager) receivers.
#
# Prereqs:
#   - the compositor is already running:  vkrenderer --wayland vkrender-0 ...
#   - gst-plugin-webrtc providing whipserversrc is on the GStreamer plugin path
#     (Ubuntu packages lack it; build with `cargo build --release -p
#     gst-plugin-webrtc` and drop libgstrswebrtc.so in the plugin dir).
#   - avahi-daemon running and avahi-utils installed (avahi-publish-service).
#
# Env overrides: PORT (8889), NAME (vkrender), WAYLAND_DISPLAY (vkrender-0).
set -u

PORT="${PORT:-8889}"
NAME="${NAME:-vkrender}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-vkrender-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# Advertise on the LAN; kill the publisher when this script exits.
avahi-publish-service "$NAME" _vkrender-whip._tcp "$PORT" "path=/whip/endpoint" &
AVAHI_PID=$!
cleanup() { kill "$AVAHI_PID" 2>/dev/null; }
trap cleanup EXIT INT TERM

echo "webrtc: WHIP endpoint on http://0.0.0.0:$PORT/whip/endpoint (advertised as '$NAME')"

# whipserversrc runs the WHIP HTTP endpoint + full WebRTC stack in-process.
exec gst-launch-1.0 -e \
  whipserversrc name=w signaller::host-addr="http://0.0.0.0:$PORT" \
    ! queue ! decodebin ! videoconvert ! waylandsink
