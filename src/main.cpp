#define WIN32_LEAN_AND_MEAN
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <string>
#include <memory>
#include <filesystem>
#include <fstream>

#include <libssh/libssh.h>

#include "ssh_engine.h"
#include "vnc_proxy.h"
#include "vnc_viewer.h"
#include "commands.h"
#include "plugin_loader.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")

// ==================== Color Theme ====================
const COLORREF CLR_BG         = RGB(20, 25, 35);
const COLORREF CLR_CARD       = RGB(30, 35, 45);
const COLORREF CLR_PRIMARY    = RGB(0, 120, 215);
const COLORREF CLR_TEXT       = RGB(220, 220, 220);
const COLORREF CLR_TEXT2      = RGB(160, 160, 160);
const COLORREF CLR_INPUT_BG   = RGB(25, 30, 40);
const COLORREF CLR_BTN_BG     = RGB(40, 45, 55);
const COLORREF CLR_BTN_SILVER = RGB(226, 228, 232);       // silver-white button background
const COLORREF CLR_BTN_SILVER_DK = RGB(130, 136, 146);    // silver-white button border
const COLORREF CLR_BTN_TEXT_DK = RGB(35, 38, 48);         // silver-white button text

// ==================== Control IDs ====================
#define IDC_EDIT_IP        2001
#define IDC_EDIT_USER      2002
#define IDC_EDIT_PASSWORD  2003
#define IDC_EDIT_DELAY     2004
#define IDC_EDIT_LOOP_INT  2005
#define IDC_CHK_LOOP       2006
#define IDC_EDIT_COMMAND   2012
#define IDC_BTN_EXECUTE    2013
#define IDC_EDIT_OUTPUT    2014
#define IDC_PROGRESS       2015
#define IDC_TAB_MAIN       2021
#define IDC_EDIT_SCP_LOCAL 2016
#define IDC_BTN_SCP_BROWSE 2017
#define IDC_EDIT_SCP_REMOTE 2018
#define IDC_BTN_SCP_PUSH   2019
#define IDC_BTN_SCP_PULL   2020

// Plugin buttons start at 3000. Reserve a wide range; plugin index is
// (id - PLUGIN_BTN_BASE).
#define PLUGIN_BTN_BASE 3000

// ==================== Custom Messages ====================
#define WM_UPDATE_OUTPUT   (WM_USER + 100)
#define WM_UPDATE_PROGRESS (WM_USER + 101)

// ==================== Context Menu IDs ====================
#define IDM_CUT       3101
#define IDM_COPY      3102
#define IDM_PASTE     3103
#define IDM_SELECTALL 3104

// ==================== Output data struct ====================
struct OutputData {
    std::string text;
};

// ==================== Globals ====================
static HFONT g_fontNormal, g_fontBold, g_fontMono, g_fontTitle, g_fontSmall;
static HBRUSH g_brBg, g_brCard, g_brInput, g_brBtn;
static HWND g_hEditIP, g_hEditUser, g_hEditPassword, g_hEditDelay, g_hEditLoopInt;
static HWND g_hChkLoop, g_hEditCommand, g_hEditOutput;
static HWND g_hProgress, g_hBtnExecute;
static HWND g_hEditScpLocal, g_hBtnScpBrowse, g_hEditScpRemote, g_hBtnScpPush, g_hBtnScpPull;
static HWND g_hMainWnd = nullptr;
static HWND g_hTab = nullptr;

// Controls placed inside each tab page. Used to show/hide on tab switch.
static std::vector<HWND> g_cmdTabControls;
static std::vector<HWND> g_scpTabControls;

static std::unique_ptr<BatchSSHExecutor> g_sshExecutor;
static std::unique_ptr<VNCProxy> g_vncProxy;
static std::unique_ptr<HttpFileServer> g_httpServer;
static std::vector<std::unique_ptr<VNCViewerWindow>> g_vncWindows;
static std::string g_resDir;
static std::vector<Plugin> g_plugins;

// ==================== Layout geometry ====================
// Consistent grid + spacing used by WM_CREATE / WM_SIZE.
constexpr int LAYOUT_MARGIN = 24;       // left/right margin from window edge
constexpr int LAYOUT_INPUT_H = 30;      // single-line edit height
constexpr int LAYOUT_SECTION_GAP = 18;  // vertical gap between sections

// Vertical positions computed once in WM_CREATE, then reused by WM_SIZE to
// re-flex horizontal widths on window resize.
static int g_yIP = 0, g_yCmd = 0, g_yScpLocal = 0, g_yScpRemote = 0;
static int g_yProg = 0, g_yOutput = 0;
static int g_yTab = 0;
static int g_hIP = 90, g_hCmd = 64;

// Forward declarations for tab helpers.
static void SwitchTab(int idx);
static void ShowControls(const std::vector<HWND>& controls, int showCmd);

// ==================== Encoding utils (same as sk2.cpp) ====================
static std::string WstringToGbk(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int bufSize = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bufSize <= 0) return "";
    std::string str(bufSize, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &str[0], bufSize, nullptr, nullptr);
    str.pop_back();
    return str;
}

static std::wstring GbkToWstring(const std::string& str) {
    if (str.empty()) return L"";
    int bufSize = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, nullptr, 0);
    if (bufSize <= 0) return L"";
    std::wstring wstr(bufSize, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &wstr[0], bufSize);
    wstr.pop_back();
    return wstr;
}

static std::string getEditText(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return "";
    std::wstring buf(len + 1, 0);
    GetWindowTextW(hEdit, &buf[0], len + 1);
    buf.resize(len);
    return WstringToGbk(buf);
}

static void setEditText(HWND hEdit, const std::string& text) {
    SetWindowTextW(hEdit, GbkToWstring(text).c_str());
}

// ==================== Logging ====================
static void WriteLog(const std::string& message) {
    std::ofstream logFile("debug.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[" << __TIME__ << "] " << message << std::endl;
        logFile.close();
    }
    OutputDebugStringA(("SeewoKiller: " + message + "\n").c_str());
}

// ==================== Button color map for WM_DRAWITEM ====================
static COLORREF GetButtonColor(int id) {
    (void)id;
    return CLR_BTN_SILVER;
}

static COLORREF GetButtonBorderColor(int id) {
    (void)id;
    return CLR_BTN_SILVER_DK;
}

// ==================== Tab helpers ====================
static void ShowControls(const std::vector<HWND>& controls, int showCmd) {
    for (HWND h : controls) {
        if (h) ShowWindow(h, showCmd);
    }
}

static void SwitchTab(int idx) {
    if (idx == 0) {
        ShowControls(g_cmdTabControls, SW_SHOW);
        ShowControls(g_scpTabControls, SW_HIDE);
    } else {
        ShowControls(g_cmdTabControls, SW_HIDE);
        ShowControls(g_scpTabControls, SW_SHOW);
    }
}

static LRESULT CALLBACK TabSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR uIdSubclass,
                                          DWORD_PTR) {
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_brBg);
        return TRUE;
    }
    case WM_PAINT: {
        LRESULT r = DefSubclassProc(hWnd, msg, wParam, lParam);
        // Paint a dark frame around the content area to hide the default
        // light tab-control border (only the border, controls are children
        // and paint on top).
        HDC hdc = GetDC(hWnd);
        if (hdc) {
            RECT rcContent;
            TabCtrl_AdjustRect(hWnd, FALSE, &rcContent);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(50, 55, 65));
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rcContent.left, rcContent.top,
                      rcContent.right, rcContent.bottom);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(pen);
            ReleaseDC(hWnd, hdc);
        }
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, TabSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// ==================== Main Window Proc ====================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        WriteLog("Window created");

        // === Fonts ===
        g_fontNormal = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_fontBold = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_fontMono = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
        g_fontTitle = CreateFontW(24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_fontSmall = CreateFontW(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

        // === Brushes ===
        g_brBg = CreateSolidBrush(CLR_BG);
        g_brCard = CreateSolidBrush(CLR_CARD);
        g_brInput = CreateSolidBrush(CLR_INPUT_BG);
        g_brBtn = CreateSolidBrush(CLR_BTN_BG);

        // === Title ===
        HWND hTitle = CreateWindowW(L"STATIC", L"Seewo Killer",
            WS_VISIBLE | WS_CHILD,
            LAYOUT_MARGIN, 14, 400, 28, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessage(hTitle, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);

        const int M = LAYOUT_MARGIN;
        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        const int W = clientRc.right - clientRc.left - 2 * M;  // usable width

        int y = 52;  // below title

        g_cmdTabControls.clear();
        g_scpTabControls.clear();

        // Section header: bold label.
        auto sectionHeader = [&](const wchar_t* text) -> HWND {
            HWND h = CreateWindowW(L"STATIC", text,
                WS_VISIBLE | WS_CHILD, M, y, W, 24, hwnd, nullptr,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(h, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
            y += 24 + 8;
            return h;
        };

        // === 目标主机 ===
        sectionHeader(L"\u76ee\u6807\u4e3b\u673a");
        g_hEditIP = CreateWindowW(L"EDIT", L"172.16.20.6-12",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL,
            M, y, W, g_hIP, hwnd, (HMENU)IDC_EDIT_IP, GetModuleHandle(nullptr), nullptr);
        SendMessage(g_hEditIP, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        g_yIP = y;
        y += g_hIP + LAYOUT_SECTION_GAP;

        // === 连接配置 ===
        sectionHeader(L"\u8fde\u63a5\u914d\u7f6e");
        {
            HWND hLbl;
            auto fieldLabel = [&](int col, const wchar_t* text) {
                hLbl = CreateWindowW(L"STATIC", text, WS_VISIBLE | WS_CHILD,
                    M + col, y, 120, 20, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
                SendMessage(hLbl, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            };
            fieldLabel(0,   L"\u7528\u6237\u540d");
            fieldLabel(140, L"\u7ba1\u7406\u5458\u5bc6\u7801");
            fieldLabel(290, L"\u5ef6\u8fdf(\u79d2)");
            fieldLabel(400, L"\u5faa\u73af\u95f4\u9694(\u79d2)");
            y += 24;

            g_hEditUser = CreateWindowW(L"EDIT", L"seewo",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT,
                M + 0, y, 130, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_EDIT_USER,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(g_hEditUser, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            g_hEditPassword = CreateWindowW(L"EDIT", L"",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_PASSWORD,
                M + 140, y, 130, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_EDIT_PASSWORD,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(g_hEditPassword, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            g_hEditDelay = CreateWindowW(L"EDIT", L"0",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_NUMBER,
                M + 290, y, 80, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_EDIT_DELAY,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(g_hEditDelay, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            g_hEditLoopInt = CreateWindowW(L"EDIT", L"60",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_NUMBER,
                M + 400, y, 90, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_EDIT_LOOP_INT,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(g_hEditLoopInt, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            g_hChkLoop = CreateWindowW(L"BUTTON", L"\u5faa\u73af\u6267\u884c",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                M + 520, y + 5, 110, 22, hwnd, (HMENU)IDC_CHK_LOOP,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(g_hChkLoop, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        }
        y += LAYOUT_INPUT_H + LAYOUT_SECTION_GAP;

        // === Mode Tab Control ===
        g_yTab = y;
        const int TAB_H = 230;  // enough for the larger "Command" tab content
        g_hTab = CreateWindowW(WC_TABCONTROLW, L"",
            WS_VISIBLE | WS_CHILD | TCS_OWNERDRAWFIXED | TCS_TABS,
            M, y, W, TAB_H, hwnd, (HMENU)IDC_TAB_MAIN, GetModuleHandle(nullptr), nullptr);
        SendMessage(g_hTab, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        TabCtrl_SetItemSize(g_hTab, (W - 4) / 2, 28);
        SetWindowSubclass(g_hTab, TabSubclassProc, 0, 0);

        // Use the explicit Unicode message to avoid ANSI/Unicode macro confusion.
        TCITEMW tci = { TCIF_TEXT };
        tci.pszText = (LPWSTR)L"\u547d\u4ee4\u6267\u884c";
        SendMessageW(g_hTab, TCM_INSERTITEMW, 0, (LPARAM)&tci);
        tci.pszText = (LPWSTR)L"\u6587\u4ef6\u4f20\u8f93";
        SendMessageW(g_hTab, TCM_INSERTITEMW, 1, (LPARAM)&tci);

        // Compute tab content area in parent coordinates.
        RECT tcRc;
        GetClientRect(g_hTab, &tcRc);
        TabCtrl_AdjustRect(g_hTab, FALSE, &tcRc);
        int contentX = M + tcRc.left;
        int contentY = y + tcRc.top;
        int contentW = tcRc.right - tcRc.left;
        int cx = contentX;
        int cy = contentY;

        // === Command Tab Content ===
        {
            HWND hLbl = CreateWindowW(L"STATIC", L"\u5feb\u6377\u547d\u4ee4\uff08\u63d2\u4ef6\uff09",
                WS_VISIBLE | WS_CHILD, cx, cy, contentW, 22, hwnd, nullptr,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(hLbl, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
            g_cmdTabControls.push_back(hLbl);
            cy += 26;

            const int btnW = 100, btnH = 32, hGap = 10;
            int buttonsPerRow = (std::max)(1, (contentW + hGap) / (btnW + hGap));
            int x = cx;
            for (size_t i = 0; i < g_plugins.size(); ++i) {
                if (i > 0 && i % (size_t)buttonsPerRow == 0) {
                    x = cx;
                    cy += btnH + 8;
                }
                HWND hBtn = CreateWindowW(L"BUTTON", g_plugins[i].displayW().c_str(),
                    WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                    x, cy, btnW, btnH, hwnd, (HMENU)(PLUGIN_BTN_BASE + i),
                    GetModuleHandle(nullptr), nullptr);
                g_cmdTabControls.push_back(hBtn);
                x += btnW + hGap;
            }
            cy += btnH + 16;

            hLbl = CreateWindowW(L"STATIC", L"\u81ea\u5b9a\u4e49\u547d\u4ee4",
                WS_VISIBLE | WS_CHILD, cx, cy, contentW, 22, hwnd, nullptr,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(hLbl, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
            g_cmdTabControls.push_back(hLbl);
            cy += 26;

            g_hEditCommand = CreateWindowW(L"EDIT", L"whoami",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL,
                cx, cy, contentW - 118, g_hCmd, hwnd, (HMENU)IDC_EDIT_COMMAND,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(g_hEditCommand, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
            g_cmdTabControls.push_back(g_hEditCommand);
            g_yCmd = cy;
            g_hBtnExecute = CreateWindowW(L"BUTTON", L"\u5f00\u59cb\u6267\u884c",
                WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                cx + contentW - 110, cy, 110, g_hCmd, hwnd, (HMENU)IDC_BTN_EXECUTE,
                GetModuleHandle(nullptr), nullptr);
            g_cmdTabControls.push_back(g_hBtnExecute);
        }

        // === SCP Tab Content (initially hidden) ===
        {
            HWND hLbl;
            // 本地路径 row
            hLbl = CreateWindowW(L"STATIC", L"\u672c\u5730\u8def\u5f84",
                WS_VISIBLE | WS_CHILD, cx, contentY + 6, 120, 20,
                hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
            SendMessage(hLbl, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            g_scpTabControls.push_back(hLbl);

            int scy = contentY + 26;
            g_hEditScpLocal = CreateWindowW(L"EDIT", L"",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
                cx, scy, contentW - 98, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_EDIT_SCP_LOCAL,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(g_hEditScpLocal, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            g_scpTabControls.push_back(g_hEditScpLocal);
            g_yScpLocal = scy;
            g_hBtnScpBrowse = CreateWindowW(L"BUTTON", L"\u6d4f\u89c8...",
                WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                cx + contentW - 90, scy, 90, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_BTN_SCP_BROWSE,
                GetModuleHandle(nullptr), nullptr);
            g_scpTabControls.push_back(g_hBtnScpBrowse);

            scy += LAYOUT_INPUT_H + 12;
            hLbl = CreateWindowW(L"STATIC", L"\u8fdc\u7a0b\u8def\u5f84",
                WS_VISIBLE | WS_CHILD, cx, scy, 120, 20, hwnd, nullptr,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(hLbl, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            g_scpTabControls.push_back(hLbl);

            scy += 24;
            g_hEditScpRemote = CreateWindowW(L"EDIT", L"C:/Temp/payload.txt",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
                cx, scy, contentW - 198, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_EDIT_SCP_REMOTE,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(g_hEditScpRemote, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
            g_scpTabControls.push_back(g_hEditScpRemote);
            g_yScpRemote = scy;
            g_hBtnScpPush = CreateWindowW(L"BUTTON", L"\u4e0a\u4f20\u5230\u5168\u90e8",
                WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                cx + contentW - 196, scy, 90, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_BTN_SCP_PUSH,
                GetModuleHandle(nullptr), nullptr);
            g_scpTabControls.push_back(g_hBtnScpPush);
            g_hBtnScpPull = CreateWindowW(L"BUTTON", L"\u4ece\u9996\u53f0\u62c9\u53d6",
                WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                cx + contentW - 90, scy, 90, LAYOUT_INPUT_H, hwnd, (HMENU)IDC_BTN_SCP_PULL,
                GetModuleHandle(nullptr), nullptr);
            g_scpTabControls.push_back(g_hBtnScpPull);

            scy += LAYOUT_INPUT_H + 8;
            hLbl = CreateWindowW(L"STATIC", L"\u62c9\u53d6\uff1a\u4efb\u610f\u4e00\u53f0\u6210\u529f\u5373\u505c\u6b62",
                WS_VISIBLE | WS_CHILD, cx, scy, contentW, 18, hwnd, nullptr,
                GetModuleHandle(nullptr), nullptr);
            SendMessage(hLbl, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
            g_scpTabControls.push_back(hLbl);
        }

        // Show the active tab, hide the other.
        TabCtrl_SetCurSel(g_hTab, 0);
        SwitchTab(0);

        // === Progress Bar ===
        y += TAB_H + LAYOUT_SECTION_GAP;
        g_hProgress = CreateWindow(PROGRESS_CLASS, nullptr,
            WS_VISIBLE | WS_CHILD | PBS_SMOOTH,
            M, y, W, 22, hwnd, (HMENU)IDC_PROGRESS, GetModuleHandle(nullptr), nullptr);
        SendMessage(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessage(g_hProgress, PBM_SETBARCOLOR, 0, CLR_PRIMARY);
        SendMessage(g_hProgress, PBM_SETBKCOLOR, 0, CLR_CARD);
        g_yProg = y;
        y += 22 + 10;

        // === 执行输出 ===
        {
            HWND hLbl = CreateWindowW(L"STATIC", L"\u6267\u884c\u8f93\u51fa",
                WS_VISIBLE | WS_CHILD, M, y, 120, 20, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
            SendMessage(hLbl, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        }
        y += 24;
        g_yOutput = y;
        g_hEditOutput = CreateWindowW(L"EDIT",
            L"Seewo Killer v3 (C++)\r\n\r\n"
            L"\u4f7f\u7528\u8bf4\u660e\uff1a\r\n"
            L"* \u9876\u90e8\uff1aIP\u8303\u56f4\u3001\u7528\u6237\u540d\u3001\u5bc6\u7801\u3001\u5ef6\u8fdf/\u5faa\u73af\u914d\u7f6e\r\n"
            L"* \u547d\u4ee4\u6267\u884c Tab\uff1a\u70b9\u51fb\u63d2\u4ef6\u6309\u94ae\u586b\u5145\u547d\u4ee4\uff0c\u6216\u5728\u4e0b\u65b9\u8f93\u5165\u81ea\u5b9a\u4e49\u547d\u4ee4\r\n"
            L"* \u6587\u4ef6\u4f20\u8f93 Tab\uff1aSCP \u4e0a\u4f20\u5230\u6240\u6709\u76ee\u6807\uff0c\u6216\u4ece\u9996\u53f0\u6210\u529f\u673a\u5668\u62c9\u53d6\r\n"
            L"* \u70b9\u51fb\u5f00\u59cb\u6267\u884c / \u4e0a\u4f20\u5230\u5168\u90e8 / \u4ece\u9996\u53f0\u62c9\u53d6\u89e6\u53d1\u64cd\u4f5c\r\n",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL | ES_READONLY,
            M, y, W, 180, hwnd, (HMENU)IDC_EDIT_OUTPUT, GetModuleHandle(nullptr), nullptr);
        SendMessage(g_hEditOutput, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        // Allow very large output text (0x7FFFFFFE = ~2 billion chars, effectively unlimited)
        SendMessage(g_hEditOutput, EM_LIMITTEXT, (WPARAM)0x7FFFFFFE, 0);

        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pDraw = (LPDRAWITEMSTRUCT)lParam;

        // Owner-drawn Tab Control (dark theme)
        if (pDraw->CtlType == ODT_TAB) {
            int idx = (int)pDraw->itemID;
            bool active = (idx == (int)SendMessageW(pDraw->hwndItem, TCM_GETCURSEL, 0, 0));
            COLORREF bg = active ? CLR_BTN_SILVER : CLR_CARD;
            COLORREF border = active ? CLR_BTN_SILVER_DK : RGB(55, 60, 72);

            // Background
            HBRUSH hBrush = CreateSolidBrush(bg);
            FillRect(pDraw->hDC, &pDraw->rcItem, hBrush);
            DeleteObject(hBrush);

            // Border
            HPEN hPen = CreatePen(PS_SOLID, 1, border);
            HPEN hOldPen = (HPEN)SelectObject(pDraw->hDC, hPen);
            HBRUSH hOldBrush = (HBRUSH)SelectObject(pDraw->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(pDraw->hDC, pDraw->rcItem.left, pDraw->rcItem.top,
                      pDraw->rcItem.right, pDraw->rcItem.bottom);
            SelectObject(pDraw->hDC, hOldPen);
            SelectObject(pDraw->hDC, hOldBrush);
            DeleteObject(hPen);

            // Tab text
            WCHAR text[128] = {0};
            TCITEMW tci = { TCIF_TEXT };
            tci.pszText = text;
            tci.cchTextMax = 128;
            SendMessageW(pDraw->hwndItem, TCM_GETITEMW, idx, (LPARAM)&tci);
            SetBkMode(pDraw->hDC, TRANSPARENT);
            SetTextColor(pDraw->hDC, active ? CLR_BTN_TEXT_DK : CLR_TEXT);
            HFONT hOldFont = (HFONT)SelectObject(pDraw->hDC, g_fontBold);
            DrawTextW(pDraw->hDC, text, -1, &pDraw->rcItem,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(pDraw->hDC, hOldFont);
            return TRUE;
        }

        if (pDraw->CtlType != ODT_BUTTON) return 0;

        int id = (int)pDraw->CtlID;
        COLORREF bgColor = GetButtonColor(id);
        COLORREF borderColor = GetButtonBorderColor(id);
        COLORREF textColor = CLR_BTN_TEXT_DK;

        // Draw background
        HBRUSH hBrush = CreateSolidBrush(bgColor);
        FillRect(pDraw->hDC, &pDraw->rcItem, hBrush);
        DeleteObject(hBrush);

        // Draw border
        HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
        HPEN hOldPen = (HPEN)SelectObject(pDraw->hDC, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(pDraw->hDC, GetStockObject(NULL_BRUSH));
        RoundRect(pDraw->hDC, pDraw->rcItem.left, pDraw->rcItem.top,
            pDraw->rcItem.right, pDraw->rcItem.bottom, 4, 4);
        SelectObject(pDraw->hDC, hOldPen);
        SelectObject(pDraw->hDC, hOldBrush);
        DeleteObject(hPen);

        // Draw text
        WCHAR text[100];
        GetWindowTextW(pDraw->hwndItem, text, 100);
        SetBkMode(pDraw->hDC, TRANSPARENT);
        SetTextColor(pDraw->hDC, textColor);
        HFONT hOldFont = (HFONT)SelectObject(pDraw->hDC, g_fontBold);
        DrawTextW(pDraw->hDC, text, -1, &pDraw->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(pDraw->hDC, hOldFont);

        return TRUE;
    }

    case WM_NOTIFY: {
        LPNMHDR nmhdr = (LPNMHDR)lParam;
        if (nmhdr->idFrom == IDC_TAB_MAIN && nmhdr->code == TCN_SELCHANGE) {
            int idx = (int)TabCtrl_GetCurSel(nmhdr->hwndFrom);
            SwitchTab(idx);
            return 0;
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_BG);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)g_brBg;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        HWND hEdit = (HWND)lParam;
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_INPUT_BG);
        return (LRESULT)g_brInput;
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_BTN_BG);
        return (LRESULT)g_brBtn;
    }

    case WM_CONTEXTMENU: {
        // Right-click context menu for edit controls (copy/paste/cut/selectall)
        HWND hTarget = (HWND)wParam;
        bool isReadOnly = (hTarget == g_hEditOutput);
        if (hTarget == g_hEditCommand || hTarget == g_hEditIP ||
            hTarget == g_hEditOutput || hTarget == g_hEditUser ||
            hTarget == g_hEditPassword || hTarget == g_hEditDelay || hTarget == g_hEditLoopInt) {
            HMENU hMenu = CreatePopupMenu();
            if (!isReadOnly) {
                AppendMenuW(hMenu, MF_STRING, IDM_CUT, L"\u526a\u5207");
            }
            AppendMenuW(hMenu, MF_STRING, IDM_COPY, L"\u590d\u5236");
            if (!isReadOnly) {
                AppendMenuW(hMenu, MF_STRING, IDM_PASTE, L"\u7c98\u8d34");
            }
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_SELECTALL, L"\u5168\u9009");

            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            // If invoked by keyboard, use caret position
            if (pt.x == -1 && pt.y == -1) {
                RECT rc;
                GetWindowRect(hTarget, &rc);
                pt.x = rc.left;
                pt.y = rc.top;
            }

            // TrackPopupMenu needs a window that owns the menu messages
            MENUINFO mi = { sizeof(mi), MIM_STYLE, MNS_NOTIFYBYPOS };
            SetMenuInfo(hMenu, &mi);

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);

            switch (cmd) {
            case IDM_CUT:
                SendMessage(hTarget, WM_CUT, 0, 0);
                break;
            case IDM_COPY:
                SendMessage(hTarget, WM_COPY, 0, 0);
                break;
            case IDM_PASTE:
                SendMessage(hTarget, WM_PASTE, 0, 0);
                break;
            case IDM_SELECTALL:
                SendMessage(hTarget, EM_SETSEL, 0, -1);
                break;
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        // --- Plugin button dispatch ---
        if (id >= PLUGIN_BTN_BASE && id < PLUGIN_BTN_BASE + (int)g_plugins.size()) {
            auto& p = g_plugins[id - PLUGIN_BTN_BASE];
            if (p.isAction() && p.actionName() == "vnc") {
                std::string ipStr = getEditText(g_hEditIP);
                auto ips = ParseIPList(ipStr);
                if (ips.empty()) {
                    MessageBoxW(hwnd, L"\u672a\u89e3\u6790\u5230\u6709\u6548IP\uff01", L"\u63d0\u793a", MB_ICONWARNING);
                    return 0;
                }
                int proxyId = g_vncProxy->startProxy(ips[0], Commands::VNC_PORT);
                if (proxyId == 0) {
                    MessageBoxW(hwnd, L"\u521b\u5efaVNC\u4ee3\u7406\u5931\u8d25\uff01", L"\u9519\u8bef", MB_ICONERROR);
                    return 0;
                }
                auto info = g_vncProxy->getProxyInfo(proxyId);
                auto vncWin = std::make_unique<VNCViewerWindow>();
                vncWin->open(ips[0], Commands::VNC_PORT, info.port, g_httpServer->getPort(),
                             Commands::VNC_PASSWORD, g_resDir);
                g_vncWindows.push_back(std::move(vncWin));
            } else {
                // One-click fill: load command into the command box.
                SetWindowTextW(g_hEditCommand, Utf8ToWide(p.command).c_str());
            }
            return 0;
        }

        // --- SCP file transfer ---
        if (id == IDC_BTN_SCP_BROWSE) {
            wchar_t szFile[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                std::wstring ws(szFile);
                SetWindowTextW(g_hEditScpLocal, ws.c_str());
            }
            return 0;
        }
        if (id == IDC_BTN_SCP_PUSH || id == IDC_BTN_SCP_PULL) {
            if (g_sshExecutor && g_sshExecutor->isRunning()) {
                MessageBoxW(hwnd, L"\u6b63\u5728\u6267\u884c\uff0c\u8bf7\u5148\u505c\u6b62\uff01", L"\u63d0\u793a", MB_ICONWARNING);
                return 0;
            }
            std::string ipStr = getEditText(g_hEditIP);
            auto ips = ParseIPList(ipStr);
            if (ips.empty()) {
                MessageBoxW(hwnd, L"\u672a\u89e3\u6790\u5230\u6709\u6548IP\uff01", L"\u63d0\u793a", MB_ICONWARNING);
                return 0;
            }
            std::string username = getEditText(g_hEditUser);
            std::string password = getEditText(g_hEditPassword);
            if (username.empty()) {
                MessageBoxW(hwnd, L"\u8bf7\u8f93\u5165\u7528\u6237\u540d\uff01", L"\u63d0\u793a", MB_ICONWARNING);
                return 0;
            }

            // Local path: get as wide (may contain non-GBK chars)
            wchar_t wlocal[MAX_PATH] = {0};
            GetWindowTextW(g_hEditScpLocal, wlocal, MAX_PATH);
            std::string localPath = WstringToGbk(wlocal);
            std::string remotePath = getEditText(g_hEditScpRemote);

            if (localPath.empty()) {
                MessageBoxW(hwnd, L"\u8bf7\u8f93\u5165\u672c\u5730\u8def\u5f84\uff01", L"\u63d0\u793a", MB_ICONWARNING);
                return 0;
            }
            if (remotePath.empty()) {
                MessageBoxW(hwnd, L"\u8bf7\u8f93\u5165\u8fdc\u7a0b\u8def\u5f84\uff01", L"\u63d0\u793a", MB_ICONWARNING);
                return 0;
            }

            // For pull: if local path is a directory, append remote filename
            if (id == IDC_BTN_SCP_PULL) {
                std::error_code ec;
                if (std::filesystem::is_directory(localPath, ec)) {
                    auto pos = remotePath.find_last_of("/\\");
                    std::string fname = (pos == std::string::npos) ? remotePath : remotePath.substr(pos + 1);
                    if (fname.empty()) fname = "pulled.dat";
                    localPath = std::string(std::filesystem::path(localPath).string()) + "\\" + fname;
                }
            }

            SetWindowTextW(g_hEditOutput, L"");
            SendMessage(g_hProgress, PBM_SETPOS, 0, 0);

            if (id == IDC_BTN_SCP_PUSH) {
                WriteLog("SCP push: " + std::to_string(ips.size()) + " targets");
                g_sshExecutor->executeScpPush(ips, username, password, localPath, remotePath);
            } else {
                WriteLog("SCP pull (first success): " + std::to_string(ips.size()) + " targets");
                g_sshExecutor->executeScpPull(ips, username, password, remotePath, localPath);
            }
            return 0;
        }

        if (id == IDC_BTN_EXECUTE) {
            WriteLog("Execute button clicked");

            if (g_sshExecutor && g_sshExecutor->isRunning()) {
                g_sshExecutor->stopLoop();
                SetWindowTextW(g_hBtnExecute, L"\u5f00\u59cb\u6267\u884c");
                return 0;
            }

            std::string ipStr = getEditText(g_hEditIP);
            std::string username = getEditText(g_hEditUser);
            std::string password = getEditText(g_hEditPassword);
            std::string command = getEditText(g_hEditCommand);
            int delay = std::atoi(getEditText(g_hEditDelay).c_str());
            bool loopEnabled = (SendMessage(g_hChkLoop, BM_GETCHECK, 0, 0) == BST_CHECKED);
            int loopInterval = std::atoi(getEditText(g_hEditLoopInt).c_str());

            if (username.empty()) {
                MessageBoxW(hwnd, L"\u8bf7\u8f93\u5165\u7528\u6237\u540d\uff01", L"\u63d0\u793a", MB_ICONWARNING);
                return 0;
            }
            if (command.empty()) {
                MessageBoxW(hwnd, L"\u8bf7\u9009\u62e9\u5feb\u6377\u547d\u4ee4\u6216\u8f93\u5165\u81ea\u5b9a\u4e49\u547d\u4ee4\uff01", L"\u63d0\u793a", MB_ICONWARNING);
                return 0;
            }

            auto ips = ParseIPList(ipStr);
            if (ips.empty()) {
                MessageBoxW(hwnd, L"\u672a\u89e3\u6790\u5230\u6709\u6548IP\uff01", L"\u63d0\u793a", MB_ICONINFORMATION);
                return 0;
            }

            // Clear output and reset progress
            SetWindowTextW(g_hEditOutput, L"");
            SendMessage(g_hProgress, PBM_SETPOS, 0, 0);

            if (loopEnabled) {
                SetWindowTextW(g_hBtnExecute, L"\u505c\u6b62\u5faa\u73af");
            }

            WriteLog("Starting batch execution: " + std::to_string(ips.size()) + " IPs, user=" + username);
            g_sshExecutor->execute(ips, username, password, command, delay, loopEnabled, loopInterval);
        }
        return 0;
    }

    case WM_UPDATE_OUTPUT: {
        OutputData* pData = reinterpret_cast<OutputData*>(lParam);
        if (pData) {
            std::wstring wtext = GbkToWstring(pData->text);

            // Use EM_SETSEL + EM_REPLACESEL to append at end without full buffer copy
            LONG oldEnd = GetWindowTextLengthW(g_hEditOutput);
            SendMessageW(g_hEditOutput, EM_SETSEL, (WPARAM)oldEnd, (LPARAM)oldEnd);

            // EM_REPLACESEL needs non-readonly, temporarily clear the flag
            DWORD style = (DWORD)GetWindowLongPtrW(g_hEditOutput, GWL_STYLE);
            if (style & ES_READONLY) {
                SetWindowLongPtrW(g_hEditOutput, GWL_STYLE, style & ~ES_READONLY);
                SendMessageW(g_hEditOutput, EM_REPLACESEL, FALSE, (LPARAM)wtext.c_str());
                SetWindowLongPtrW(g_hEditOutput, GWL_STYLE, style | ES_READONLY);
            } else {
                SendMessageW(g_hEditOutput, EM_REPLACESEL, FALSE, (LPARAM)wtext.c_str());
            }

            // Scroll to bottom
            SendMessageW(g_hEditOutput, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
            SendMessageW(g_hEditOutput, EM_SCROLLCARET, 0, 0);

            delete pData;
        }
        return 0;
    }

    case WM_UPDATE_PROGRESS: {
        int progress = (int)wParam;
        SendMessage(g_hProgress, PBM_SETPOS, progress, 0);
        if (progress >= 100) {
            SetWindowTextW(g_hBtnExecute, L"\u5f00\u59cb\u6267\u884c");
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        mmi->ptMinTrackSize.x = 720;
        mmi->ptMinTrackSize.y = 620;
        return 0;
    }

    case WM_SIZE: {
        RECT cr;
        GetClientRect(hwnd, &cr);
        int clientW = cr.right - cr.left;
        int clientH = cr.bottom - cr.top;
        const int M = LAYOUT_MARGIN;
        int W = (std::max)(100, clientW - 2 * M);

        if (g_hEditIP)
            SetWindowPos(g_hEditIP, nullptr, M, g_yIP, W, g_hIP, SWP_NOZORDER);

        if (g_hTab) {
            SetWindowPos(g_hTab, nullptr, M, g_yTab, W, 230, SWP_NOZORDER);
            TabCtrl_SetItemSize(g_hTab, (W - 4) / 2, 28);

            // Recompute tab content area after horizontal resize.
            RECT tcRc;
            GetClientRect(g_hTab, &tcRc);
            TabCtrl_AdjustRect(g_hTab, FALSE, &tcRc);
            int cx = M + tcRc.left;
            int cw = tcRc.right - tcRc.left;

            if (g_hEditCommand)
                SetWindowPos(g_hEditCommand, nullptr, cx, g_yCmd, cw - 118, g_hCmd, SWP_NOZORDER);
            if (g_hBtnExecute)
                SetWindowPos(g_hBtnExecute, nullptr, cx + cw - 110, g_yCmd, 110, g_hCmd, SWP_NOZORDER);

            if (g_hEditScpLocal)
                SetWindowPos(g_hEditScpLocal, nullptr, cx, g_yScpLocal, cw - 98, LAYOUT_INPUT_H, SWP_NOZORDER);
            if (g_hBtnScpBrowse)
                SetWindowPos(g_hBtnScpBrowse, nullptr, cx + cw - 90, g_yScpLocal, 90, LAYOUT_INPUT_H, SWP_NOZORDER);
            if (g_hEditScpRemote)
                SetWindowPos(g_hEditScpRemote, nullptr, cx, g_yScpRemote, cw - 198, LAYOUT_INPUT_H, SWP_NOZORDER);
            if (g_hBtnScpPush)
                SetWindowPos(g_hBtnScpPush, nullptr, cx + cw - 196, g_yScpRemote, 90, LAYOUT_INPUT_H, SWP_NOZORDER);
            if (g_hBtnScpPull)
                SetWindowPos(g_hBtnScpPull, nullptr, cx + cw - 90, g_yScpRemote, 90, LAYOUT_INPUT_H, SWP_NOZORDER);
        }

        if (g_hProgress)
            SetWindowPos(g_hProgress, nullptr, M, g_yProg, W, 22, SWP_NOZORDER);

        if (g_hEditOutput) {
            int outH = (std::max)(120, clientH - g_yOutput - 10);
            SetWindowPos(g_hEditOutput, nullptr, M, g_yOutput, W, outH, SWP_NOZORDER);
        }

        return 0;
    }

    case WM_CLOSE: {
        // Graceful shutdown: stop background operations first
        WriteLog("WM_CLOSE received, stopping background operations");
        if (g_sshExecutor) {
            g_sshExecutor->stopLoop();
        }
        if (g_vncProxy) {
            g_vncProxy->stopAllProxies();
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY: {
        WriteLog("Window destroyed");
        // Background operations already stopped in WM_CLOSE
        g_sshExecutor.reset();
        g_vncProxy.reset();
        g_httpServer.reset();
        g_vncWindows.clear();

        for (auto f : {g_fontNormal, g_fontBold, g_fontMono, g_fontTitle, g_fontSmall})
            if (f) DeleteObject(f);
        for (auto b : {g_brBg, g_brCard, g_brInput, g_brBtn})
            if (b) DeleteObject(b);

        PostQuitMessage(0);
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ==================== WinMain ====================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    try {
        WriteLog("=== SeewoKiller v3 Starting ===");

        // Init Common Controls
        INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_TAB_CLASSES };
        InitCommonControlsEx(&icex);

        // Init Winsock
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            MessageBoxW(nullptr, L"Winsock init failed", L"Error", MB_ICONERROR);
            return 1;
        }

        // Init COM (for WebView2)
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        // Init libssh
        ssh_init();

        // Determine res directory
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        g_resDir = WstringToGbk((exeDir / "res").wstring());

        // Load plugins BEFORE creating the window (WM_CREATE builds buttons
        // from g_plugins and the layout height depends on plugin count).
        {
            auto pluginsDir = (exeDir / "plugins").string();
            g_plugins = PluginLoader::loadFromDirectory(pluginsDir);
            WriteLog("Loaded " + std::to_string(g_plugins.size()) + " plugins from " + pluginsDir);
        }

        // Start HTTP file server
        g_httpServer = std::make_unique<HttpFileServer>();
        int httpPort = g_httpServer->start(18923, g_resDir);
        WriteLog("HTTP server on port " + std::to_string(httpPort));

        // Init VNC proxy
        g_vncProxy = std::make_unique<VNCProxy>();

        // Create main window first (needed for callback)
        const wchar_t CLASS_NAME[] = L"SeewoKillerV3Main";
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hbrBackground = CreateSolidBrush(CLR_BG);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);  // Use default icon
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassW(&wc);

        // Estimate client height so the window fits the whole layout. Mirrors
        // WM_CREATE geometry; the flexible output area absorbs any small error.
        {
            int h = 52;                                          // below title
            h += 24 + 8 + 90 + LAYOUT_SECTION_GAP;               // 目标主机
            h += 24 + LAYOUT_INPUT_H + LAYOUT_SECTION_GAP;       // 连接配置
            h += 230 + LAYOUT_SECTION_GAP;                       // 命令/传输 Tab 控件
            h += 22 + 10;                                        // progress + gap
            h += 24 + 180;                                       // 执行输出 label + output
            int clientH = h + 12;
            int winH = clientH + 46;                             // title bar + borders
            g_hMainWnd = CreateWindowExW(0, CLASS_NAME, L"Seewo Killer",
                WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME,
                CW_USEDEFAULT, CW_USEDEFAULT, 760, winH,
                nullptr, nullptr, hInst, nullptr);
        }

        if (!g_hMainWnd) {
            MessageBoxW(nullptr, L"Window creation failed", L"Error", MB_ICONERROR);
            WSACleanup();
            return 1;
        }

        ShowWindow(g_hMainWnd, nCmdShow);
        UpdateWindow(g_hMainWnd);
        WriteLog("Window created, hwnd=" + std::to_string((uintptr_t)g_hMainWnd));

        // Init SSH executor - now g_hMainWnd is valid
        g_sshExecutor = std::make_unique<BatchSSHExecutor>();
        g_sshExecutor->setOutputCallback([](const std::string& text) {
            OutputData* pData = new OutputData{ text };
            PostMessage(g_hMainWnd, WM_UPDATE_OUTPUT, 0, (LPARAM)pData);
        });
        g_sshExecutor->setProgressCallback([](const ProgressInfo& prog) {
            PostMessage(g_hMainWnd, WM_UPDATE_PROGRESS, (WPARAM)prog.percent, 0);
        });

        // Message loop
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Cleanup
        ssh_finalize();
        CoUninitialize();
        WSACleanup();
        WriteLog("=== SeewoKiller v3 Exited ===");
        return 0;
    }
    catch (const std::exception& e) {
        WriteLog("Fatal exception in wWinMain: " + std::string(e.what()));
        MessageBoxA(nullptr, ("Program crashed: " + std::string(e.what())).c_str(), "Error", MB_ICONERROR);
        return 1;
    }
    catch (...) {
        WriteLog("Unknown fatal exception in wWinMain");
        MessageBoxW(nullptr, L"Program crashed with unknown error", L"Error", MB_ICONERROR);
        return 1;
    }
}
