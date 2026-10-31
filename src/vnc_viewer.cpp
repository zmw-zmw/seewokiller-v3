#include "vnc_viewer.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

// Dark theme colors matching main window
static const COLORREF VNC_BG = RGB(20, 25, 35);
static const wchar_t VNC_CLASS[] = L"SeewoKillerVNCViewer";

VNCViewerWindow::VNCViewerWindow() : m_hwnd(nullptr) {}
VNCViewerWindow::~VNCViewerWindow() {
    if (m_webviewController) {
        m_webviewController->Close();
        m_webviewController.Reset();
    }
    if (m_webview) {
        m_webview.Reset();
    }
    if (m_hwnd && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
}

static std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wstr(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    return wstr;
}

// ==================== Fullscreen Implementation ====================
// WebView2's Fullscreen API (used by noVNC) fires ContainsFullScreenElementChanged
// when JS calls element.requestFullscreen() / document.exitFullscreen().
// We must resize the host window accordingly.

void VNCViewerWindow::enterFullscreen() {
    if (!m_hwnd || m_isFullscreen) return;
    m_isFullscreen = true;

    // Save current window placement
    GetWindowPlacement(m_hwnd, &m_savedPlacement);

    // Get monitor info for the current monitor
    HMONITOR hMonitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfoW(hMonitor, &mi);
    RECT monRect = mi.rcMonitor;

    // Remove title bar, borders, and maximize to monitor
    DWORD style = (DWORD)GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);

    // Save original styles
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU));
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

    // Resize to cover the full monitor
    SetWindowPos(m_hwnd, nullptr,
        monRect.left, monRect.top,
        monRect.right - monRect.left,
        monRect.bottom - monRect.top,
        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);

    // Resize WebView2 to fill new client area
    if (m_webviewController) {
        RECT client;
        GetClientRect(m_hwnd, &client);
        m_webviewController->put_Bounds(client);
    }
}

void VNCViewerWindow::exitFullscreen() {
    if (!m_hwnd || !m_isFullscreen) return;
    m_isFullscreen = false;

    // Restore original window styles
    DWORD style = (DWORD)GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);

    SetWindowLongPtrW(m_hwnd, GWL_STYLE,
        style | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE,
        exStyle | WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

    // Restore window placement (position & size)
    SetWindowPlacement(m_hwnd, &m_savedPlacement);

    // Force redraw of non-client area
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Resize WebView2 to fill new client area
    if (m_webviewController) {
        RECT client;
        GetClientRect(m_hwnd, &client);
        m_webviewController->put_Bounds(client);
    }
}

// ==================== Window Procedure ====================

void VNCViewerWindow::open(const std::string& vncHost, int vncPort,
                           int wsProxyPort, int httpPort,
                           const std::string& password,
                           const std::string& resDir) {
    m_resDir = resDir;

    m_url = "http://127.0.0.1:" + std::to_string(httpPort) +
            "/vnc.html?host=127.0.0.1&port=" + std::to_string(wsProxyPort) +
            "&password=" + password +
            "&title=VNC%20-%20" + vncHost;

    // Register window class (once)
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = VNC_CLASS;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(VNC_BG);
        RegisterClassW(&wc);
        registered = true;
    }

    std::wstring title = L"VNC - " + utf8_to_wide(vncHost);
    m_hwnd = CreateWindowExW(
        0, VNC_CLASS, title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
        createWebView(m_hwnd, m_url, resDir);
    }
}

LRESULT CALLBACK VNCViewerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    VNCViewerWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<VNCViewerWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<VNCViewerWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_SIZE: {
        // Resize WebView2 controller to fill the window
        if (self && self->m_webviewController) {
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            self->m_webviewController->put_Bounds(bounds);
        }
        // Also update the full-screen WebView2 bounds if in fullscreen
        return 0;
    }
    case WM_DESTROY:
        if (self) {
            if (self->m_webviewController) {
                self->m_webviewController->Close();
                self->m_webviewController.Reset();
            }
            self->m_webview.Reset();
            self->m_hwnd = nullptr;
        }
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(VNC_BG);
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        return 1;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ==================== WebView2 Creation ====================

void VNCViewerWindow::createWebView(HWND hwnd, const std::string& url, const std::string& resDir) {
    std::wstring wUrl = utf8_to_wide(url);
    std::wstring wResDir = utf8_to_wide(resDir);
    // Use %LOCALAPPDATA% for WebView2 data (Program Files is not writable even as admin)
    wchar_t appDataPath[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appDataPath);
    std::wstring userDataFolder = std::wstring(appDataPath) + L"\\SeewoKiller\\webview_data";

    // Capture 'this' so the callback can store the controller
    VNCViewerWindow* self = this;

    auto envHandler = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [self, hwnd, wUrl, userDataFolder](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) return result;

            auto ctrlHandler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [self, hwnd, wUrl](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    if (FAILED(result) || !controller) return result;

                    // Store the controller for resize handling
                    self->m_webviewController = controller;

                    ICoreWebView2* webview = nullptr;
                    controller->get_CoreWebView2(&webview);
                    if (webview) {
                        // Store WebView2 for fullscreen handling
                        self->m_webview = webview;

                        // Set dark background
                        ICoreWebView2Controller2* controller2 = nullptr;
                        if (SUCCEEDED(controller->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&controller2)) && controller2) {
                            COREWEBVIEW2_COLOR bgColor = {255, 20, 25, 35}; // ABGR
                            controller2->put_DefaultBackgroundColor(bgColor);
                            controller2->Release();
                        }

                        // Resize to fill window
                        RECT bounds;
                        GetClientRect(hwnd, &bounds);
                        controller->put_Bounds(bounds);

                        // --- Register fullscreen event handler ---
                        // When noVNC's JS calls requestFullscreen(), WebView2 fires this event.
                        // We need to resize the host window accordingly.
                        auto fsHandler = Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
                            [self](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
                                if (!self) return S_OK;
                                BOOL containsFullScreen = FALSE;
                                sender->get_ContainsFullScreenElement(&containsFullScreen);
                                if (containsFullScreen) {
                                    self->enterFullscreen();
                                } else {
                                    self->exitFullscreen();
                                }
                                return S_OK;
                            });
                        webview->add_ContainsFullScreenElementChanged(fsHandler.Get(), nullptr);

                        // Navigate to noVNC
                        webview->Navigate(wUrl.c_str());
                        webview->Release();
                    }
                    return S_OK;
                });

            env->CreateCoreWebView2Controller(hwnd, ctrlHandler.Get());
            return S_OK;
        });

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr, envHandler.Get());
}
