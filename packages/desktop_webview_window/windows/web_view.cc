//
// Created by yangbin on 2021/11/12.
//

#include "web_view.h"

#include <tchar.h>
#include <windows.h>

#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "flutter/method_result_functions.h"
#include "strconv.h"
#include "utils.h"

namespace webview_window {

static LRESULT CALLBACK WndProc(HWND const window, UINT const message,
                                WPARAM const wparam,
                                LPARAM const lparam) noexcept {
  return DefWindowProc(window, message, wparam, lparam);
}

const auto kWebViewClassName = _T("web_view_window_web_view");

using namespace Microsoft::WRL;

WebView::WebView(
    std::shared_ptr<flutter::MethodChannel<flutter::EncodableValue>>
        method_channel,
    int64_t web_view_id, std::wstring userDataFolder,
    bool deny_camera_and_microphone, ZoomOptions zoom_options,
    std::function<void(HRESULT)> on_web_view_created)
    : method_channel_(std::move(method_channel)),
      web_view_id_(web_view_id),
      user_data_folder_(std::move(userDataFolder)),
      deny_camera_and_microphone_(deny_camera_and_microphone),
      zoom_options_(zoom_options),
      on_web_view_created_callback_(std::move(on_web_view_created)) {
  RegisterWindowClass(kWebViewClassName, WndProc);
  view_window_ = wil::unique_hwnd(::CreateWindowEx(
      0, kWebViewClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
      HWND_MESSAGE, nullptr, ::GetModuleHandle(nullptr), nullptr));
  assert(view_window_ != nullptr);
  if (!view_window_) {
    on_web_view_created_callback_(S_FALSE);
    return;
  }

  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data_folder_.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
            if (!SUCCEEDED(result)) {
              on_web_view_created_callback_(result);
              return S_OK;
            }
            if (deny_camera_and_microphone_) {
              // SavesInProfile is available in Runtime 111 and later. Do not
              // silently turn a window-scoped policy into a persistent denial
              // on older runtimes.
              wil::unique_cotaskmem_string version;
              int comparison = 0;
              HRESULT hr = env->get_BrowserVersionString(&version);
              if (SUCCEEDED(hr)) {
                hr = CompareBrowserVersions(version.get(), L"111.0.1661.34",
                                            &comparison);
              }
              if (FAILED(hr) || comparison < 0) {
                auto callback = std::move(on_web_view_created_callback_);
                callback(FAILED(hr) ? hr
                                    : HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION));
                return S_OK;
              }
            }
            environment_ = env;
            env->CreateCoreWebView2Controller(
                view_window_.get(),
                Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result,
                           ICoreWebView2Controller *controller) -> HRESULT {
                      if (SUCCEEDED(result)) {
                        webview_controller_ = controller;
                        result = OnWebviewControllerCreated();
                      }
                      auto callback = std::move(on_web_view_created_callback_);
                      callback(result);
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
}

HRESULT WebView::OnWebviewControllerCreated() {
  if (!webview_controller_) {
    return E_POINTER;
  }
  
  HRESULT hr = webview_controller_->get_CoreWebView2(&webview_);
  if (FAILED(hr) || !webview_) {
    std::cerr << "failed to get core webview" << std::endl;
    return FAILED(hr) ? hr : E_FAIL;
  }

  wil::com_ptr<ICoreWebView2Settings> settings;
  hr = webview_->get_Settings(&settings);
  if (FAILED(hr) || !settings) {
    std::cerr << "failed to get settings" << std::endl;
    return FAILED(hr) ? hr : E_FAIL;
  }
  
  settings->put_IsScriptEnabled(true);
  settings->put_IsZoomControlEnabled(zoom_options_.enable_zoom_control);
  settings->put_AreDefaultContextMenusEnabled(false);
  settings->put_IsStatusBarEnabled(false);
  settings->put_IsWebMessageEnabled(true);

  wil::com_ptr<ICoreWebView2Settings2> settings2;
  hr = settings->QueryInterface(IID_PPV_ARGS(&settings2));
  if (SUCCEEDED(hr) && settings2) {
    wil::unique_cotaskmem_string user_agent;
    hr = settings2->get_UserAgent(&user_agent);
    if (SUCCEEDED(hr) && user_agent) {
      default_user_agent_ = std::wstring(user_agent.get());
    }
  }

  if (zoom_options_.enable_zoom_control) {
    if (zoom_options_.has_initial_zoom_factor &&
        zoom_options_.initial_zoom_factor > 0) {
      webview_controller_->put_ZoomFactor(zoom_options_.initial_zoom_factor);
    }
    webview_controller_->add_ZoomFactorChanged(
        Callback<ICoreWebView2ZoomFactorChangedEventHandler>(
            [this](ICoreWebView2Controller *sender, IUnknown *args) {
              ReportZoomFactorChanged();
              return S_OK;
            })
            .Get(),
        nullptr);
  }

  webview_controller_->put_IsVisible(visible_);
  UpdateBounds();

  // Always use single window to load web page.
  webview_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [](ICoreWebView2 *sender,
             ICoreWebView2NewWindowRequestedEventArgs *args) {
            wil::unique_cotaskmem_string url;
            HRESULT hr = args->get_Uri(&url);
            if (SUCCEEDED(hr) && url) {
              sender->Navigate(url.get());
            }
            args->put_Handled(true);
            return S_OK;
          })
          .Get(),
      nullptr);

  if (deny_camera_and_microphone_) {
    // Answered here, so the request never reaches Windows' own privacy prompt.
    hr = webview_->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2 *sender,
               ICoreWebView2PermissionRequestedEventArgs *args) {
              COREWEBVIEW2_PERMISSION_KIND kind;
              if (SUCCEEDED(args->get_PermissionKind(&kind)) &&
                  (kind == COREWEBVIEW2_PERMISSION_KIND_CAMERA ||
                   kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE)) {
                wil::com_ptr<ICoreWebView2PermissionRequestedEventArgs3> args3;
                HRESULT hr = args->QueryInterface(IID_PPV_ARGS(&args3));
                if (FAILED(hr)) return hr;
                hr = args3->put_SavesInProfile(FALSE);
                if (FAILED(hr)) return hr;
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
              }
              return S_OK;
            })
            .Get(),
        nullptr);
    if (FAILED(hr)) return hr;
  }

  webview_->add_ContentLoading(
      Callback<ICoreWebView2ContentLoadingEventHandler>(
          [](ICoreWebView2 *sender,
             ICoreWebView2ContentLoadingEventArgs *args) { return S_OK; })
          .Get(),
      nullptr);

  webview_->add_HistoryChanged(
      Callback<ICoreWebView2HistoryChangedEventHandler>(
          [this](ICoreWebView2 *sender, IUnknown *args) {
            auto method_args = flutter::EncodableMap{
                {flutter::EncodableValue("id"),
                 flutter::EncodableValue(web_view_id_)},
                {flutter::EncodableValue("canGoBack"),
                 flutter::EncodableValue(CanGoBack())},
                {flutter::EncodableValue("canGoForward"),
                 flutter::EncodableValue(CanGoForward())},
            };
            method_channel_->InvokeMethod(
                "onHistoryChanged",
                std::make_unique<flutter::EncodableValue>(method_args));
            return S_OK;
          })
          .Get(),
      nullptr);

  webview_->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [this](ICoreWebView2 *sender,
                 ICoreWebView2NavigationStartingEventArgs *args) {
            method_channel_->InvokeMethod(
                "onNavigationStarted",
                std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
                    {flutter::EncodableValue("id"),
                     flutter::EncodableValue(web_view_id_)},
                }));

            // Every new navigation supersedes pending approval, including
            // navigations that intentionally bypass the Dart callback.
            CancelPendingNavigation();
            if (triggerOnUrlRequestedEvent) {
              wil::unique_cotaskmem_string uri;
              UINT64 navigation_id;
              HRESULT hr = args->get_Uri(&uri);
              if (FAILED(hr) || !uri ||
                  FAILED(args->get_NavigationId(&navigation_id))) {
                args->put_Cancel(true);
                return S_OK;
              }

              // Capture URI string before async callback
              std::wstring uri_string(uri.get());
              auto navigation = std::make_shared<NavigationRequest>(
                  uri_string, navigation_id);
              pending_navigation_ = navigation;

              if (!navigation->IsHttp()) {
                // Keep non-HTTP navigation canceled until Dart allows it.
                args->put_Cancel(true);
                wil::com_ptr<ICoreWebView2> webview = sender;
                // Closing or superseding the request completes it with false,
                // so a late reply cannot access this view or restart navigation.
                navigation->OnDecision(
                    [this, uri_string, webview](bool allow) {
                      if (allow) {
                        setTriggerOnUrlRequestedEvent(false);
                        webview->Navigate(uri_string.c_str());
                      }
                    });
              }

              // HTTP(S) must continue with its original method, body and headers.
              // WebResourceRequested below defers it until this reply arrives.
              auto result_handler =
                  std::make_unique<flutter::MethodResultFunctions<>>(
                      [navigation](const flutter::EncodableValue *success_value) {
                        bool letPass = false;
                        if (success_value &&
                            std::holds_alternative<bool>(*success_value)) {
                          letPass = std::get<bool>(*success_value);
                        }
                        navigation->Complete(letPass);
                      },
                      [navigation](const std::string &, const std::string &,
                                   const flutter::EncodableValue *) {
                        navigation->Complete(false);
                      },
                      [navigation]() { navigation->Complete(false); });

              method_channel_->InvokeMethod(
                  "onUrlRequested",
                  std::make_unique<flutter::EncodableValue>(
                      flutter::EncodableMap{
                          {flutter::EncodableValue("id"),
                           flutter::EncodableValue(web_view_id_)},
                          {flutter::EncodableValue("url"),
                           flutter::EncodableValue(
                               wide_to_utf8(uri_string))},
                      }),
                  std::move(result_handler));
            } else {
              args->put_Cancel(false);
              triggerOnUrlRequestedEvent = true;
            }
            return S_OK;
          })
          .Get(),
      nullptr);
  webview_->AddWebResourceRequestedFilter(
      L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
  webview_->add_WebResourceRequested(
      Callback<ICoreWebView2WebResourceRequestedEventHandler>(
          [this](ICoreWebView2 *sender,
                 ICoreWebView2WebResourceRequestedEventArgs *args) -> HRESULT {
            const auto navigation = pending_navigation_;
            if (!navigation || !navigation->IsHttp()) {
              return S_OK;
            }
            wil::com_ptr<ICoreWebView2WebResourceRequest> request;
            wil::unique_cotaskmem_string uri;
            if (FAILED(args->get_Request(&request)) || !request ||
                FAILED(request->get_Uri(&uri)) || !uri ||
                !navigation->MatchesResource(uri.get())) {
              return S_OK;
            }

            wil::com_ptr<ICoreWebView2Deferral> deferral;
            HRESULT hr = args->GetDeferral(&deferral);
            if (FAILED(hr)) {
              sender->Stop();
              return hr;
            }
            wil::com_ptr<ICoreWebView2WebResourceRequestedEventArgs> event = args;
            wil::com_ptr<ICoreWebView2> webview = sender;
            auto environment = environment_;
            navigation->OnDecision(
                [event, deferral, environment, webview](bool allow) {
                  if (!allow) {
                    // A synthetic 204 prevents the network request and keeps
                    // the current document, just like canceled navigation.
                    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
                    HRESULT hr = environment->CreateWebResourceResponse(
                        nullptr, 204, L"No Content", L"", &response);
                    if (FAILED(hr) ||
                        FAILED(event->put_Response(response.get()))) {
                      webview->Stop();
                    }
                  }
                  // Without a replacement response, WebView2 sends the
                  // original request, including POST data, exactly once.
                  deferral->Complete();
                });
            return S_OK;
          })
          .Get(),
      nullptr);
  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this](ICoreWebView2 *sender,
                 ICoreWebView2NavigationCompletedEventArgs *args) {
            UINT64 navigation_id;
            if (pending_navigation_ && pending_navigation_->IsHttp() &&
                SUCCEEDED(args->get_NavigationId(&navigation_id)) &&
                pending_navigation_->id == navigation_id) {
              pending_navigation_->Complete(false);
              pending_navigation_.reset();
            }
            auto method_args = flutter::EncodableMap{
                {flutter::EncodableValue("id"),
                 flutter::EncodableValue(web_view_id_)},
            };
            method_channel_->InvokeMethod(
                "onNavigationCompleted",
                std::make_unique<flutter::EncodableValue>(method_args));
            return S_OK;
          })
          .Get(),
      nullptr);
  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2 *sender,
                 ICoreWebView2WebMessageReceivedEventArgs *args) {
            wil::unique_cotaskmem_string messageRaw;
            HRESULT hrString = args->TryGetWebMessageAsString(&messageRaw);
            if (FAILED(hrString)) {
              if (hrString == E_INVALIDARG) {
                // web message was not a string --> should only happen if it was
                // a JSON object
                HRESULT hrJson = args->get_WebMessageAsJson(&messageRaw);
                if (FAILED(hrJson)) {
                  return hrJson;
                }
              } else {
                return hrString;
              }
            }
            method_channel_->InvokeMethod(
                "onWebMessageReceived",
                std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
                    {flutter::EncodableValue("id"),
                     flutter::EncodableValue(web_view_id_)},
                    {flutter::EncodableValue("message"),
                     flutter::EncodableValue(
                         wide_to_utf8(std::wstring(messageRaw.get())))},
                }));
            return S_OK;
          })
          .Get(),
      nullptr);
  return S_OK;
}

void WebView::UpdateBounds() {
  // Resize WebView to fit the bounds of the parent window
  RECT bounds;
  GetClientRect(view_window_.get(), &bounds);
  if (webview_controller_) 
    webview_controller_->put_Bounds(bounds);
}

void WebView::Navigate(const std::wstring &url) {
  CancelPendingNavigation();
  if (webview_) {
    webview_->Navigate(url.c_str());
  } else {
    std::cerr << "webview not created" << std::endl;
  }
}

void WebView::SetVisible(bool visible) {
  visible_ = visible;
  if (webview_controller_) {
    webview_controller_->put_IsVisible(visible);
  }
}

void WebView::SetMemoryUsageTargetLow(bool low) {
  if (!webview_) return;
  auto webview19 = webview_.try_query<ICoreWebView2_19>();
  if (!webview19) return;
  webview19->put_MemoryUsageTargetLevel(
      low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
          : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
}

double WebView::GetZoomFactor() const {
  if (!webview_controller_) return 1.0;
  double factor = 1.0;
  webview_controller_->get_ZoomFactor(&factor);
  return factor;
}

void WebView::SetZoomFactor(double zoom_factor) {
  if (!webview_controller_ || zoom_factor <= 0) return;
  webview_controller_->put_ZoomFactor(zoom_factor);
}

void WebView::ZoomByWheelDelta(short delta) {
  if (!webview_controller_ || delta == 0) return;
  double factor = 1.0;
  if (FAILED(webview_controller_->get_ZoomFactor(&factor))) return;
  // Match Chromium's common Ctrl+wheel step.
  constexpr double kStep = 1.2;
  if (delta > 0) {
    factor *= kStep;
  } else {
    factor /= kStep;
  }
  webview_controller_->put_ZoomFactor(factor);
}

void WebView::ReportZoomFactorChanged() {
  auto method_args = flutter::EncodableMap{
      {flutter::EncodableValue("id"), flutter::EncodableValue(web_view_id_)},
      {flutter::EncodableValue("zoomFactor"),
       flutter::EncodableValue(GetZoomFactor())},
  };
  method_channel_->InvokeMethod(
      "onZoomFactorChanged",
      std::make_unique<flutter::EncodableValue>(method_args));
}

void WebView::CancelPendingNavigation() {
  if (pending_navigation_) {
    pending_navigation_->Complete(false);
    pending_navigation_.reset();
  }
}

void WebView::AddScriptToExecuteOnDocumentCreated(
    const std::wstring &javaScript) {
  if (webview_) {
    webview_->AddScriptToExecuteOnDocumentCreated(javaScript.c_str(), nullptr);
  }
}

void WebView::SetApplicationNameForUserAgent(const std::wstring &name) {
  if (webview_) {
    wil::com_ptr<ICoreWebView2Settings> settings;
    HRESULT hr = webview_->get_Settings(&settings);
    if (FAILED(hr) || !settings) {
      return;
    }
    wil::com_ptr<ICoreWebView2Settings2> settings2;
    hr = settings->QueryInterface(IID_PPV_ARGS(&settings2));
    if (SUCCEEDED(hr) && settings2) {
      settings2->put_UserAgent((default_user_agent_ + name).c_str());
    }
  }
}

void WebView::GoBack() {
  CancelPendingNavigation();
  if (webview_) {
    webview_->GoBack();
  }
}

void WebView::GoForward() {
  CancelPendingNavigation();
  if (webview_) {
    webview_->GoForward();
  }
}

void WebView::Reload() {
  CancelPendingNavigation();
  if (webview_) {
    webview_->Reload();
  }
}

void WebView::Stop() {
  CancelPendingNavigation();
  if (webview_) {
    webview_->Stop();
  }
}

void WebView::GetAllCookies(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (webview_) {
    wil::com_ptr<ICoreWebView2_2> webView2;
    HRESULT hr = webview_->QueryInterface(IID_PPV_ARGS(&webView2));

    if (FAILED(hr) || !webView2) {
      result->Error("0", "Failed to get ICoreWebView2_2");
      return;
    }

    wil::com_ptr<ICoreWebView2CookieManager> cookieManager;
    HRESULT hrc = webView2->get_CookieManager(&cookieManager);

    if (FAILED(hrc) || !cookieManager) {
      result->Error("0", "Failed to get ICoreWebView2CookieManager");
      return;
    }

    cookieManager->GetCookies(
        nullptr,
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [result = std::move(result)](
                HRESULT hr, ICoreWebView2CookieList *cookieList) -> HRESULT {
              if (FAILED(hr) || !cookieList) {
                result->Error("0", "Failed to get cookies");
                return S_OK;
              }

              UINT cookieCount;
              hr = cookieList->get_Count(&cookieCount);
              if (FAILED(hr)) {
                result->Error("0", "Failed to get cookie count");
                return S_OK;
              }

              std::vector<flutter::EncodableValue> cookies;
              for (UINT i = 0; i < cookieCount; ++i) {
                wil::com_ptr<ICoreWebView2Cookie> cookie;
                hr = cookieList->GetValueAtIndex(i, &cookie);
                if (FAILED(hr) || !cookie) {
                  continue;
                }

                wil::unique_cotaskmem_string name;
                wil::unique_cotaskmem_string value;
                wil::unique_cotaskmem_string domain;
                wil::unique_cotaskmem_string path;
                double expires;
                BOOL isSecure;
                BOOL isHttpOnly;
                BOOL isSessionOnly;

                hr = cookie->get_Name(&name);
                if (FAILED(hr)) continue;
                hr = cookie->get_Value(&value);
                if (FAILED(hr)) continue;
                hr = cookie->get_Domain(&domain);
                if (FAILED(hr)) continue;
                hr = cookie->get_Path(&path);
                if (FAILED(hr)) continue;
                hr = cookie->get_Expires(&expires);
                if (FAILED(hr)) continue;
                hr = cookie->get_IsSecure(&isSecure);
                if (FAILED(hr)) continue;
                hr = cookie->get_IsHttpOnly(&isHttpOnly);
                if (FAILED(hr)) continue;
                hr = cookie->get_IsSession(&isSessionOnly);
                if (FAILED(hr)) continue;

                std::map<flutter::EncodableValue, flutter::EncodableValue>
                    cookieMap;
                cookieMap[flutter::EncodableValue("name")] =
                    flutter::EncodableValue(
                        webview_window::ConvertLPCWSTRToString(name.get()));
                cookieMap[flutter::EncodableValue("value")] =
                    flutter::EncodableValue(
                        webview_window::ConvertLPCWSTRToString(value.get()));
                cookieMap[flutter::EncodableValue("domain")] =
                    flutter::EncodableValue(
                        webview_window::ConvertLPCWSTRToString(domain.get()));
                cookieMap[flutter::EncodableValue("path")] =
                    flutter::EncodableValue(
                        webview_window::ConvertLPCWSTRToString(path.get()));

                if (expires >= 0) {
                  cookieMap[flutter::EncodableValue(std::string("expires"))] =
                      flutter::EncodableValue(static_cast<double>(expires));
                } else {
                  cookieMap[flutter::EncodableValue(std::string("expires"))] =
                      flutter::EncodableValue();
                }

                cookieMap[flutter::EncodableValue("secure")] =
                    flutter::EncodableValue(static_cast<bool>(isSecure));
                cookieMap[flutter::EncodableValue("httpOnly")] =
                    flutter::EncodableValue(static_cast<bool>(isHttpOnly));
                cookieMap[flutter::EncodableValue("sessionOnly")] =
                    flutter::EncodableValue(static_cast<bool>(isSessionOnly));

                cookies.push_back(flutter::EncodableValue(cookieMap));
              }

              result->Success(flutter::EncodableValue(cookies));
              return S_OK;
            })
            .Get());

  } else {
    result->Error("0", "webview not created");
  }
}

void WebView::openDevToolsWindow() {
  if (webview_) {
    webview_->OpenDevToolsWindow();
  }
}

bool WebView::CanGoBack() const {
  if (webview_) {
    BOOL can_go_back;
    webview_->get_CanGoBack(&can_go_back);
    return can_go_back;
  }
  return false;
}

bool WebView::CanGoForward() const {
  if (webview_) {
    BOOL can_go_forward;
    webview_->get_CanGoForward(&can_go_forward);
    return can_go_forward;
  }
  return false;
}

void WebView::ExecuteJavaScript(
    const std::wstring &javaScript,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> completer) {
  if (webview_) {
    webview_->ExecuteScript(
        javaScript.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [completer(std::move(completer))](HRESULT error,
                                              PCWSTR result) -> HRESULT {
              if (error != S_OK) {
                completer->Error("0", "Error executing JavaScript");
              } else {
                if (result) {
                  completer->Success(flutter::EncodableValue(
                      wide_to_utf8(std::wstring(result))));
                } else {
                  completer->Success(flutter::EncodableValue(""));
                }
              }
              return S_OK;
            })
            .Get());
  } else {
    completer->Error("0", "webview not created");
  }
}

void WebView::PostWebMessageAsString(
    const std::wstring &webmessage,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> completer) {
  if (webview_) {
    if (webview_->PostWebMessageAsString(webmessage.c_str()) == NOERROR) {
      completer->Success();
    } else {
      completer->Error("0", "Error posting webmessage as String");
    }
  } else {
    completer->Error("0", "webview not created");
  }
}

void WebView::PostWebMessageAsJson(
    const std::wstring &webmessage,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> completer) {
  if (webview_) {
    if (webview_->PostWebMessageAsJson(webmessage.c_str()) == NOERROR) {
      completer->Success();
    } else {
      completer->Error("0", "Error posting webmessage as JSON");
    }
  } else {
    completer->Error("0", "webview not created");
  }
}

void WebView::setTriggerOnUrlRequestedEvent(const bool value) {
  this->triggerOnUrlRequestedEvent = value;
}

WebView::~WebView() {
  if (pending_navigation_) {
    pending_navigation_->Complete(false);
    pending_navigation_.reset();
  }
  if (webview_) {
    webview_->Stop();
  }
  if (webview_controller_) {
    webview_controller_->Close();
  }
}

}  // namespace webview_window
