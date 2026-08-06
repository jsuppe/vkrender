#!/usr/bin/env bash
# M22 Linux WHIP room + mDNS advertisement.
#
# Runs the multi-presenter WHIP receiver (vkr_webrtc: whip_room ingest on
# webrtcbin, one waylandsink per presenter into the M13 compositor) alongside
# an avahi `_vkrender-whip._tcp` advertisement so the Flutter sender
# auto-discovers it - matching the Windows (DnsServiceRegister) and Android
# (NsdManager) receivers.
#
# Prereqs:
#   - the compositor is already running:  vkrenderer --wayland vkrender-0 ...
#   - gstreamer1.0-plugins-bad (webrtcbin) + gstreamer1.0-nice installed
#   - avahi-daemon running and avahi-utils installed (avahi-publish-service)
#
# Env overrides: PORT (8889), NAME (vkrender), MAX (12), WAYLAND_DISPLAY
# (vkrender-0), VKR_WEBRTC (path to the binary, default ./build/vkr_webrtc).
set -u

PORT="${PORT:-8889}"
NAME="${NAME:-vkrender}"
MAX="${MAX:-12}"
BIN="${VKR_WEBRTC:-./build/vkr_webrtc}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-vkrender-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# Advertise on the LAN; kill the publisher when this script exits.
avahi-publish-service "$NAME" _vkrender-whip._tcp "$PORT" "path=/whip/endpoint" &
AVAHI_PID=$!
cleanup() { kill "$AVAHI_PID" 2>/dev/null; }
trap cleanup EXIT INT TERM

echo "webrtc: room on http://0.0.0.0:$PORT/whip/endpoint (advertised as '$NAME')"
exec "$BIN" "$PORT" "$MAX"
