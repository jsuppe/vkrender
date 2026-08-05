// Android-only bridge to the mediaProjection foreground service. On every
// other platform these are no-ops: desktop/web getDisplayMedia needs no
// service, and iOS screen capture uses a ReplayKit broadcast extension instead.
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:flutter/services.dart';

class ScreenCaptureService {
  static const MethodChannel _channel =
      MethodChannel('dev.vkrender/screen_capture');

  static bool get _isAndroid => !kIsWeb && Platform.isAndroid;

  /// Start the foreground service so MediaProjection is allowed to begin.
  /// Must run before getDisplayMedia on Android 14+.
  static Future<void> start() async {
    if (!_isAndroid) return;
    try {
      await _channel.invokeMethod('start');
    } on PlatformException {
      // Surface nothing here; getDisplayMedia will fail visibly if projection
      // is actually blocked.
    }
  }

  static Future<void> stop() async {
    if (!_isAndroid) return;
    try {
      await _channel.invokeMethod('stop');
    } on PlatformException {
      // ignore
    }
  }
}
