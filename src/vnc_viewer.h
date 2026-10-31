#pragma once
#include <windows.h>
#include <string>
#include <wrl.h>
#include <WebView2.h>

using namespace Microsoft::WRL;

// VNC viewer window using WebView2 + noVNC
class VNCViewerWindow {
public:
    VNCViewerWindow();
    ~VNCViewerWindow();

    void open(const std::string& vncHost, int vncPort,
              int wsProxyPort, int httpPort,
              const std::string& password,
              const std::string& resDir);

    bool isOpen() const { return m_hwnd != nullptr; }
    HWND getHwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void createWebView(HWND hwnd, const std::string& url, const std::string& resDir);
    void enterFullscreen();
    void exitFullscreen();

    HWND m_hwnd;
    std::string m_url;
    std::string m_resDir;
    ComPtr<ICoreWebView2Controller> m_webviewController;
    ComPtr<ICoreWebView2> m_webview;

    // Fullscreen state
    bool m_isFullscreen = false;
    WINDOWPLACEMENT m_savedPlacement = { sizeof(WINDOWPLACEMENT) };
};
