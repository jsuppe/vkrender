// M32: the other half of "pick what to share" - proving the id the picker
// returns actually reaches the capturer.
//
// The picker test covers the UI; this covers the plumbing between it and
// libwebrtc. Both stop at the platform channel, which is the honest boundary:
// whether libwebrtc can capture a given window id is a native question that
// only a live sender against a real receiver can answer.
//
// Each case lets capture fail at the channel on purpose. start() is only run
// as far as the capture step; going further would mean standing up a whole
// peer connection to observe an argument map that has already been recorded.
import 'dart:io' show Platform;

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:vkrender_sender/webrtc_sender.dart';

const _channel = MethodChannel('FlutterWebRTC.Method');

class _Recorder {
  final List<String> calls = [];
  Map<String, dynamic>? displayConstraints;
  Map<String, dynamic>? userConstraints;
  List<Map<String, dynamic>> screens = [
    {
      'id': 'screen:first',
      'name': 'Entire screen 1',
      'type': 'screen',
      'thumbnailSize': {'width': 0, 'height': 0},
    },
    {
      'id': 'screen:second',
      'name': 'Entire screen 2',
      'type': 'screen',
      'thumbnailSize': {'width': 0, 'height': 0},
    },
  ];

  void install() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_channel, (call) async {
      calls.add(call.method);
      switch (call.method) {
        case 'initialize':
          return null;
        case 'getDesktopSources':
          return {'sources': screens};
        case 'getDisplayMedia':
          displayConstraints = Map<String, dynamic>.from(
              (call.arguments as Map)['constraints'] as Map);
          throw PlatformException(code: 'STOP', message: 'stop here');
        case 'getUserMedia':
          userConstraints = Map<String, dynamic>.from(
              (call.arguments as Map)['constraints'] as Map);
          throw PlatformException(code: 'STOP', message: 'stop here');
      }
      return null;
    });
  }

  void remove() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_channel, null);
  }
}

/// Run start() and swallow the deliberate capture failure.
Future<List<SenderState>> _run(WebrtcSender Function(void Function(SenderState))
        make,
    Future<void> Function(WebrtcSender) body) async {
  final states = <SenderState>[];
  final sender = make(states.add);
  try {
    await body(sender);
  } catch (_) {
    // expected: capture was stopped at the channel
  }
  return states;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late _Recorder rec;
  final endpoint = Uri.parse('http://127.0.0.1:8889/whip');

  WebrtcSender make(void Function(SenderState) sink) =>
      WebrtcSender(onState: (s, {detail}) => sink(s));

  setUp(() {
    rec = _Recorder();
    rec.install();
  });

  tearDown(() => rec.remove());

  test('a chosen source id becomes the exact deviceId constraint', () async {
    await _run(make,
        (s) => s.start(endpoint, CaptureSource.screen,
            desktopSourceId: 'window:0x00A1B2'));

    final video = rec.displayConstraints!['video'] as Map;
    expect(video['deviceId'], {'exact': 'window:0x00A1B2'});
    // The frame rate the sender asks for must survive alongside the id.
    expect((video['mandatory'] as Map)['frameRate'], 30.0);
    expect(rec.displayConstraints!['audio'], isFalse);

    // A chosen id must NOT trigger enumeration - that would be a wasted
    // round trip, and on a busy desktop it is a slow one.
    expect(rec.calls, isNot(contains('getDesktopSources')));
  }, skip: !Platform.isWindows ? 'desktop capture path is desktop-only' : null);

  test('no chosen id falls back to the first enumerated screen', () async {
    await _run(make, (s) => s.start(endpoint, CaptureSource.screen));

    expect(rec.calls, contains('getDesktopSources'));
    final video = rec.displayConstraints!['video'] as Map;
    expect(video['deviceId'], {'exact': 'screen:first'});
  }, skip: !Platform.isWindows ? 'desktop capture path is desktop-only' : null);

  test('no sources at all fails loudly rather than capturing nothing',
      () async {
    rec.screens = [];
    Object? err;
    final sender = make((_) {});
    try {
      await sender.start(endpoint, CaptureSource.screen);
    } catch (e) {
      err = e;
    }
    expect('$err', contains('no screen capture sources found'));
    expect(rec.calls, isNot(contains('getDisplayMedia')));
  }, skip: !Platform.isWindows ? 'desktop capture path is desktop-only' : null);

  test('camera capture ignores the desktop id entirely', () async {
    await _run(make,
        (s) => s.start(endpoint, CaptureSource.camera,
            desktopSourceId: 'window:0x00A1B2'));

    expect(rec.calls, contains('getUserMedia'));
    expect(rec.calls, isNot(contains('getDisplayMedia')));
    expect(rec.calls, isNot(contains('getDesktopSources')));
    expect((rec.userConstraints!['video'] as Map)['facingMode'], 'user');
  });

  test('a failed capture reports failed LAST, after the teardown', () async {
    final states = await _run(make,
        (s) => s.start(endpoint, CaptureSource.camera));

    // stop() emits `stopped` during teardown, so the failure detail would be
    // lost if the order were not deliberate.
    expect(states.first, SenderState.capturing);
    expect(states.last, SenderState.failed);
  });
}
