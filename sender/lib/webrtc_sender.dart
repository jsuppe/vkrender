// The reusable heart of the sender: capture a source, publish it over WebRTC,
// and negotiate with a WHIP receiver. Platform-agnostic -- the same code runs
// on Android, iOS, desktop and web because flutter_webrtc wraps libwebrtc
// (native) or the browser's WebRTC (web) behind one API.
import 'dart:async';

import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'screen_capture_service.dart';
import 'whip_client.dart';

enum CaptureSource { screen, camera }

/// Lifecycle states surfaced to the UI.
enum SenderState { idle, capturing, signalling, connecting, live, failed, stopped }

class WebrtcSender {
  RTCPeerConnection? _pc;
  MediaStream? _stream;
  WhipSession? _whip;

  final void Function(SenderState state, {String? detail})? onState;

  WebrtcSender({this.onState});

  /// The captured local stream, for on-device preview.
  MediaStream? get stream => _stream;

  void _emit(SenderState s, [String? detail]) => onState?.call(s, detail: detail);

  bool _fgsRunning = false;

  Future<MediaStream> _capture(CaptureSource src) async {
    if (src == CaptureSource.screen) {
      // Android 14+ requires the mediaProjection foreground service to be
      // running *before* getDisplayMedia; no-op elsewhere.
      await ScreenCaptureService.start();
      _fgsRunning = true;
      // On mobile this raises the OS screen-capture consent dialog; on desktop
      // flutter_webrtc surfaces its own source picker; on web the browser's.
      return navigator.mediaDevices.getDisplayMedia({'video': true, 'audio': false});
    }
    return navigator.mediaDevices.getUserMedia({
      'video': {'facingMode': 'user'},
      'audio': false,
    });
  }

  /// Reorder the video transceiver's codecs so H.264 is offered first. The
  /// Android receiver's pipeline is `rtph264depay ! h264parse` -- H.264 only --
  /// so without this a default VP8 negotiation would connect but decode
  /// nothing there. Windows/Linux use decodebin and tolerate anything, but we
  /// want one sender that works against all three receivers.
  Future<bool> _preferH264(RTCPeerConnection pc) async {
    RTCRtpCapabilities caps;
    try {
      caps = await getRtpSenderCapabilities('video');
    } catch (_) {
      return false;
    }
    final codecs = caps.codecs ?? [];
    if (codecs.isEmpty) return false;

    final h264 = <RTCRtpCodecCapability>[];
    final others = <RTCRtpCodecCapability>[];
    for (final c in codecs) {
      if ((c.mimeType).toLowerCase() == 'video/h264') {
        h264.add(c);
      } else {
        others.add(c);
      }
    }
    if (h264.isEmpty) return false; // platform has no H.264 encoder

    final transceivers = await pc.getTransceivers();
    for (final t in transceivers) {
      if (t.sender.track?.kind == 'video') {
        await t.setCodecPreferences([...h264, ...others]);
      }
    }
    return true;
  }

  /// WHIP is non-trickle: the POSTed offer must already carry ICE candidates,
  /// so wait for gathering to finish (bounded, LAN gathering is near-instant).
  Future<void> _waitIceGathering(RTCPeerConnection pc) async {
    if (pc.iceGatheringState ==
        RTCIceGatheringState.RTCIceGatheringStateComplete) {
      return;
    }
    final done = Completer<void>();
    pc.onIceGatheringState = (state) {
      if (state == RTCIceGatheringState.RTCIceGatheringStateComplete &&
          !done.isCompleted) {
        done.complete();
      }
    };
    // Fallback: proceed after a short budget even if the "complete" event is
    // missed (host candidates on a LAN are gathered in tens of ms).
    Timer(const Duration(seconds: 3), () {
      if (!done.isCompleted) done.complete();
    });
    await done.future;
  }

  Future<void> start(Uri whipEndpoint, CaptureSource source) async {
    try {
      _emit(SenderState.capturing);
      final stream = await _capture(source);
      _stream = stream;

      final pc = await createPeerConnection({
        // LAN casting: no STUN/TURN. Host candidates reach the receiver
        // directly, which is exactly the vkrender deployment.
        'iceServers': <Map<String, dynamic>>[],
        'sdpSemantics': 'unified-plan',
      });
      _pc = pc;
      pc.onConnectionState = (s) {
        switch (s) {
          case RTCPeerConnectionState.RTCPeerConnectionStateConnected:
            _emit(SenderState.live);
            break;
          case RTCPeerConnectionState.RTCPeerConnectionStateFailed:
            _emit(SenderState.failed, 'peer connection failed');
            break;
          case RTCPeerConnectionState.RTCPeerConnectionStateDisconnected:
          case RTCPeerConnectionState.RTCPeerConnectionStateClosed:
            _emit(SenderState.stopped);
            break;
          default:
            break;
        }
      };

      for (final track in stream.getVideoTracks()) {
        await pc.addTrack(track, stream);
      }
      await _preferH264(pc);

      _emit(SenderState.signalling);
      final offer = await pc.createOffer({});
      await pc.setLocalDescription(offer);
      await _waitIceGathering(pc);
      final local = await pc.getLocalDescription();

      final whip = WhipSession(whipEndpoint);
      _whip = whip;
      final answerSdp = await whip.exchangeOffer(local!.sdp!);

      _emit(SenderState.connecting);
      await pc.setRemoteDescription(RTCSessionDescription(answerSdp, 'answer'));
    } catch (e) {
      _emit(SenderState.failed, e.toString());
      await stop();
      rethrow;
    }
  }

  Future<void> stop() async {
    await _whip?.stop();
    _whip = null;
    for (final t in _stream?.getTracks() ?? const []) {
      try {
        await t.stop();
      } catch (_) {}
    }
    try {
      await _stream?.dispose();
    } catch (_) {}
    _stream = null;
    try {
      await _pc?.close();
    } catch (_) {}
    _pc = null;
    if (_fgsRunning) {
      _fgsRunning = false;
      await ScreenCaptureService.stop();
    }
    _emit(SenderState.stopped);
  }
}
