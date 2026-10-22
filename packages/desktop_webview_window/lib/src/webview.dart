import 'package:desktop_webview_window/src/cookie.dart';
import 'package:desktop_webview_window/src/create_configuration.dart';
import 'package:flutter/foundation.dart';

/// Handle custom message from JavaScript in your app.
typedef JavaScriptMessageHandler = void Function(String name, dynamic body);

typedef PromptHandler = String Function(String prompt, String defaultText);

typedef OnHistoryChangedCallback = void Function(
    bool canGoBack, bool canGoForward);

/// Callback when WebView start to load a URL.
/// [url] is the URL string.
typedef OnUrlRequestCallback = bool Function(String url);

/// Callback when WebView receives a web message
/// [message] constains the webmessage
typedef OnWebMessageReceivedCallback = void Function(String message);

/// Windows only. [reason] is `moveSizeEnd` or `close`.
typedef OnWindowPlacementChangedCallback = void Function(
    WebviewWindowPlacement placement, String reason);

abstract class Webview {
  Future<void> get onClose;

  ///  true if the webview is currently loading a page.
  ValueListenable<bool> get isNavigating;

  /// Install a message handler that you can call from your Javascript code.
  ///
  /// available: macOS (10.10+)
  void registerJavaScriptMessageHandler(
    String name,
    JavaScriptMessageHandler handler,
  );

  /// available: macOS
  void unregisterJavaScriptMessageHandler(String name);

  /// available: macOS
  void setPromptHandler(PromptHandler? handler);

  /// Navigates to the given URL.
  void launch(String url, {bool triggerOnUrlRequestEvent = true});

  /// change webview theme.
  ///
  /// available only: macOS (Brightness.dark only 10.14+)
  void setBrightness(Brightness? brightness);

  void addScriptToExecuteOnDocumentCreated(String javaScript);

  /// Append a string to the webview's user-agent.
  Future<void> setApplicationNameForUserAgent(String applicationName);

  /// Navigate to the previous page in the history.
  Future<void> back();

  /// Navigate to the next page in the history.
  Future<void> forward();

  /// Show or hide webview window.
  ///
  /// On Windows, [activate] false shows the window without taking focus.
  Future<void> setWebviewWindowVisibility(bool visible, {bool activate = true});

  /// Windows only.
  Future<void> setAlwaysOnTop(bool alwaysOnTop);

  /// Windows only. Ask WebView2 to lower its memory use, e.g. while hidden.
  Future<void> setMemoryUsageTargetLow(bool low);

  /// Windows only.
  Future<WebviewWindowPlacement?> getWindowPlacement();

  /// Windows only, with [CreateConfiguration.hideOnClose]: X was clicked and
  /// the window is now hidden.
  void setOnCloseRequestedCallback(VoidCallback? callback);

  /// Windows only, with [CreateConfiguration.reportWindowPlacement].
  void setOnWindowPlacementChangedCallback(
      OnWindowPlacementChangedCallback? callback);

  /// Windows only, with [CreateConfiguration.alwaysOnTopSystemMenuLabel]: the
  /// user toggled the system menu item.
  void setOnAlwaysOnTopChangedCallback(ValueChanged<bool>? callback);

  /// Windows only, with [CreateConfiguration.enableZoomControl].
  Future<double> getZoomFactor();

  /// Windows only, with [CreateConfiguration.enableZoomControl].
  Future<void> setZoomFactor(double zoomFactor);

  /// Windows only, with [CreateConfiguration.enableZoomControl].
  void setOnZoomFactorChangedCallback(ValueChanged<double>? callback);

  /// Windows only, with [CreateConfiguration.showLockToolbar]. Updates the
  /// lock toggle without reading or writing page scroll.
  Future<void> setLockToolbarLocked(bool locked);

  /// Windows only, with [CreateConfiguration.showLockToolbar]: the user
  /// toggled the lock switch.
  void setOnLockToolbarChangedCallback(ValueChanged<bool>? callback);

  /// Windows only. Shows a plain (not checkable) item with [label] in the
  /// window's system menu, after the items added before it (e.g. the
  /// [CreateConfiguration.alwaysOnTopSystemMenuLabel] item); calling it again
  /// changes the label in place, and null (or an empty label) removes the
  /// item. The menu has no such item until this is called. See
  /// [setOnSystemMenuCommandItemSelectedCallback].
  Future<void> setSystemMenuCommandItem(String? label);

  /// Windows only, with [setSystemMenuCommandItem]: the user selected the
  /// item.
  void setOnSystemMenuCommandItemSelectedCallback(VoidCallback? callback);

  /// Move and Resize the webview window
  Future<void> moveWebviewWindow(int left, int top, int width, int height);

  /// Activates the webview window (giving it the focus)
  Future<void> bringToForeground({bool maximized = false});

  /// get position, extents and maximization info of the webview window
  Future<Map<dynamic, dynamic>?> getPositionalParameters();

  /// Reload the current page.
  Future<void> reload();

  /// Stop all navigations and pending resource fetches.
  Future<void> stop();

  /// Opens the Browser DevTools in a separate window
  Future<void> openDevToolsWindow();

  /// Register a callback that will be invoked when the webview history changes.
  void setOnHistoryChangedCallback(OnHistoryChangedCallback? callback);

  void setOnUrlRequestCallback(OnUrlRequestCallback? callback);

  void addOnWebMessageReceivedCallback(OnWebMessageReceivedCallback callback);

  void removeOnWebMessageReceivedCallback(
    OnWebMessageReceivedCallback callback,
  );

  void removeAllWebMessageReceivedCallback();

  /// Close the web view window.
  void close();

  /// evaluate JavaScript in the web view.
  Future<String?> evaluateJavaScript(String javaScript);

  /// post a web message as String to the top level document in this WebView
  Future<void> postWebMessageAsString(String webMessage);

  /// post a web message as JSON to the top level document in this WebView
  Future<void> postWebMessageAsJson(String webMessage);

  Future<List<WebviewCookie>> getAllCookies();
}
