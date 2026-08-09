// M32: verification for the "choose what to share" picker.
//
// These are widget tests rather than an on-screen check on purpose. The picker
// is pure Dart UI over one plugin call, so the interesting behaviour -- which
// sources get listed, under which heading, and exactly what id/type comes back
// out of a tap -- is fully observable without a display. That matters here:
// this machine is usually headless, and a GDI screen grab cannot see Flutter's
// DirectComposition-presented content anyway, so "look at the window" is not an
// instrument that can answer these questions.
//
// What this does NOT cover: whether libwebrtc can actually capture the id it is
// handed. That is a native concern and needs a live sender against a receiver.
import 'dart:convert';

import 'package:flutter/foundation.dart'
    show debugDefaultTargetPlatformOverride, TargetPlatform;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:vkrender_sender/source_picker.dart';

const _channel = MethodChannel('FlutterWebRTC.Method');

/// A real 1x1 PNG. Image.memory decodes for real in a widget test, so feeding
/// it arbitrary bytes would raise an async decode error and fail the test for
/// the wrong reason.
final Uint8List _png = base64Decode(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==');

Map<String, dynamic> _src(String id, String name, String type,
        {bool thumb = true}) =>
    {
      'id': id,
      'name': name,
      'type': type, // 'screen' | 'window'
      'thumbnailSize': {'width': 320, 'height': 180},
      if (thumb) 'thumbnail': _png,
    };

/// Records what the picker asked the plugin for, and replies with [sources]
/// filtered by the requested type -- mirroring the real plugin, which is what
/// makes the two-call (screens, then windows) structure observable.
class FakeCapturer {
  FakeCapturer(this.sources);

  List<Map<String, dynamic>> sources;
  String? failWith;
  final List<List<String>> typeRequests = [];
  final List<Map<dynamic, dynamic>?> thumbRequests = [];

  void install() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_channel, (call) async {
      switch (call.method) {
        case 'initialize':
          return null;
        case 'getDesktopSources':
          if (failWith != null) {
            throw PlatformException(code: 'ERROR', message: failWith);
          }
          final args = call.arguments as Map;
          final types = (args['types'] as List).cast<String>();
          typeRequests.add(types);
          thumbRequests.add(args['thumbnailSize'] as Map?);
          return {
            'sources': sources.where((s) => types.contains(s['type'])).toList(),
          };
      }
      return null;
    });
  }

  void remove() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_channel, null);
  }
}

/// Holds the picker's return value across the async gap.
class _Host {
  DesktopSourceChoice? choice;
  bool returned = false;
}

/// Pumps a page whose one button opens the picker. Going through showDialog
/// rather than building the dialog widget directly is deliberate: "cancel
/// returns null" and "a tap pops with a value" are Navigator behaviours that
/// building the widget alone would not exercise.
Future<_Host> _openPicker(WidgetTester tester) async {
  final host = _Host();
  await tester.pumpWidget(MaterialApp(
    home: Builder(
      builder: (context) => Scaffold(
        body: Center(
          child: ElevatedButton(
            onPressed: () async {
              host.choice = await showDesktopSourcePicker(context);
              host.returned = true;
            },
            child: const Text('open'),
          ),
        ),
      ),
    ),
  ));
  await tester.tap(find.text('open'));
  await tester.pumpAndSettle();
  return host;
}

/// Pick a source by its displayed name.
///
/// The scroll is not incidental. The dialog asks for 720x460 but takes whatever
/// the window allows, and two grids of tiles do not both fit on a short window,
/// so the windows section really is below the fold there. Tapping without
/// scrolling first hit-tests empty space and silently does nothing - which is
/// exactly how the first run of this test failed.
Future<void> _pick(WidgetTester tester, String name) async {
  final target = find.text(name);
  expect(target, findsOneWidget, reason: '$name should be listed');
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
  await tester.pumpAndSettle();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late FakeCapturer fake;

  setUp(() {
    fake = FakeCapturer([
      _src('screen:0', 'Entire screen 1', 'screen'),
      _src('screen:1', 'Entire screen 2', 'screen'),
      _src('window:aaa', 'Notepad', 'window'),
      _src('window:bbb', 'vkrender sender', 'window'),
      _src('window:ccc', 'No Thumb App', 'window', thumb: false),
    ]);
    fake.install();
  });

  tearDown(() {
    fake.remove();
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets('lists displays and windows under separate headings', (t) async {
    await _openPicker(t);

    expect(find.text('Choose what to share'), findsOneWidget);
    expect(find.text('Displays (2)'), findsOneWidget);
    expect(find.text('Windows (3)'), findsOneWidget);

    expect(find.text('Entire screen 1'), findsOneWidget);
    expect(find.text('Notepad'), findsOneWidget);
    expect(find.text('No Thumb App'), findsOneWidget);

    // Screens are queried before windows so the stable entries sort first.
    expect(fake.typeRequests, [
      ['screen'],
      ['window'],
    ]);
    // Thumbnails are requested at the modest size the picker documents.
    expect(fake.thumbRequests.first, {'width': 320, 'height': 180});
  });

  testWidgets('tapping a window returns its id, type and label', (t) async {
    final host = await _openPicker(t);
    await _pick(t, 'Notepad');

    expect(host.returned, isTrue);
    final choice = host.choice;
    expect(choice, isNotNull);
    expect(choice!.id, 'window:aaa');
    expect(choice.name, 'Notepad');
    expect(choice.isWindow, isTrue);
    expect(choice.label, 'Window: Notepad');
  });

  testWidgets('tapping a display reports it as a display', (t) async {
    final host = await _openPicker(t);
    await _pick(t, 'Entire screen 2');

    final choice = host.choice;
    expect(choice, isNotNull);
    expect(choice!.id, 'screen:1');
    expect(choice.isWindow, isFalse);
    expect(choice.label, 'Display: Entire screen 2');
  });

  testWidgets('cancel returns null', (t) async {
    final host = await _openPicker(t);
    await t.tap(find.text('Cancel'));
    await t.pumpAndSettle();

    expect(host.returned, isTrue, reason: 'the dialog must actually pop');
    expect(host.choice, isNull);
  });

  testWidgets('refresh re-queries, so windows opened meanwhile appear',
      (t) async {
    await _openPicker(t);
    expect(find.text('Calculator'), findsNothing);

    // A window appears while the dialog is up - the exact case the refresh
    // button exists for.
    fake.sources = [...fake.sources, _src('window:ddd', 'Calculator', 'window')];
    await t.tap(find.byTooltip('Refresh'));
    await t.pumpAndSettle();

    expect(find.text('Calculator'), findsOneWidget);
    expect(find.text('Windows (4)'), findsOneWidget);
    expect(fake.typeRequests.length, 4); // two loads x (screens + windows)
  });

  testWidgets('an enumeration failure is reported, not swallowed', (t) async {
    fake.failWith = 'access denied';
    await _openPicker(t);

    expect(find.textContaining('Could not list sources'), findsOneWidget);
    expect(find.textContaining('access denied'), findsOneWidget);
  });

  testWidgets('no sources shows an explanation rather than an empty box',
      (t) async {
    fake.sources = [];
    await _openPicker(t);

    expect(find.text('No shareable displays or windows found'), findsOneWidget);
  });

  testWidgets('a source with no thumbnail is still listed and pickable',
      (t) async {
    final host = await _openPicker(t);

    // Falling back to an icon beats hiding a perfectly shareable window.
    expect(find.text('No Thumb App'), findsOneWidget);
    expect(find.byIcon(Icons.web_asset), findsOneWidget);

    await _pick(t, 'No Thumb App');
    expect(host.choice?.id, 'window:ccc');
  });

  test('desktopPickerApplies is true only where we own the picker', () {
    for (final p in [
      TargetPlatform.windows,
      TargetPlatform.linux,
      TargetPlatform.macOS
    ]) {
      debugDefaultTargetPlatformOverride = p;
      expect(desktopPickerApplies, isTrue, reason: '$p should use our picker');
    }
    for (final p in [TargetPlatform.android, TargetPlatform.iOS]) {
      debugDefaultTargetPlatformOverride = p;
      // Mobile raises the OS consent dialog; a second picker would not match
      // what the platform finally hands back.
      expect(desktopPickerApplies, isFalse, reason: '$p has its own chooser');
    }
    debugDefaultTargetPlatformOverride = null;
  });
}
