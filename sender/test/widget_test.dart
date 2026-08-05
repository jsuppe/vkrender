// Smoke test: the home page builds and shows the capture-source picker.
import 'package:flutter_test/flutter_test.dart';

import 'package:vkrender_sender/main.dart';

void main() {
  testWidgets('home page renders capture controls', (WidgetTester tester) async {
    await tester.pumpWidget(const VkrenderSenderApp());

    expect(find.text('vkrender sender'), findsOneWidget);
    expect(find.text('Screen'), findsOneWidget);
    expect(find.text('Camera'), findsOneWidget);
    expect(find.text('Cast'), findsOneWidget);
  });
}
