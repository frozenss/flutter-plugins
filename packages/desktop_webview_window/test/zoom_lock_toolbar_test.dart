import 'package:desktop_webview_window/desktop_webview_window.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const channel = MethodChannel('webview_window');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final createArguments = <Map<dynamic, dynamic>>[];
  final methodCalls = <MethodCall>[];
  var nextViewId = 0;
  final createdViewIds = <int>[];

  setUp(() {
    createArguments.clear();
    methodCalls.clear();
    messenger.setMockMethodCallHandler(channel, (call) async {
      methodCalls.add(call);
      if (call.method == 'create') {
        createArguments.add(call.arguments as Map);
        final viewId = nextViewId++;
        createdViewIds.add(viewId);
        return viewId;
      }
      if (call.method == 'getZoomFactor') {
        return 1.25;
      }
      if (call.method == 'setZoomFactor' ||
          call.method == 'setLockToolbarLocked') {
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

  test('create omits zoom and lock flags by default', () async {
    await WebviewWindow.create(
      configuration: const CreateConfiguration(titleBarHeight: 0),
    );

    final args = createArguments.single;
    expect(args.containsKey('enableZoomControl'), isFalse);
    expect(args.containsKey('initialZoomFactor'), isFalse);
    expect(args.containsKey('showLockToolbar'), isFalse);
    expect(args.containsKey('lockToolbarInitiallyLocked'), isFalse);
  });

  test('create serializes zoom and lock options when enabled', () async {
    const configuration = CreateConfiguration(
      titleBarHeight: 0,
      enableZoomControl: true,
      initialZoomFactor: 1.5,
      showLockToolbar: true,
      lockToolbarInitiallyLocked: true,
    );
    await WebviewWindow.create(configuration: configuration);

    expect(createArguments.single, configuration.toMap());
    expect(createArguments.single['enableZoomControl'], isTrue);
    expect(createArguments.single['initialZoomFactor'], 1.5);
    expect(createArguments.single['showLockToolbar'], isTrue);
    expect(createArguments.single['lockToolbarInitiallyLocked'], isTrue);
  });

  test('zoom factor get/set and change callback round-trip on the channel',
      () async {
    final webview = await WebviewWindow.create(
      configuration: const CreateConfiguration(
        titleBarHeight: 0,
        enableZoomControl: true,
      ),
    );
    final viewId = createdViewIds.single;
    double? seen;
    webview.setOnZoomFactorChangedCallback((factor) => seen = factor);

    expect(await webview.getZoomFactor(), 1.25);
    await webview.setZoomFactor(1.75);

    expect(
      methodCalls.where((c) => c.method == 'getZoomFactor').single.arguments,
      {'viewId': viewId},
    );
    expect(
      methodCalls.where((c) => c.method == 'setZoomFactor').single.arguments,
      {'viewId': viewId, 'value': 1.75},
    );

    await messenger.handlePlatformMessage(
      channel.name,
      const StandardMethodCodec().encodeMethodCall(
        MethodCall('onZoomFactorChanged', {'id': viewId, 'zoomFactor': 1.75}),
      ),
      (_) {},
    );
    expect(seen, 1.75);
  });

  test('lock toolbar set and change callback round-trip on the channel',
      () async {
    final webview = await WebviewWindow.create(
      configuration: const CreateConfiguration(
        titleBarHeight: 0,
        showLockToolbar: true,
      ),
    );
    final viewId = createdViewIds.single;
    bool? seen;
    webview.setOnLockToolbarChangedCallback((locked) => seen = locked);

    await webview.setLockToolbarLocked(true);
    expect(
      methodCalls
          .where((c) => c.method == 'setLockToolbarLocked')
          .single
          .arguments,
      {'viewId': viewId, 'value': true},
    );

    await messenger.handlePlatformMessage(
      channel.name,
      const StandardMethodCodec().encodeMethodCall(
        MethodCall('onLockToolbarChanged', {'id': viewId, 'locked': true}),
      ),
      (_) {},
    );
    expect(seen, isTrue);
  });
}
