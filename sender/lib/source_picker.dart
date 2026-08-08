// M32: choose WHAT to share - a specific display, or a single window.
//
// Until now the desktop path grabbed `sources.first` from desktopCapturer,
// which meant "whichever display the OS happened to list first" and no way to
// share one window. On a multi-display machine that is a coin toss, and
// sharing a single window is the thing people actually want in a meeting.
//
// Desktop only. Mobile raises the OS consent dialog and web raises the
// browser's own picker, both of which already let the user choose - offering a
// second picker there would be redundant and would not match what the platform
// finally hands back.

import 'dart:async';

import 'package:flutter/foundation.dart' show defaultTargetPlatform, kIsWeb, TargetPlatform;
import 'package:flutter/material.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';

/// A display or window the user picked, ready to hand to getDisplayMedia.
class DesktopSourceChoice {
  const DesktopSourceChoice({
    required this.id,
    required this.name,
    required this.isWindow,
  });

  final String id;
  final String name;
  final bool isWindow;

  String get label => isWindow ? 'Window: $name' : 'Display: $name';
}

/// Thumbnails are requested at a modest size: they are only a visual aid for
/// picking, and asking for large ones makes enumeration noticeably slow when a
/// machine has many windows open.
final _thumbSize = ThumbnailSize(320, 180);

Future<List<DesktopCapturerSource>> _load() async {
  final screens =
      await desktopCapturer.getSources(types: [SourceType.Screen], thumbnailSize: _thumbSize);
  final windows =
      await desktopCapturer.getSources(types: [SourceType.Window], thumbnailSize: _thumbSize);
  // Displays first: sharing a whole screen is the common case, and it keeps
  // the list stable while windows come and go.
  return [...screens, ...windows];
}

/// Show the picker. Returns null if the user cancelled.
Future<DesktopSourceChoice?> showDesktopSourcePicker(BuildContext context) {
  return showDialog<DesktopSourceChoice>(
    context: context,
    builder: (_) => const _SourcePickerDialog(),
  );
}

/// True when this platform needs our picker (see the file header).
bool get desktopPickerApplies {
  if (kIsWeb) return false;
  return defaultTargetPlatform == TargetPlatform.windows ||
      defaultTargetPlatform == TargetPlatform.linux ||
      defaultTargetPlatform == TargetPlatform.macOS;
}

class _SourcePickerDialog extends StatefulWidget {
  const _SourcePickerDialog();

  @override
  State<_SourcePickerDialog> createState() => _SourcePickerDialogState();
}

class _SourcePickerDialogState extends State<_SourcePickerDialog> {
  List<DesktopCapturerSource>? _sources;
  String? _error;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  Future<void> _refresh() async {
    setState(() {
      _sources = null;
      _error = null;
    });
    try {
      final s = await _load();
      if (mounted) setState(() => _sources = s);
    } catch (e) {
      if (mounted) setState(() => _error = '$e');
    }
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Row(
        children: [
          const Expanded(child: Text('Choose what to share')),
          IconButton(
            tooltip: 'Refresh',
            icon: const Icon(Icons.refresh),
            // Windows open and close while the dialog is up; without this the
            // list goes stale and the user has to cancel out to see a new one.
            onPressed: _refresh,
          ),
        ],
      ),
      content: SizedBox(width: 720, height: 460, child: _body()),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('Cancel'),
        ),
      ],
    );
  }

  Widget _body() {
    if (_error != null) {
      return Center(child: Text('Could not list sources:\n$_error'));
    }
    final sources = _sources;
    if (sources == null) {
      return const Center(child: CircularProgressIndicator());
    }
    if (sources.isEmpty) {
      return const Center(child: Text('No shareable displays or windows found'));
    }
    final screens = sources.where((s) => s.type == SourceType.Screen).toList();
    final windows = sources.where((s) => s.type == SourceType.Window).toList();

    return ListView(
      children: [
        if (screens.isNotEmpty) ...[
          _heading(context, 'Displays', screens.length),
          _grid(screens),
        ],
        if (windows.isNotEmpty) ...[
          _heading(context, 'Windows', windows.length),
          _grid(windows),
        ],
      ],
    );
  }

  Widget _heading(BuildContext context, String text, int count) => Padding(
        padding: const EdgeInsets.fromLTRB(4, 12, 4, 8),
        child: Text('$text ($count)',
            style: Theme.of(context).textTheme.titleSmall),
      );

  Widget _grid(List<DesktopCapturerSource> items) => GridView.count(
        crossAxisCount: 3,
        shrinkWrap: true,
        physics: const NeverScrollableScrollPhysics(),
        childAspectRatio: 1.25,
        mainAxisSpacing: 8,
        crossAxisSpacing: 8,
        children: [for (final s in items) _tile(s)],
      );

  Widget _tile(DesktopCapturerSource s) {
    final thumb = s.thumbnail;
    return Card(
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: () => Navigator.of(context).pop(DesktopSourceChoice(
          id: s.id,
          name: s.name,
          isWindow: s.type == SourceType.Window,
        )),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Expanded(
              child: Container(
                color: Colors.black12,
                // A source without a thumbnail is still perfectly shareable -
                // fall back to an icon rather than hiding it from the list.
                child: thumb != null
                    ? Image.memory(thumb, fit: BoxFit.contain, gaplessPlayback: true)
                    : Icon(s.type == SourceType.Screen
                        ? Icons.desktop_windows
                        : Icons.web_asset),
              ),
            ),
            Padding(
              padding: const EdgeInsets.all(6),
              child: Text(
                s.name.isEmpty ? '(untitled)' : s.name,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(fontSize: 12),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
