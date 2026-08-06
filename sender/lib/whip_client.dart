// WHIP (WebRTC-HTTP Ingestion Protocol, RFC 9725) client.
//
// WHIP is deliberately tiny: a single HTTP POST of the SDP offer to the
// endpoint, whose `201 Created` response body is the SDP answer and whose
// `Location` header names a resource we DELETE to tear the session down.
// That is the whole signalling surface -- no websocket, no signalling server.
// Our receivers run GStreamer `whipserversrc`, which is the matching WHIP
// *server*; this class is the sender half.
import 'package:http/http.dart' as http;

class WhipException implements Exception {
  final String message;
  WhipException(this.message);
  @override
  String toString() => 'WhipException: $message';
}

/// The receiver answered 401: it wants the PIN shown on its screen.
/// The UI prompts, then retries with [WhipSession.pin] set.
class WhipPinRequired implements Exception {
  final bool retry; // true when a wrong PIN was just rejected
  WhipPinRequired({this.retry = false});
  @override
  String toString() => retry ? 'Incorrect PIN' : 'PIN required';
}

class WhipSession {
  /// The ingestion endpoint, e.g. http://192.168.1.20:8889/whip/endpoint
  final Uri endpoint;

  /// PIN displayed on the receiver's screen (M25). Sent as a bearer token.
  final String? pin;
  Uri? _resource;

  WhipSession(this.endpoint, {this.pin});

  /// POST the local offer, return the remote answer SDP. Records the resource
  /// URL from the Location header so [stop] can DELETE it.
  Future<String> exchangeOffer(String offerSdp) async {
    final headers = <String, String>{'Content-Type': 'application/sdp'};
    if (pin != null && pin!.isNotEmpty) {
      headers['Authorization'] = 'Bearer ${pin!}';
    }
    final http.Response resp;
    try {
      resp = await http
          .post(endpoint, headers: headers, body: offerSdp)
          .timeout(const Duration(seconds: 10));
    } catch (e) {
      throw WhipException('POST $endpoint failed: $e');
    }
    if (resp.statusCode == 401) {
      // M25: the receiver is showing a PIN on its screen.
      throw WhipPinRequired(retry: pin != null && pin!.isNotEmpty);
    }
    if (resp.statusCode != 201) {
      throw WhipException(
          'WHIP POST expected 201, got ${resp.statusCode}: ${resp.body}');
    }
    final loc = resp.headers['location'];
    if (loc != null && loc.isNotEmpty) {
      // Location may be absolute or relative to the endpoint.
      _resource = loc.startsWith('http') ? Uri.parse(loc) : endpoint.resolve(loc);
    }
    if (resp.body.isEmpty) {
      throw WhipException('WHIP 201 had an empty answer body');
    }
    return resp.body;
  }

  /// Best-effort teardown of the ingestion session.
  Future<void> stop() async {
    final r = _resource;
    _resource = null;
    if (r == null) return;
    try {
      await http.delete(r).timeout(const Duration(seconds: 5));
    } catch (_) {
      // The peer connection close already stops media; a failed DELETE just
      // leaves the server to time the session out.
    }
  }
}
