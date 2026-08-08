// vkrender sender: capture this device's screen (or camera) and cast it into
// a vkrender scene over WebRTC/WHIP. One codebase for Android, iOS, desktop
// and web -- the cast lands as one more producer stream on the compositor,
// exactly like OBS or a browser WHIP sender would.
import 'package:flutter/material.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'discovery.dart';
import 'source_picker.dart';
import 'webrtc_sender.dart';
import 'whip_client.dart';

void main() => runApp(const VkrenderSenderApp());

class VkrenderSenderApp extends StatelessWidget {
  const VkrenderSenderApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'vkrender sender',
      theme: ThemeData(
        colorSchemeSeed: const Color(0xFFff5fa2), // vaporwave pink
        brightness: Brightness.dark,
        useMaterial3: true,
      ),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});
  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final ReceiverDiscovery _discovery = ReceiverDiscovery();
  final TextEditingController _manual = TextEditingController();
  CaptureSource _source = CaptureSource.screen;

  @override
  void initState() {
    super.initState();
    _discovery.start();
  }

  @override
  void dispose() {
    _discovery.dispose();
    _manual.dispose();
    super.dispose();
  }

  Future<void> _castTo(Uri whipUri) async {
    // M32: on desktop, ask WHAT to share before connecting. Doing it here
    // rather than inside CastPage means cancelling the picker simply returns
    // to this screen instead of opening a cast page that immediately fails.
    DesktopSourceChoice? chosen;
    if (_source == CaptureSource.screen && desktopPickerApplies) {
      chosen = await showDesktopSourcePicker(context);
      if (chosen == null) return; // cancelled
      if (!mounted) return;
    }
    Navigator.of(context).push(MaterialPageRoute(
      builder: (_) =>
          CastPage(endpoint: whipUri, source: _source, desktopSource: chosen),
    ));
  }

  void _castManual() {
    final text = _manual.text.trim();
    if (text.isEmpty) return;
    // Accept "host", "host:port", or a full URL.
    Uri uri;
    if (text.startsWith('http')) {
      uri = Uri.parse(text);
    } else {
      final hasPort = text.contains(':');
      final hostPort = hasPort ? text : '$text:8889';
      uri = Uri.parse('http://$hostPort$kDefaultWhipPath');
    }
    _castTo(uri);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('vkrender sender')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text('Capture source', style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          SegmentedButton<CaptureSource>(
            segments: const [
              ButtonSegment(
                  value: CaptureSource.screen,
                  icon: Icon(Icons.screen_share),
                  label: Text('Screen')),
              ButtonSegment(
                  value: CaptureSource.camera,
                  icon: Icon(Icons.videocam),
                  label: Text('Camera')),
            ],
            selected: {_source},
            onSelectionChanged: (s) => setState(() => _source = s.first),
          ),
          const SizedBox(height: 24),
          Text('Receivers on this network',
              style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          StreamBuilder<List<ReceiverInfo>>(
            stream: _discovery.receivers,
            initialData: _discovery.current,
            builder: (context, snap) {
              final list = snap.data ?? const [];
              if (list.isEmpty) {
                return const Card(
                  child: ListTile(
                    leading: Icon(Icons.search),
                    title: Text('Searching for vkrender receivers…'),
                    subtitle: Text('Or enter an address below'),
                  ),
                );
              }
              return Column(
                children: [
                  for (final r in list)
                    Card(
                      child: ListTile(
                        leading: const Icon(Icons.cast),
                        title: Text(r.name),
                        subtitle: Text('${r.host}:${r.port}${r.path}'),
                        trailing: const Icon(Icons.chevron_right),
                        onTap: () => _castTo(r.whipUri),
                      ),
                    ),
                ],
              );
            },
          ),
          const SizedBox(height: 24),
          Text('Manual address', style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _manual,
                  decoration: const InputDecoration(
                    hintText: '192.168.1.20:8889',
                    border: OutlineInputBorder(),
                  ),
                  onSubmitted: (_) => _castManual(),
                ),
              ),
              const SizedBox(width: 8),
              FilledButton.icon(
                onPressed: _castManual,
                icon: const Icon(Icons.cast_connected),
                label: const Text('Cast'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class CastPage extends StatefulWidget {
  final Uri endpoint;
  final CaptureSource source;

  /// M32: the display or window chosen in the picker. Null means "let the
  /// platform decide" - the primary screen on desktop, or the OS/browser
  /// consent dialog on mobile and web.
  final DesktopSourceChoice? desktopSource;

  const CastPage(
      {super.key,
      required this.endpoint,
      required this.source,
      this.desktopSource});
  @override
  State<CastPage> createState() => _CastPageState();
}

class _CastPageState extends State<CastPage> {
  final RTCVideoRenderer _renderer = RTCVideoRenderer();
  late final WebrtcSender _sender;
  SenderState _state = SenderState.idle;
  String? _detail;

  @override
  void initState() {
    super.initState();
    _sender = WebrtcSender(onState: (s, {detail}) {
      if (!mounted) return;
      setState(() {
        _state = s;
        if (detail != null) _detail = detail;
      });
    });
    _boot();
  }

  Future<void> _boot({String? pin}) async {
    await _renderer.initialize();
    try {
      await _sender.start(widget.endpoint, widget.source,
          pin: pin, desktopSourceId: widget.desktopSource?.id);
      if (mounted) _renderer.srcObject = _sender.stream;
    } on WhipPinRequired catch (e) {
      if (mounted) await _promptForPin(retry: e.retry);
    } catch (_) {
      // State already reflects the failure via onState.
    }
  }

  /// M25: the receiver is showing a PIN on its screen - collect it and retry.
  Future<void> _promptForPin({required bool retry}) async {
    final controller = TextEditingController();
    final entered = await showDialog<String>(
      context: context,
      barrierDismissible: false,
      builder: (ctx) => AlertDialog(
        title: const Text('Enter PIN'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(retry
                ? 'That PIN was not accepted. Check the code shown on the display.'
                : 'Enter the PIN shown on the display you are casting to.'),
            const SizedBox(height: 12),
            TextField(
              controller: controller,
              autofocus: true,
              keyboardType: TextInputType.number,
              textAlign: TextAlign.center,
              style: const TextStyle(fontSize: 32, letterSpacing: 8),
              decoration: const InputDecoration(border: OutlineInputBorder()),
              onSubmitted: (v) => Navigator.of(ctx).pop(v),
            ),
          ],
        ),
        actions: [
          TextButton(
              onPressed: () => Navigator.of(ctx).pop(), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.of(ctx).pop(controller.text.trim()),
              child: const Text('Connect')),
        ],
      ),
    );
    if (!mounted) return;
    if (entered == null || entered.isEmpty) {
      Navigator.of(context).pop(); // user cancelled: leave the cast page
      return;
    }
    await _boot(pin: entered); // retry (recurses on a wrong PIN)
  }

  @override
  void dispose() {
    _sender.stop();
    _renderer.dispose();
    super.dispose();
  }

  String get _statusText {
    switch (_state) {
      case SenderState.capturing:
        // Name what is actually being shared - "screen" is misleading once a
        // single window can be the source.
        return widget.source == CaptureSource.camera
            ? 'Capturing camera…'
            : 'Capturing ${widget.desktopSource?.label ?? 'screen'}…';
      case SenderState.signalling:
        return 'Creating offer…';
      case SenderState.connecting:
        return 'Connecting to receiver…';
      case SenderState.live:
        return 'Live';
      case SenderState.failed:
        return 'Failed: ${_detail ?? 'unknown error'}';
      case SenderState.stopped:
        return 'Stopped';
      case SenderState.pinRequired:
        return 'PIN required - check the display';
      default:
        return 'Starting…';
    }
  }

  Color _statusColor(BuildContext context) {
    switch (_state) {
      case SenderState.live:
        return Colors.greenAccent;
      case SenderState.failed:
        return Theme.of(context).colorScheme.error;
      default:
        return Theme.of(context).colorScheme.onSurfaceVariant;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text('Casting to ${widget.endpoint.host}')),
      body: Column(
        children: [
          Expanded(
            child: Container(
              color: Colors.black,
              child: RTCVideoView(_renderer, mirror: false),
            ),
          ),
          Padding(
            padding: const EdgeInsets.all(16),
            child: Row(
              children: [
                Icon(
                  _state == SenderState.live
                      ? Icons.fiber_manual_record
                      : Icons.hourglass_top,
                  color: _statusColor(context),
                  size: 16,
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(_statusText,
                      style: TextStyle(color: _statusColor(context))),
                ),
                FilledButton.tonalIcon(
                  onPressed: () => Navigator.of(context).pop(),
                  icon: const Icon(Icons.stop),
                  label: const Text('Stop'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
