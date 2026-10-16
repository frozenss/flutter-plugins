import 'dart:io';

class CreateConfiguration {
  final int windowWidth;
  final int windowHeight;

  /// Position of the top left point of the webview window
  final int windowPosX;
  final int windowPosY;

  /// the title of window
  final String title;

  final int titleBarHeight;

  final int titleBarTopPadding;

  final String userDataFolderWindows;

  final bool useWindowPositionAndSize;
  final bool openMaximized;

  /// Windows only. Create the window hidden, so nothing is painted until
  /// [Webview.setWebviewWindowVisibility] shows it.
  ///
  /// `create` resolves only once WebView2 has started, which can take seconds
  /// on a cold start; a window created visible sits blank and white all that
  /// time, and hiding it after `create` returns is too late to prevent that.
  final bool openHidden;

  /// Windows only. Deny every camera and microphone permission request the
  /// page (or any frame in it) makes, instead of handing it to Windows, which
  /// prompts the user on the app's behalf.
  ///
  /// The denial is not saved to the shared profile. Requires WebView2 Runtime
  /// 111.0.1661.34 or later; window creation fails on older runtimes when enabled.
  final bool denyCameraAndMicrophone;

  /// Windows only. Clicking X hides the window instead of destroying it; the
  /// webview keeps its page. See [Webview.setOnCloseRequestedCallback].
  final bool hideOnClose;

  /// Windows only. Report the window placement when a move or resize ends and
  /// (with [hideOnClose]) when X is clicked. See
  /// [Webview.setOnWindowPlacementChangedCallback].
  final bool reportWindowPlacement;

  /// Windows only. Create the window always on top.
  final bool alwaysOnTop;

  /// Windows only. When set, the window's system menu gets a checkable item
  /// with this label that toggles always-on-top. See
  /// [Webview.setOnAlwaysOnTopChangedCallback].
  final String? alwaysOnTopSystemMenuLabel;

  /// Windows only. Restore a placement previously reported by the window.
  /// Overrides [windowWidth], [windowHeight] and the position settings. A
  /// rect that is completely off-screen is pulled back into view by Windows.
  final WebviewWindowPlacement? windowPlacement;

  /// Windows only. Enable WebView2 page zoom (Ctrl+wheel, Ctrl+＋/－/0).
  /// Default off. See [Webview.getZoomFactor], [Webview.setZoomFactor],
  /// [Webview.setOnZoomFactorChangedCallback].
  final bool enableZoomControl;

  /// Windows only. Initial [Webview] zoom factor when [enableZoomControl] is
  /// true. Omit (null) to leave WebView2 at its default (1.0).
  final double? initialZoomFactor;

  /// Windows only. Show a short native strip under the system title bar with
  /// one lock toggle. Default off. The plugin does not read or write page
  /// scroll. See [Webview.setLockToolbarLocked],
  /// [Webview.setOnLockToolbarChangedCallback].
  final bool showLockToolbar;

  /// Windows only. Initial on/off of the lock toggle when [showLockToolbar]
  /// is true. Default off.
  final bool lockToolbarInitiallyLocked;

  const CreateConfiguration({
    this.windowWidth = 1280,
    this.windowHeight = 720,
    this.windowPosX = 0,
    this.windowPosY = 0,
    this.title = "",
    this.titleBarHeight = 40,
    this.titleBarTopPadding = 0,
    this.userDataFolderWindows = 'webview_window_WebView2',
    this.useWindowPositionAndSize = false,
    this.openMaximized = false,
    this.openHidden = false,
    this.denyCameraAndMicrophone = false,
    this.hideOnClose = false,
    this.reportWindowPlacement = false,
    this.alwaysOnTop = false,
    this.alwaysOnTopSystemMenuLabel,
    this.windowPlacement,
    this.enableZoomControl = false,
    this.initialZoomFactor,
    this.showLockToolbar = false,
    this.lockToolbarInitiallyLocked = false,
  });

  factory CreateConfiguration.platform() {
    return CreateConfiguration(
      titleBarTopPadding: Platform.isMacOS ? 24 : 0,
    );
  }

  Map toMap() => {
        "windowWidth": windowWidth,
        "windowHeight": windowHeight,
        "windowPosX": windowPosX,
        "windowPosY": windowPosY,
        "title": title,
        "titleBarHeight": titleBarHeight,
        "titleBarTopPadding": titleBarTopPadding,
        "userDataFolderWindows": userDataFolderWindows,
        "useWindowPositionAndSize": useWindowPositionAndSize,
        "openMaximized": openMaximized,
        "openHidden": openHidden,
        "denyCameraAndMicrophone": denyCameraAndMicrophone,
        "hideOnClose": hideOnClose,
        "reportWindowPlacement": reportWindowPlacement,
        "alwaysOnTop": alwaysOnTop,
        if (alwaysOnTopSystemMenuLabel != null)
          "alwaysOnTopSystemMenuLabel": alwaysOnTopSystemMenuLabel,
        if (windowPlacement != null)
          "windowPlacement": windowPlacement!.toMap(),
        // Opt-in only: omit when false/null so older callers and default
        // creates stay bit-identical to the previous patch line.
        if (enableZoomControl) "enableZoomControl": true,
        if (initialZoomFactor != null) "initialZoomFactor": initialZoomFactor,
        if (showLockToolbar) "showLockToolbar": true,
        if (lockToolbarInitiallyLocked) "lockToolbarInitiallyLocked": true,
      };
}

/// The restored (normal) bounds of a webview window in physical pixels,
/// workspace coordinates, as Windows' `GetWindowPlacement` reports them,
/// with the DPI they were measured at.
class WebviewWindowPlacement {
  const WebviewWindowPlacement({
    required this.left,
    required this.top,
    required this.right,
    required this.bottom,
    required this.dpi,
    this.maximized = false,
  });

  factory WebviewWindowPlacement.fromMap(Map<dynamic, dynamic> map) {
    return WebviewWindowPlacement(
      left: map['left'] as int,
      top: map['top'] as int,
      right: map['right'] as int,
      bottom: map['bottom'] as int,
      dpi: map['dpi'] as int,
      maximized: map['maximized'] as bool? ?? false,
    );
  }

  final int left;
  final int top;
  final int right;
  final int bottom;
  final int dpi;
  final bool maximized;

  Map<String, Object> toMap() => {
        'left': left,
        'top': top,
        'right': right,
        'bottom': bottom,
        'dpi': dpi,
        'maximized': maximized,
      };

  @override
  String toString() => 'WebviewWindowPlacement($left,$top,$right,$bottom '
      'dpi=$dpi maximized=$maximized)';
}
