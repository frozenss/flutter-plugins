//
// Created by yangbin on 2021/10/20.
//

#ifndef _WEBVIEW_WINDOW_WEBVIEW_WINDOW_H_
#define _WEBVIEW_WINDOW_WEBVIEW_WINDOW_H_

#include <Windows.h>

#include <flutter/dart_project.h>
#include <flutter/method_channel.h>
#include <flutter/encodable_value.h>
#include <flutter/flutter_view_controller.h>

#include <string>
#include <memory>
#include <functional>
#include <vector>

#include <wrl.h>
#include "wil/com.h"
#include "WebView2.h"

#include "flutter_view.h"
#include "web_view.h"

class WebviewWindow {

 public:

  using MethodChannelPtr = std::shared_ptr<flutter::MethodChannel<flutter::EncodableValue>>;

  WebviewWindow(MethodChannelPtr method_channel,
                int64_t window_id,
                int title_bar_height,
                std::function<void()> on_close_callback);

  virtual ~WebviewWindow();

  using CreateCallback = std::function<void(bool success)>;

  void CreateAndShow(const std::wstring &title, int height, int width,
                     const std::wstring &userDataFolder,
                     int windowPosX, int windowPosY, bool useWindowPositionAndSize,
                     bool openMaximized, bool openHidden,
                     bool denyCameraAndMicrophone, CreateCallback callback);

  // Opt-in window behaviour, set before CreateAndShow. All default off, so a
  // caller that does not ask gets the original behaviour.
  struct WindowOptions {
    // X (WM_CLOSE) hides the window instead of destroying it, and reports
    // "onWindowCloseRequested" plus the placement.
    bool hide_on_close = false;
    // Report "onWindowPlacementChanged" when a move/resize ends.
    bool report_placement = false;
    bool always_on_top = false;
    // When non-empty, a checkable item with this label is added to the
    // system menu; toggling it sets always-on-top and reports
    // "onAlwaysOnTopChanged".
    std::wstring always_on_top_menu_label;
    // Restore placement saved from a previous "getWindowPlacement" /
    // "onWindowPlacementChanged" (physical pixels at |placement_dpi|).
    bool has_placement = false;
    RECT placement_normal_rect{};
    UINT placement_dpi = 96;
    // WebView2 page zoom (Ctrl+wheel / Ctrl+＋－0) and ZoomFactor API.
    bool enable_zoom_control = false;
    bool has_initial_zoom_factor = false;
    double initial_zoom_factor = 1.0;
    // Native lock strip under the title bar; plugin does not touch scroll.
    bool show_lock_toolbar = false;
    bool lock_toolbar_initially_locked = false;
  };

  void SetWindowOptions(WindowOptions options) { options_ = std::move(options); }

  // Shows without activating when |activate| is false, so the window does not
  // take focus from the app's main window.
  void setVisibility(bool visible, bool activate);

  void SetAlwaysOnTop(bool on_top);

  void SetMemoryUsageTargetLow(bool low);

  void SetLockToolbarLocked(bool locked);

  // Shows a plain (not checkable) system menu item with |label| after the
  // items added before it, relabels it in place when already shown, and
  // removes it when |label| is empty. With the lock toolbar, a push button
  // with the same label is shown right of the lock toggle and hidden
  // together with the item. Selecting either reports
  // "onSystemMenuCommandItemSelected". Neither exists until this is called.
  void SetSystemMenuCommandItem(const std::wstring &label);

  [[nodiscard]] flutter::EncodableMap GetWindowPlacementMap() const;


  // OS callback called by message pump. Handles the WM_NCCREATE message which
  // is passed when the non-client area is being created and enables automatic
  // non-client DPI scaling so that the non-client area automatically
  // responsponds to changes in DPI. All other messages are handled by
  // MessageHandler.
  static LRESULT CALLBACK WndProc(HWND window,
                                  UINT message,
                                  WPARAM wparam,
                                  LPARAM lparam) noexcept;

  void SetBrightness(int brightness);

  void setVisibility(bool visible);

  void moveWebviewWindow(int left, int top, int width, int height);

  void bringToForeground(bool maximized);

  void getPositionalParameters(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> completer);

  [[nodiscard]] const std::unique_ptr<webview_window::WebView> &GetWebView() const {
    return web_view_;
  }

 private:

  // Retrieves a class instance pointer for |window|
  static WebviewWindow *GetThisFromHandle(HWND window) noexcept;

  MethodChannelPtr method_channel_;

  wil::unique_hwnd hwnd_;

  int64_t window_id_;

  std::function<void()> on_close_callback_;

  std::unique_ptr<webview_window::FlutterView> flutter_action_bar_;

  std::unique_ptr<webview_window::WebView> web_view_;

  wil::unique_hwnd lock_toolbar_;

  wil::unique_hwnd lock_button_;

  // Push button on the lock strip mirroring the system menu command item;
  // created on first show, hidden (not destroyed) when the item is removed.
  wil::unique_hwnd command_button_;

  int last_title_bar_width_ = 0;

  bool destroyed_ = false;

  int title_bar_height_;

  WindowOptions options_;

  bool always_on_top_ = false;

  bool lock_toolbar_locked_ = false;

  // Label of the shown system menu command item; empty = not in the menu.
  std::wstring system_menu_command_label_;

  // The separator before the plugin's own system menu items is added once.
  bool system_menu_separator_added_ = false;

  // Set while CreateAndShow applies a saved placement, so the WM_DPICHANGED
  // it may trigger does not rescale a rect that is already in target pixels.
  bool applying_placement_ = false;

  void ApplySavedPlacement();

  void InstallAlwaysOnTopMenuItem();

  void CreateLockToolbar(int client_width, double scale_factor);

  void LayoutChildren(const RECT &client_rect, double scale_factor);

  int ContentTop(double scale_factor) const;

  void ReportPlacement(const char *reason);

  void ReportLockToolbarChanged();

  void ReportSystemMenuCommandItemSelected();

  // Creates, relabels, shows or hides |command_button_| to match
  // |system_menu_command_label_|; no-op without the lock toolbar.
  void UpdateCommandButton();

  // Label width in DEFAULT_GUI_FONT plus padding, at least the minimum width.
  int CommandButtonWidth(double scale_factor) const;

  // Places |command_button_| right of the lock toggle on the strip.
  void LayoutCommandButton(double scale_factor);

  // Scale factor used for laying out child controls (as on WM_SIZE).
  double LayoutScaleFactor() const;

  void RegisterZoomMouseHook();

  void UnregisterZoomMouseHook();

  // Low-level mouse hook so Ctrl+wheel zooms while the pointer is over this
  // window even when another window (e.g. the Flutter main window) has focus.
  static LRESULT CALLBACK ZoomMouseHookProc(int code, WPARAM wparam,
                                            LPARAM lparam);

  static HHOOK zoom_mouse_hook_;
  static std::vector<WebviewWindow *> zoom_hook_windows_;

  // Processes and route salient window messages for mouse handling,
  // size change and DPI. Delegates handling of these to member overloads that
  // inheriting classes can handle.
  LRESULT MessageHandler(HWND window,
                         UINT message,
                         WPARAM wparam,
                         LPARAM lparam) noexcept;

  LRESULT HandleNCHitTest(int x, int y) noexcept;

  void SetBorderless() noexcept;

};

#endif //_WEBVIEW_WINDOW_WEBVIEW_WINDOW_H_
