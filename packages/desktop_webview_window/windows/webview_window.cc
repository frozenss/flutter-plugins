//
// Created by yangbin on 2021/10/20.
//

#include <windows.h>

#include "webview_window.h"

#include <algorithm>
#include <tchar.h>
#include <utility>

#include "strconv.h"
#include "utils.h"

#include "include/desktop_webview_window/desktop_webview_window_plugin.h"

namespace {

TCHAR kWebViewWindowClassName[] = _T("WebviewWindow");

using namespace webview_window;

// Scale helper to convert logical scaler values to physical using passed in
// scale factor
int Scale(int source, double scale_factor) {
  return static_cast<int>(source * scale_factor);
}

// System menu command ids must be below 0xF000 with the low four bits clear.
constexpr UINT kAlwaysOnTopSysCommand = 0x0010;
constexpr UINT kSystemMenuCommandItemSysCommand = 0x0020;

// Child control id for the lock toggle on the native strip.
constexpr int kLockButtonId = 1001;

// Child control id for the command button right of the lock toggle.
constexpr int kCommandButtonId = 1002;

// Logical layout of the lock strip controls.
constexpr int kLockButtonLogicalWidth = 72;
constexpr int kStripLogicalPad = 6;
constexpr int kCommandButtonLogicalMinWidth = 72;
constexpr int kCommandButtonLogicalTextPad = 12;
constexpr int kCommandButtonLogicalInset = 2;

// Logical height of the lock strip under the title bar.
constexpr int kLockToolbarLogicalHeight = 28;

UINT DpiForRect(const RECT &rect) {
  HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
  return FlutterDesktopGetDpiForMonitor(monitor);
}

}

using namespace Microsoft::WRL;

HHOOK WebviewWindow::zoom_mouse_hook_ = nullptr;
std::vector<WebviewWindow *> WebviewWindow::zoom_hook_windows_;

WebviewWindow::WebviewWindow(
    MethodChannelPtr method_channel,
    int64_t window_id,
    int title_bar_height,
    std::function<void()> on_close_callback
) : method_channel_(std::move(method_channel)),
    window_id_(window_id),
    on_close_callback_(std::move(on_close_callback)),
    hwnd_(),
    title_bar_height_(title_bar_height) {

}

WebviewWindow::~WebviewWindow() {
  UnregisterZoomMouseHook();
  flutter_action_bar_.reset();
  lock_button_.reset();
  lock_toolbar_.reset();
  web_view_.reset();
  SetWindowLongPtr(hwnd_.get(), GWLP_USERDATA, 0);
  hwnd_.reset();
}

void WebviewWindow::CreateAndShow(const std::wstring &title, int height, int width,
                                  const std::wstring &userDataFolder,
                                  int windowPosX, int windowPosY, bool useWindowPositionAndSize,
                                  bool openMaximized, bool openHidden,
                                  bool denyCameraAndMicrophone,
                                  CreateCallback callback) {

  RegisterWindowClass(kWebViewWindowClassName, WebviewWindow::WndProc);

  // the same as flutter default main.cpp
  const POINT target_point = {static_cast<LONG>(10),
                              static_cast<LONG>(10)};
  HMONITOR monitor = MonitorFromPoint(target_point, MONITOR_DEFAULTTONEAREST);

  UINT dpi = FlutterDesktopGetDpiForMonitor(monitor);
  double scale_factor = dpi / 96.0;

  // Hidden means never shown here: WS_VISIBLE paints the window the moment it
  // is created, and the ShowWindow below would show it again regardless.
  DWORD dwStyle = WS_OVERLAPPEDWINDOW;
  if (!openHidden)
    dwStyle |= WS_VISIBLE;
  if (openMaximized)
    dwStyle |= WS_MAXIMIZE;

  if (useWindowPositionAndSize) {
    hwnd_ = wil::unique_hwnd(::CreateWindow(
      kWebViewWindowClassName, title.c_str(),
      dwStyle,
      windowPosX, windowPosY,
      width, height,
      nullptr, nullptr, GetModuleHandle(nullptr), this));
  } else {
    hwnd_ = wil::unique_hwnd(::CreateWindow(
      kWebViewWindowClassName, title.c_str(),
      dwStyle,
      CW_USEDEFAULT, CW_USEDEFAULT,
      Scale(width, scale_factor), Scale(height, scale_factor),
      nullptr, nullptr, GetModuleHandle(nullptr), this));
  }
  if (!hwnd_) {
    callback(false);
    return;
  }

  // Centered window on screen.
  RECT rc;
  GetClientRect(hwnd_.get(), &rc);
  if (!useWindowPositionAndSize && !openMaximized) {
    ClipOrCenterRectToMonitor(&rc, MONITOR_CENTER);
    SetWindowPos(hwnd_.get(), nullptr, rc.left, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  if (options_.has_placement && !openMaximized) {
    ApplySavedPlacement();
    GetClientRect(hwnd_.get(), &rc);
  }
  if (!options_.always_on_top_menu_label.empty()) {
    InstallAlwaysOnTopMenuItem();
  }
  if (options_.always_on_top) {
    SetAlwaysOnTop(true);
  }

  // Create the browser view.
  webview_window::WebView::ZoomOptions zoom_options;
  zoom_options.enable_zoom_control = options_.enable_zoom_control;
  zoom_options.has_initial_zoom_factor = options_.has_initial_zoom_factor;
  zoom_options.initial_zoom_factor = options_.initial_zoom_factor;
  web_view_ = std::make_unique<webview_window::WebView>(
      method_channel_, window_id_, userDataFolder, denyCameraAndMicrophone,
      zoom_options,
      [callback](HRESULT hr) {
        if (SUCCEEDED(hr)) {
          callback(true);
        } else {
          callback(false);
        }
      });

  if (openHidden) {
    web_view_->SetVisible(false);
  }

  auto web_view_handle = web_view_->NativeWindow().get();
  SetParent(web_view_handle, hwnd_.get());
  ShowWindow(web_view_handle, SW_SHOW);

  // Create the title bar view.
  if (title_bar_height_ > 0) {
    std::vector<std::string> args = {"web_view_title_bar", std::to_string(window_id_)};
    flutter_action_bar_ = std::make_unique<webview_window::FlutterView>(std::move(args));
    auto title_bar_handle = flutter_action_bar_->GetWindow();
    SetParent(title_bar_handle, hwnd_.get());
    ShowWindow(title_bar_handle, SW_SHOW);
  }

  if (options_.show_lock_toolbar) {
    lock_toolbar_locked_ = options_.lock_toolbar_initially_locked;
    CreateLockToolbar(rc.right - rc.left, scale_factor);
  }

  LayoutChildren(rc, scale_factor);

  if (options_.enable_zoom_control) {
    RegisterZoomMouseHook();
  }

  assert(hwnd_ != nullptr);

  if (!openHidden) {
    ShowWindow(hwnd_.get(), SW_SHOW);
    UpdateWindow(hwnd_.get());
  }

}

void WebviewWindow::RegisterZoomMouseHook() {
  auto &windows = zoom_hook_windows_;
  if (std::find(windows.begin(), windows.end(), this) != windows.end()) {
    return;
  }
  windows.push_back(this);
  if (!zoom_mouse_hook_) {
    zoom_mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, ZoomMouseHookProc,
                                         GetModuleHandle(nullptr), 0);
  }
}

void WebviewWindow::UnregisterZoomMouseHook() {
  auto &windows = zoom_hook_windows_;
  windows.erase(std::remove(windows.begin(), windows.end(), this),
                windows.end());
  if (windows.empty() && zoom_mouse_hook_) {
    UnhookWindowsHookEx(zoom_mouse_hook_);
    zoom_mouse_hook_ = nullptr;
  }
}

LRESULT CALLBACK WebviewWindow::ZoomMouseHookProc(int code, WPARAM wparam,
                                                  LPARAM lparam) {
  if (code == HC_ACTION && wparam == WM_MOUSEWHEEL &&
      (GetKeyState(VK_CONTROL) & 0x8000)) {
    const auto *info = reinterpret_cast<MSLLHOOKSTRUCT *>(lparam);
    HWND under = WindowFromPoint(info->pt);
    for (auto *window : zoom_hook_windows_) {
      if (!window || !window->hwnd_ || !window->web_view_) continue;
      HWND root = window->hwnd_.get();
      if (under != root && !IsChild(root, under)) continue;
      // Focused case: WebView2 IsZoomControlEnabled handles Ctrl+wheel.
      if (GetForegroundWindow() == root) break;
      const short delta = GET_WHEEL_DELTA_WPARAM(info->mouseData);
      window->web_view_->ZoomByWheelDelta(delta);
      // Swallow so the focused main window does not also scroll/zoom.
      return 1;
    }
  }
  return CallNextHookEx(zoom_mouse_hook_, code, wparam, lparam);
}

void WebviewWindow::SetBrightness(int brightness) {
}

void WebviewWindow::setVisibility(bool visible)
{
  if(visible)
    ::ShowWindow(hwnd_.get(), SW_SHOW);
  else
    ::ShowWindow(hwnd_.get(), SW_HIDE);
  if (web_view_) {
    web_view_->SetVisible(visible);
  }
}

void WebviewWindow::setVisibility(bool visible, bool activate) {
  if (!visible || activate) {
    setVisibility(visible);
    return;
  }
  ::ShowWindow(hwnd_.get(), SW_SHOWNOACTIVATE);
  if (web_view_) {
    web_view_->SetVisible(true);
  }
}

void WebviewWindow::SetAlwaysOnTop(bool on_top) {
  always_on_top_ = on_top;
  ::SetWindowPos(hwnd_.get(), on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0,
                 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  if (!options_.always_on_top_menu_label.empty()) {
    HMENU menu = GetSystemMenu(hwnd_.get(), FALSE);
    if (menu) {
      CheckMenuItem(menu, kAlwaysOnTopSysCommand,
                    MF_BYCOMMAND | (on_top ? MF_CHECKED : MF_UNCHECKED));
    }
  }
}

void WebviewWindow::SetMemoryUsageTargetLow(bool low) {
  if (web_view_) {
    web_view_->SetMemoryUsageTargetLow(low);
  }
}

void WebviewWindow::SetLockToolbarLocked(bool locked) {
  lock_toolbar_locked_ = locked;
  if (lock_button_) {
    SendMessage(lock_button_.get(), BM_SETCHECK,
                locked ? BST_CHECKED : BST_UNCHECKED, 0);
  }
}

void WebviewWindow::InstallAlwaysOnTopMenuItem() {
  HMENU menu = GetSystemMenu(hwnd_.get(), FALSE);
  if (!menu) return;
  AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
  system_menu_separator_added_ = true;
  AppendMenu(menu, MF_STRING | (always_on_top_ ? MF_CHECKED : MF_UNCHECKED),
             kAlwaysOnTopSysCommand, options_.always_on_top_menu_label.c_str());
}

void WebviewWindow::SetSystemMenuCommandItem(const std::wstring &label) {
  HMENU menu = GetSystemMenu(hwnd_.get(), FALSE);
  if (!menu) return;
  const bool shown = !system_menu_command_label_.empty();
  if (label.empty()) {
    if (shown) {
      DeleteMenu(menu, kSystemMenuCommandItemSysCommand, MF_BYCOMMAND);
    }
  } else if (shown) {
    ModifyMenu(menu, kSystemMenuCommandItemSysCommand, MF_BYCOMMAND | MF_STRING,
               kSystemMenuCommandItemSysCommand, label.c_str());
  } else {
    if (!system_menu_separator_added_) {
      AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
      system_menu_separator_added_ = true;
    }
    AppendMenu(menu, MF_STRING, kSystemMenuCommandItemSysCommand,
               label.c_str());
  }
  system_menu_command_label_ = label;
  UpdateCommandButton();
}

void WebviewWindow::UpdateCommandButton() {
  if (!lock_toolbar_) return;
  if (system_menu_command_label_.empty()) {
    if (command_button_) ShowWindow(command_button_.get(), SW_HIDE);
    return;
  }
  if (!command_button_) {
    command_button_ = wil::unique_hwnd(::CreateWindowEx(
        0, L"BUTTON", L"", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_.get(),
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandButtonId)),
        GetModuleHandle(nullptr), nullptr));
    if (!command_button_) return;
    SendMessage(command_button_.get(), WM_SETFONT,
                reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                TRUE);
  }
  SetWindowText(command_button_.get(), system_menu_command_label_.c_str());
  LayoutCommandButton(LayoutScaleFactor());
  ShowWindow(command_button_.get(), SW_SHOWNA);
}

void WebviewWindow::LayoutCommandButton(double scale_factor) {
  if (!command_button_) return;
  const int title_bar_height = Scale(title_bar_height_, scale_factor);
  const int toolbar_height = Scale(kLockToolbarLogicalHeight, scale_factor);
  const int pad = Scale(kStripLogicalPad, scale_factor);
  const int inset = Scale(kCommandButtonLogicalInset, scale_factor);
  const int left = pad + Scale(kLockButtonLogicalWidth, scale_factor) + pad;
  MoveWindow(command_button_.get(), left, title_bar_height + inset,
             CommandButtonWidth(scale_factor), toolbar_height - 2 * inset,
             true);
}

double WebviewWindow::LayoutScaleFactor() const {
  // Same DPI source as WM_SIZE, so a relabel lays out like a resize.
  RECT rect;
  GetClientRect(hwnd_.get(), &rect);
  HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
  return FlutterDesktopGetDpiForMonitor(monitor) / 96.0;
}

int WebviewWindow::CommandButtonWidth(double scale_factor) const {
  int text_width = 0;
  HDC dc = GetDC(command_button_.get());
  if (dc) {
    HGDIOBJ old_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    SIZE size{};
    if (GetTextExtentPoint32(dc, system_menu_command_label_.c_str(),
                             static_cast<int>(system_menu_command_label_.size()),
                             &size)) {
      text_width = size.cx;
    }
    SelectObject(dc, old_font);
    ReleaseDC(command_button_.get(), dc);
  }
  return (std::max)(Scale(kCommandButtonLogicalMinWidth, scale_factor),
                    text_width +
                        2 * Scale(kCommandButtonLogicalTextPad, scale_factor));
}

int WebviewWindow::ContentTop(double scale_factor) const {
  auto top = Scale(title_bar_height_, scale_factor);
  if (options_.show_lock_toolbar) {
    top += Scale(kLockToolbarLogicalHeight, scale_factor);
  }
  return top;
}

void WebviewWindow::CreateLockToolbar(int client_width, double scale_factor) {
  const int toolbar_height = Scale(kLockToolbarLogicalHeight, scale_factor);
  const int title_bar_height = Scale(title_bar_height_, scale_factor);
  lock_toolbar_ = wil::unique_hwnd(::CreateWindowEx(
      0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
      0, title_bar_height, client_width, toolbar_height, hwnd_.get(), nullptr,
      GetModuleHandle(nullptr), nullptr));
  // Short strip with one lock toggle; label is the control itself.
  // Label is U+9501; keep this source ASCII-only for MSVC code-page builds.
  const int pad = Scale(kStripLogicalPad, scale_factor);
  lock_button_ = wil::unique_hwnd(::CreateWindowEx(
      0, L"BUTTON", L"\x9501",
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_LEFT | BS_VCENTER,
      pad, title_bar_height, Scale(kLockButtonLogicalWidth, scale_factor),
      toolbar_height, hwnd_.get(),
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLockButtonId)),
      GetModuleHandle(nullptr), nullptr));
  if (lock_button_) {
    SendMessage(lock_button_.get(), WM_SETFONT,
                reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                TRUE);
    if (lock_toolbar_locked_) {
      SendMessage(lock_button_.get(), BM_SETCHECK, BST_CHECKED, 0);
    }
  }
}

void WebviewWindow::LayoutChildren(const RECT &client_rect,
                                   double scale_factor) {
  const int title_bar_height = Scale(title_bar_height_, scale_factor);
  const int content_top = ContentTop(scale_factor);
  const int width = client_rect.right - client_rect.left;
  const int height = client_rect.bottom - client_rect.top;

  if (flutter_action_bar_) {
    if (last_title_bar_width_ != width) {
      last_title_bar_width_ = width;
      MoveWindow(flutter_action_bar_->GetWindow(), 0, 0, width,
                 title_bar_height, true);
    } else {
      last_title_bar_width_ = width + 1;
      MoveWindow(flutter_action_bar_->GetWindow(), 0, 0,
                 last_title_bar_width_, title_bar_height, true);
    }
  }

  if (lock_toolbar_) {
    const int toolbar_height = Scale(kLockToolbarLogicalHeight, scale_factor);
    MoveWindow(lock_toolbar_.get(), 0, title_bar_height, width, toolbar_height,
               true);
  }
  if (lock_button_) {
    const int toolbar_height = Scale(kLockToolbarLogicalHeight, scale_factor);
    const int pad = Scale(kStripLogicalPad, scale_factor);
    MoveWindow(lock_button_.get(), pad, title_bar_height,
               Scale(kLockButtonLogicalWidth, scale_factor), toolbar_height,
               true);
  }
  LayoutCommandButton(scale_factor);

  if (web_view_ != nullptr) {
    MoveWindow(web_view_->NativeWindow().get(), 0, content_top, width,
               height - content_top, true);
    web_view_->UpdateBounds();
  }
}

void WebviewWindow::ReportSystemMenuCommandItemSelected() {
  auto args = flutter::EncodableMap{
      {flutter::EncodableValue("id"), flutter::EncodableValue(window_id_)}};
  method_channel_->InvokeMethod(
      "onSystemMenuCommandItemSelected",
      std::make_unique<flutter::EncodableValue>(args));
}

void WebviewWindow::ReportLockToolbarChanged() {
  auto args = flutter::EncodableMap{
      {flutter::EncodableValue("id"), flutter::EncodableValue(window_id_)},
      {flutter::EncodableValue("locked"),
       flutter::EncodableValue(lock_toolbar_locked_)},
  };
  method_channel_->InvokeMethod(
      "onLockToolbarChanged",
      std::make_unique<flutter::EncodableValue>(std::move(args)));
}

void WebviewWindow::ApplySavedPlacement() {
  RECT normal = options_.placement_normal_rect;
  // The rect was saved in physical pixels at placement_dpi. If the monitor it
  // lands on now has another scale, keep the same logical size.
  UINT target_dpi = DpiForRect(normal);
  if (options_.placement_dpi > 0 && target_dpi != options_.placement_dpi) {
    double ratio = static_cast<double>(target_dpi) / options_.placement_dpi;
    normal.right = normal.left + Scale(normal.right - normal.left, ratio);
    normal.bottom = normal.top + Scale(normal.bottom - normal.top, ratio);
  }
  WINDOWPLACEMENT wp{};
  wp.length = sizeof(WINDOWPLACEMENT);
  GetWindowPlacement(hwnd_.get(), &wp);
  wp.flags = 0;
  // Keep the current visibility: the caller shows the window later.
  wp.showCmd = IsWindowVisible(hwnd_.get()) ? SW_SHOWNOACTIVATE : SW_HIDE;
  wp.rcNormalPosition = normal;
  applying_placement_ = true;
  // The system pulls a rect that is completely off-screen back into view.
  SetWindowPlacement(hwnd_.get(), &wp);
  applying_placement_ = false;
}

flutter::EncodableMap WebviewWindow::GetWindowPlacementMap() const {
  WINDOWPLACEMENT wp{};
  wp.length = sizeof(WINDOWPLACEMENT);
  GetWindowPlacement(hwnd_.get(), &wp);
  const RECT &r = wp.rcNormalPosition;
  return flutter::EncodableMap{
      {flutter::EncodableValue("left"), flutter::EncodableValue(int(r.left))},
      {flutter::EncodableValue("top"), flutter::EncodableValue(int(r.top))},
      {flutter::EncodableValue("right"), flutter::EncodableValue(int(r.right))},
      {flutter::EncodableValue("bottom"),
       flutter::EncodableValue(int(r.bottom))},
      {flutter::EncodableValue("maximized"),
       flutter::EncodableValue(wp.showCmd == SW_SHOWMAXIMIZED ||
                               IsZoomed(hwnd_.get()) != FALSE)},
      {flutter::EncodableValue("dpi"),
       flutter::EncodableValue(int(FlutterDesktopGetDpiForMonitor(
                                   MonitorFromWindow(hwnd_.get(), MONITOR_DEFAULTTONEAREST))))},
  };
}

void WebviewWindow::ReportPlacement(const char *reason) {
  auto args = GetWindowPlacementMap();
  args[flutter::EncodableValue("id")] = flutter::EncodableValue(window_id_);
  args[flutter::EncodableValue("reason")] = flutter::EncodableValue(reason);
  method_channel_->InvokeMethod(
      "onWindowPlacementChanged",
      std::make_unique<flutter::EncodableValue>(std::move(args)));
}

void WebviewWindow::moveWebviewWindow(int left, int top, int width, int height) {
  ::SetWindowPos(hwnd_.get(), nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void WebviewWindow::bringToForeground(bool maximized) {
  SetForegroundWindow(hwnd_.get());
  if (maximized) {
    if (web_view_) {
      web_view_->SetVisible(true);
    }
    ::ShowWindow(hwnd_.get(), SW_MAXIMIZE);
  }
}

void WebviewWindow::getPositionalParameters(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> completer) {
  RECT rc;
  GetWindowRect(hwnd_.get(), &rc);

  std::unique_ptr<WINDOWPLACEMENT> wp(new WINDOWPLACEMENT);
  GetWindowPlacement(hwnd_.get(), wp.get());
  std::map<flutter::EncodableValue, flutter::EncodableValue> m{
    {"left", rc.left},
    {"top", rc.top},
    {"width", rc.right-rc.left},
    {"height", rc.bottom-rc.top},
    {"maximized", wp->showCmd==SW_MAXIMIZE}};
  completer->Success(flutter::EncodableValue(m));
}

// static
LRESULT CALLBACK
WebviewWindow::WndProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) noexcept {
  if (message == WM_NCCREATE) {
    auto window_struct = reinterpret_cast<CREATESTRUCT *>(lparam);
    SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window_struct->lpCreateParams));

//    auto that = static_cast<WebviewWindow *>(window_struct->lpCreateParams);
//    that->hwnd_ = window;
  } else if (WebviewWindow *that = GetThisFromHandle(window)) {
    return that->MessageHandler(window, message, wparam, lparam);
  }

  return DefWindowProc(window, message, wparam, lparam);
}

LRESULT
WebviewWindow::MessageHandler(
    HWND hwnd,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_action_bar_) {
    std::optional<LRESULT> result = flutter_action_bar_->HandleTopLevelWindowProc(hwnd, message, wparam, lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_DESTROY: {
      flutter_action_bar_.reset();
      command_button_.reset();
      lock_button_.reset();
      lock_toolbar_.reset();
      web_view_.reset();

      // might receive multiple WM_DESTROY messages.
      if (!destroyed_) {
        destroyed_ = true;
        auto args = flutter::EncodableMap{
            {flutter::EncodableValue("id"), flutter::EncodableValue(window_id_)}
        };
        method_channel_->InvokeMethod(
            "onWindowClose",
            std::make_unique<flutter::EncodableValue>(args)
        );
        if (on_close_callback_) {
          on_close_callback_();
        }
      }
      return 0;
    }
    case WM_CLOSE: {
      if (!options_.hide_on_close) break;
      if (options_.report_placement) {
        ReportPlacement("close");
      }
      setVisibility(false);
      auto args = flutter::EncodableMap{
          {flutter::EncodableValue("id"), flutter::EncodableValue(window_id_)}};
      method_channel_->InvokeMethod(
          "onWindowCloseRequested",
          std::make_unique<flutter::EncodableValue>(args));
      return 0;
    }
    case WM_EXITSIZEMOVE: {
      if (options_.report_placement) {
        ReportPlacement("moveSizeEnd");
      }
      break;
    }
    case WM_MOUSEACTIVATE: {
      // Lock strip clicks must not activate the window, so point-word lookup
      // can keep typing in the main window.
      if (options_.show_lock_toolbar && lock_button_) {
        POINT pt;
        if (GetCursorPos(&pt)) {
          ScreenToClient(hwnd, &pt);
          HWND child = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE);
          if (child != nullptr &&
              (child == lock_button_.get() || child == lock_toolbar_.get() ||
               (command_button_ && child == command_button_.get()))) {
            return MA_NOACTIVATE;
          }
        }
      }
      break;
    }
    case WM_COMMAND: {
      if (LOWORD(wparam) == kLockButtonId &&
          HIWORD(wparam) == BN_CLICKED &&
          options_.show_lock_toolbar) {
        const bool locked =
            SendMessage(lock_button_.get(), BM_GETCHECK, 0, 0) == BST_CHECKED;
        lock_toolbar_locked_ = locked;
        ReportLockToolbarChanged();
        return 0;
      }
      if (LOWORD(wparam) == kCommandButtonId &&
          HIWORD(wparam) == BN_CLICKED &&
          !system_menu_command_label_.empty()) {
        ReportSystemMenuCommandItemSelected();
        return 0;
      }
      break;
    }
    case WM_MOUSEWHEEL: {
      // When the main window keeps focus (point-word lookup), Windows can still
      // deliver wheel messages to the hovered window. Apply Ctrl+wheel zoom
      // without activating, and only when we are not the foreground window so
      // WebView2's own zoom control is not doubled.
      if (options_.enable_zoom_control && web_view_ &&
          (GetKeyState(VK_CONTROL) & 0x8000) &&
          GetForegroundWindow() != hwnd_.get()) {
        web_view_->ZoomByWheelDelta(GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
      }
      break;
    }
    case WM_SYSCOMMAND: {
      if ((wparam & 0xFFF0) == kAlwaysOnTopSysCommand &&
          !options_.always_on_top_menu_label.empty()) {
        SetAlwaysOnTop(!always_on_top_);
        auto args = flutter::EncodableMap{
            {flutter::EncodableValue("id"), flutter::EncodableValue(window_id_)},
            {flutter::EncodableValue("alwaysOnTop"),
             flutter::EncodableValue(always_on_top_)}};
        method_channel_->InvokeMethod(
            "onAlwaysOnTopChanged",
            std::make_unique<flutter::EncodableValue>(args));
        return 0;
      }
      if ((wparam & 0xFFF0) == kSystemMenuCommandItemSysCommand &&
          !system_menu_command_label_.empty()) {
        ReportSystemMenuCommandItemSelected();
        return 0;
      }
      break;
    }
    case WM_DPICHANGED: {
      if (applying_placement_) return 0;
      auto newRectSize = reinterpret_cast<RECT *>(lparam);
      LONG newWidth = newRectSize->right - newRectSize->left;
      LONG newHeight = newRectSize->bottom - newRectSize->top;

      SetWindowPos(hwnd, nullptr, newRectSize->left, newRectSize->top, newWidth,
                   newHeight, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_SIZE: {
      RECT rect;
      GetClientRect(hwnd, &rect);
      LayoutChildren(rect, LayoutScaleFactor());
      return 0;
    }
    case WM_FONTCHANGE: {
      if (flutter_action_bar_) {
        flutter_action_bar_->ReloadSystemFonts();
      }
      break;
    }
    case WM_ACTIVATE: {
      return 0;
    }
  }

  return DefWindowProc(hwnd, message, wparam, lparam);
}

// static
WebviewWindow *WebviewWindow::GetThisFromHandle(HWND const window) noexcept {
  return reinterpret_cast<WebviewWindow *>(
      GetWindowLongPtr(window, GWLP_USERDATA));
}

