import 'package:desktop_webview_window/desktop_webview_window.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const channel = MethodChannel('webview_window');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final methodCalls = <MethodCall>[];
  var nextViewId = 0;
  final createdViewIds = <int>[];

  setUp(() {
    methodCalls.clear();
    messenger.setMockMethodCallHandler(channel, (call) async {
      methodCalls.add(call);
      if (call.method == 'create') {
        final viewId = nextViewId++;
        createdViewIds.add(viewId);
        return viewId;
      }
      if (call.method == 'setSystemMenuCommandItem') {
        return null;
      }
      throw MissingPluginException(call.method);
    });
  });

  tearDown(() async {
    for (final viewId in createdViewIds) {
      await messenger.handlePlatformMessage(
        channel.name,
        const StandardMethodCodec().encodeMethodCall(
          MethodCall('onWindowClose', {'id': viewId}),
        ),
        (_) {},
      );
    }
    createdViewIds.clear();
    messenger.setMockMethodCallHandler(channel, null);
  });

  test('create sends no system menu command item by default', () async {
    await WebviewWindow.create(
      configuration: const CreateConfiguration(titleBarHeight: 0),
    );

    expect(
      methodCalls.where((c) => c.method == 'setSystemMenuCommandItem'),
      isEmpty,
    );
  });

  test('setSystemMenuCommandItem shows, relabels and removes the item',
      () async {
    final webview = await WebviewWindow.create(
      configuration: const CreateConfiguration(titleBarHeight: 0),
    );
    final viewId = createdViewIds.single;

    await webview.setSystemMenuCommandItem('Look up surface');
    await webview.setSystemMenuCommandItem('Look up lemma');
    await webview.setSystemMenuCommandItem(null);

    expect(
      methodCalls
          .where((c) => c.method == 'setSystemMenuCommandItem')
          .map((c) => c.arguments),
      [
        {'viewId': viewId, 'label': 'Look up surface'},
        {'viewId': viewId, 'label': 'Look up lemma'},
        {'viewId': viewId},
      ],
    );
  });

  test('selecting the item calls the callback for that window', () async {
    final first = await WebviewWindow.create(
      configuration: const CreateConfiguration(titleBarHeight: 0),
    );
    final second = await WebviewWindow.create(
      configuration: const CreateConfiguration(titleBarHeight: 0),
    );
    var firstSelected = 0;
    var secondSelected = 0;
    first.setOnSystemMenuCommandItemSelectedCallback(() => firstSelected++);
    second.setOnSystemMenuCommandItemSelectedCallback(() => secondSelected++);

    await messenger.handlePlatformMessage(
      channel.name,
      const StandardMethodCodec().encodeMethodCall(
        MethodCall('onSystemMenuCommandItemSelected', {
          'id': createdViewIds.last,
        }),
      ),
      (_) {},
    );

    expect(firstSelected, 0);
    expect(secondSelected, 1);
  });
}
