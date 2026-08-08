// The reusable heart of the sender: capture a source, publish it over WebRTC,
// and negotiate with a WHIP receiver. Platform-agnostic -- the same code runs
// on Android, iOS, desktop and web because flutter_webrtc wraps libwebrtc
// (native) or the browser's WebRTC (web) behind one API.
import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'screen_capture_service.dart';
import 'whip_client.dart';

enum CaptureSource { screen, camera }

/// Lifecycle states surfaced to the UI. `pinRequired` means the receiver is
/// showing a PIN on its screen and the user must type it here (M25).
enum SenderState {
  idle,
  capturing,
  signalling,
  connecting,
  live,
  failed,
  stopped,
  pinRequired
}

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

  static bool get _isDesktop =>
      !kIsWeb && (Platform.isWindows || Platform.isLinux || Platform.isMacOS);

  Future<MediaStream> _capture(CaptureSource src, {String? desktopSourceId}) async {
    if (src == CaptureSource.screen) {
      // Android 14+ requires the mediaProjection foreground service to be
      // running *before* getDisplayMedia; no-op elsewhere.
      await ScreenCaptureService.start();
      _fgsRunning = true;
      // Desktop flutter_webrtc does not honor a bare {'video': true} - the
      // capturer needs an explicit source id from desktopCapturer (found
      // live: hourglass -> stopped with zero frames sent).
      //
      // M32: that id now comes from the picker, so this can be one WINDOW or a
      // chosen DISPLAY. Falling back to the first screen keeps the old
      // behaviour when no choice was made (e.g. a caller that skips the UI).
      if (_isDesktop) {
        var id = desktopSourceId;
        if (id == null) {
          final sources =
              await desktopCapturer.getSources(types: [SourceType.Screen]);
          if (sources.isEmpty) {
            throw StateError('no screen capture sources found');
          }
          id = sources.first.id;
        }
        return navigator.mediaDevices.getDisplayMedia({
          'video': {
            'deviceId': {'exact': id},
            'mandatory': {'frameRate': 30.0},
          },
          'audio': false,
        });
      }
      // Mobile raises the OS consent dialog; web raises the browser's picker.
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

  /// [desktopSourceId] picks a specific display or window on desktop (M32);
  /// null falls back to the primary screen. Ignored on mobile and web, where
  /// the platform's own consent dialog does the choosing.
  Future<void> start(Uri whipEndpoint, CaptureSource source,
      {String? pin, String? desktopSourceId}) async {
    try {
      _emit(SenderState.capturing);
      final stream = await _capture(source, desktopSourceId: desktopSourceId);
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

      final whip = WhipSession(whipEndpoint, pin: pin);
      _whip = whip;
      final answerSdp = await whip.exchangeOffer(local!.sdp!);

      _emit(SenderState.connecting);
      await pc.setRemoteDescription(RTCSessionDescription(answerSdp, 'answer'));
    } on WhipPinRequired catch (e) {
      // Keep the failure quiet: the UI collects the PIN and calls start()
      // again with it. Capture is torn down so the user isn't left recording.
      await stop();
      _emit(SenderState.pinRequired, e.toString());
      rethrow;
    } catch (e) {
      // Tear down first, then report - stop() emits `stopped`, and the
      // failure detail must be the LAST state the UI sees.
      await stop();
      _emit(SenderState.failed, e.toString());
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
