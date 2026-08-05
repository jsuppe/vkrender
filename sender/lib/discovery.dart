// mDNS / DNS-SD discovery of vkrender WHIP receivers.
//
// The receivers advertise `_vkrender-whip._tcp` on their signalling port with
// a TXT record `path=/whip/endpoint` (added on the receiver side alongside the
// AirPlay-style DnsServiceRegister / NsdManager / avahi advertisements). This
// browses for them so the app needs zero manual configuration on a LAN.
//
// Note: browsers do not expose mDNS, so on Flutter web this silently yields
// nothing and the UI falls back to manual host:port entry.
import 'dart:async';
import 'dart:convert';

import 'package:nsd/nsd.dart';

const String kWhipServiceType = '_vkrender-whip._tcp';
const String kDefaultWhipPath = '/whip/endpoint';

class ReceiverInfo {
  final String name;
  final String host;
  final int port;
  final String path;

  ReceiverInfo(
      {required this.name,
      required this.host,
      required this.port,
      required this.path});

  Uri get whipUri => Uri.parse('http://$host:$port$path');

  @override
  bool operator ==(Object other) =>
      other is ReceiverInfo && other.whipUri == whipUri;
  @override
  int get hashCode => whipUri.hashCode;
}

class ReceiverDiscovery {
  Discovery? _discovery;
  final Map<String, ReceiverInfo> _found = {};
  final StreamController<List<ReceiverInfo>> _controller =
      StreamController<List<ReceiverInfo>>.broadcast();

  Stream<List<ReceiverInfo>> get receivers => _controller.stream;
  List<ReceiverInfo> get current => _found.values.toList();

  static String? _txt(Service s, String key) {
    final v = s.txt?[key];
    if (v == null) return null;
    try {
      return utf8.decode(v);
    } catch (_) {
      return null;
    }
  }

  static String? _address(Service s) {
    if (s.host != null && s.host!.isNotEmpty) return s.host;
    final addrs = s.addresses;
    if (addrs != null && addrs.isNotEmpty) return addrs.first.address;
    return null;
  }

  Future<void> start() async {
    if (_discovery != null) return;
    final Discovery d;
    try {
      d = await startDiscovery(kWhipServiceType, ipLookupType: IpLookupType.v4);
    } catch (_) {
      // Platform without mDNS (e.g. web) -- discovery simply stays empty.
      return;
    }
    _discovery = d;
    d.addServiceListener((service, status) {
      final key = service.name ?? _address(service) ?? '';
      if (status == ServiceStatus.found) {
        final host = _address(service);
        final port = service.port;
        if (host == null || port == null) return;
        _found[key] = ReceiverInfo(
          name: service.name ?? 'vkrender',
          host: host,
          port: port,
          path: _txt(service, 'path') ?? kDefaultWhipPath,
        );
      } else if (status == ServiceStatus.lost) {
        _found.remove(key);
      }
      _controller.add(current);
    });
  }

  Future<void> stop() async {
    final d = _discovery;
    _discovery = null;
    if (d != null) {
      try {
        await stopDiscovery(d);
      } catch (_) {}
    }
  }

  void dispose() {
    stop();
    _controller.close();
  }
}
