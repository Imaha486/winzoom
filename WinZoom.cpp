// WinZoom.cpp
// 任意のウインドウだけを拡大表示し、マウス入力を元のウインドウへ自然に届けるツール
//
// 仕組み
//  ・表示 : DWM サムネイル API で元ウインドウ(とそのメニュー/ダイアログ等)を GPU 拡大し、
//           クリック透過のオーバーレイウインドウに描画する(画面コピーなし・遅延ほぼゼロ)
//  ・入力 : メッセージ偽装はしない。低レベルマウスフックでカーソル移動量を 1/倍率 にして
//           「本物のカーソル」を元ウインドウ上の対応位置に置く。クリック・ドラッグ・ホイールは
//           本物のハードウェア入力としてそのまま届く。見た目のカーソルは拡大側に別途描画する。
//  ・キー : フォーカスは元ウインドウに残るので何もしなくても届く。
//
// Build: Visual Studio 2026 (v145) / x64 / Unicode / /utf-8

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

// ---------------------------------------------------------------------------
// 定数
// ---------------------------------------------------------------------------
constexpr wchar_t kMainClass[]    = L"WinZoom.Main";
constexpr wchar_t kOverlayClass[] = L"WinZoom.Overlay";
constexpr wchar_t kCursorClass[]  = L"WinZoom.Cursor";
constexpr wchar_t kVersion[]      = L"1.0.0";

constexpr UINT WM_TRAYICON      = WM_APP + 1;
constexpr UINT WM_APP_REFRESH   = WM_APP + 2;
constexpr UINT WM_APP_REINJECT  = WM_APP + 3;
constexpr UINT WM_APP_SCALESTEP = WM_APP + 4;

constexpr ULONG_PTR kInjectMagic = 0x575A4F4DUL;   // 自分で再送したクリックの印

enum : int { HK_TOGGLE = 1, HK_UP, HK_DOWN, HK_UP_NUM, HK_DOWN_NUM };
enum : UINT_PTR { TIMER_SYNC = 1, TIMER_CURSOR = 2 };
enum : UINT { IDM_TOGGLE = 100, IDM_STOP, IDM_HELP, IDM_EXIT, IDM_SCALE_BASE = 200 };

constexpr double kScales[] = { 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 3.0, 3.5, 4.0 };
constexpr int kScaleCount = static_cast<int>(sizeof(kScales) / sizeof(kScales[0]));

using PFN_MagInitialize       = BOOL(WINAPI*)();
using PFN_MagUninitialize     = BOOL(WINAPI*)();
using PFN_MagShowSystemCursor = BOOL(WINAPI*)(BOOL);

// ---------------------------------------------------------------------------
// 状態
// ---------------------------------------------------------------------------
struct Tracked {
    HWND hwnd = nullptr;
    HTHUMBNAIL thumb = nullptr;
    RECT win{};     // GetWindowRect(見えないリサイズ枠を含む)
    RECT frame{};   // 見えている枠
    RECT src{};     // サムネイルのソース全体が対応する実画面矩形
};

struct App {
    HINSTANCE inst = nullptr;
    HWND main = nullptr, overlay = nullptr, cursorWnd = nullptr;
    HICON icon = nullptr;
    UINT msgTaskbarCreated = 0;
    HHOOK mouseHook = nullptr;
    HWINEVENTHOOK fgHook = nullptr;
    std::vector<HWINEVENTHOOK> sessionHooks;
    HWND lastFg = nullptr;
    double defaultScale = 1.5;
    std::wstring iniPath;
    bool isWin11 = false;
    bool selfElevated = false;

    HMODULE magLib = nullptr;
    PFN_MagUninitialize magUninit = nullptr;
    PFN_MagShowSystemCursor magShowCursor = nullptr;
    bool sysCursorHidden = false;

    // --- セッション(拡大中の対象) ---
    bool active = false;
    HWND target = nullptr;
    DWORD pid = 0, tid = 0;
    std::wstring exe;
    double scale = 1.5;
    bool suspended = false;
    std::vector<Tracked> wins;      // 下→上の Z 順
    std::vector<RECT> zones;        // 拡大座標で操作を受け付ける領域
    std::vector<RECT> occluders;    // オーバーレイより手前にある他アプリの矩形
    POINT origin{};                 // 拡大の基準点(対象ウインドウの左上)
    std::vector<RECT> layoutSig;
    RECT overlayRect{};
    bool overlayVisible = false;
    bool refreshPosted = false;

    // --- 入力 ---
    bool mapped = false;            // カーソルが拡大側にいて写像中か
    double rfx = 0, rfy = 0;        // 実カーソル位置(サブピクセル)
    POINT lastR{};                  // 最後に実際に置いた実カーソル位置
    POINT expect{};
    bool expectValid = false;
    int buttons = 0;
    bool inMoveSize = false;
    int moveKind = 0;               // 0:未判定 1:移動 2:サイズ変更
    SIZE moveStartSize{};
    POINT moveStartPos{};

    bool reinjectPending = false;
    DWORD reinjectDownFlag = 0, reinjectUpFlag = 0, reinjectData = 0;
    bool reinjectUpSeen = false;

    bool posAdjusted = false;
    POINT origWinPos{}, adjWinPos{};

    // --- 描画カーソル ---
    HCURSOR curHandle = nullptr;
    HBITMAP curBmp = nullptr;
    SIZE curSize{};
    POINT curHot{};
    bool curDirty = false;
    bool cursorWndVisible = false;
    POINT cursorWndPos{ INT_MIN, INT_MIN };
};

App g;

// 前方宣言
void Refresh();
void StopSession(bool restorePos);
void ApplyScale(double s, bool fromUser);

// ---------------------------------------------------------------------------
// 小物
// ---------------------------------------------------------------------------
inline int RW(const RECT& r) { return r.right - r.left; }
inline int RH(const RECT& r) { return r.bottom - r.top; }
inline bool RectEq(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
inline bool PtIn(const RECT& r, double x, double y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
inline double MapX(double x)   { return g.origin.x + (x - g.origin.x) * g.scale; }
inline double MapY(double y)   { return g.origin.y + (y - g.origin.y) * g.scale; }
inline double UnmapX(double v) { return g.origin.x + (v - g.origin.x) / g.scale; }
inline double UnmapY(double v) { return g.origin.y + (v - g.origin.y) / g.scale; }

RECT MapRect(const RECT& r) {
    RECT m;
    m.left   = static_cast<LONG>(std::lround(MapX(r.left)));
    m.top    = static_cast<LONG>(std::lround(MapY(r.top)));
    m.right  = static_cast<LONG>(std::lround(MapX(r.right)));
    m.bottom = static_cast<LONG>(std::lround(MapY(r.bottom)));
    return m;
}

bool IsOurs(HWND h) { return h && (h == g.main || h == g.overlay || h == g.cursorWnd); }

bool IsCloaked(HWND h) {
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0;
}

RECT FrameRect(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) || IsRectEmpty(&r))
        GetWindowRect(h, &r);
    return r;
}

bool ClassIs(HWND h, const wchar_t* name) {
    wchar_t c[96];
    return GetClassNameW(h, c, 96) && wcscmp(c, name) == 0;
}

bool IsShellWindow(HWND h) {
    static const wchar_t* kShell[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
        L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland",
        L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
    };
    wchar_t c[96];
    if (!GetClassNameW(h, c, 96)) return false;
    for (auto k : kShell) if (wcscmp(c, k) == 0) return true;
    return false;
}

bool IsTracked(HWND h) {
    for (auto& t : g.wins) if (t.hwnd == h) return true;
    return false;
}

void ClampToDesktop(double& x, double& y) {
    const double l = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const double t = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const double r = l + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1;
    const double b = t + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1;
    x = std::clamp(x, l, r);
    y = std::clamp(y, t, b);
}

bool AnyButtonDown() {
    const int keys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int k : keys) if (GetAsyncKeyState(k) & 0x8000) return true;
    return false;
}

int CountButtonsDown() {
    int n = 0;
    const int keys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int k : keys) if (GetAsyncKeyState(k) & 0x8000) ++n;
    return n;
}

void PostRefresh() {
    if (!g.refreshPosted) {
        g.refreshPosted = true;
        PostMessageW(g.main, WM_APP_REFRESH, 0, 0);
    }
}

bool DetectWin11() {
    using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto f = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    return f && f(&vi) == 0 && vi.dwBuildNumber >= 22000;
}

// 戻り値: 1=昇格している 0=していない -1=判定不能(たいてい相手が昇格している)
int ProcessElevation(DWORD pid) {
    HANDLE hp = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : GetCurrentProcess();
    if (!hp) return -1;
    int result = -1;
    HANDLE tok = nullptr;
    if (OpenProcessToken(hp, TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te{};
        DWORD len = 0;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &len)) result = te.TokenIsElevated ? 1 : 0;
        CloseHandle(tok);
    }
    if (pid) CloseHandle(hp);
    return result;
}

std::wstring ExeNameOf(DWORD pid) {
    std::wstring name;
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hp) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
        if (QueryFullProcessImageNameW(hp, 0, buf, &n)) {
            const wchar_t* p = wcsrchr(buf, L'\\');
            name = p ? p + 1 : buf;
        }
        CloseHandle(hp);
    }
    for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
    return name;
}

// ---------------------------------------------------------------------------
// 設定(INI)
// ---------------------------------------------------------------------------
void InitIniPath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    std::wstring dir = (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf) : std::wstring(L".");
    dir += L"\\WinZoom";
    CreateDirectoryW(dir.c_str(), nullptr);
    g.iniPath = dir + L"\\WinZoom.ini";
}

double ReadScale(const wchar_t* sec, const wchar_t* key, double def) {
    wchar_t buf[32]{};
    GetPrivateProfileStringW(sec, key, L"", buf, 32, g.iniPath.c_str());
    const double v = _wtof(buf);
    return (v >= 1.0 && v <= 8.0) ? v : def;
}

void WriteScale(const wchar_t* sec, const wchar_t* key, double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.2f", v);
    WritePrivateProfileStringW(sec, key, buf, g.iniPath.c_str());
}

// ---------------------------------------------------------------------------
// システムカーソルの表示切替(拡大鏡 API。プロセス全体ではなく画面全体に効く)
// ---------------------------------------------------------------------------
void InitMagnification() {
    g.magLib = LoadLibraryW(L"Magnification.dll");
    if (!g.magLib) return;
    auto init = reinterpret_cast<PFN_MagInitialize>(reinterpret_cast<void*>(GetProcAddress(g.magLib, "MagInitialize")));
    g.magUninit = reinterpret_cast<PFN_MagUninitialize>(reinterpret_cast<void*>(GetProcAddress(g.magLib, "MagUninitialize")));
    g.magShowCursor = reinterpret_cast<PFN_MagShowSystemCursor>(reinterpret_cast<void*>(GetProcAddress(g.magLib, "MagShowSystemCursor")));
    if (!init || !init()) { g.magShowCursor = nullptr; g.magUninit = nullptr; }
}

void ShowSystemCursor(bool show) {
    if (!g.magShowCursor) return;
    if (show && g.sysCursorHidden)       { g.magShowCursor(TRUE);  g.sysCursorHidden = false; }
    else if (!show && !g.sysCursorHidden) { g.magShowCursor(FALSE); g.sysCursorHidden = true; }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    if (g.magShowCursor && g.sysCursorHidden) g.magShowCursor(TRUE);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// トレイ
// ---------------------------------------------------------------------------
void DrawLens(HDC dc, int sz, COLORREF ring, COLORREF glass) {
    const int t = std::max(2, sz / 8);
    HPEN pen = CreatePen(PS_SOLID, t, ring);
    HBRUSH br = CreateSolidBrush(glass);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    const int d = sz * 11 / 16;
    Ellipse(dc, t / 2 + 1, t / 2 + 1, d, d);
    HPEN pen2 = CreatePen(PS_SOLID, t + 1, ring);
    SelectObject(dc, pen2);
    MoveToEx(dc, d - t, d - t, nullptr);
    LineTo(dc, sz - t / 2 - 1, sz - t / 2 - 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(pen2);
    DeleteObject(br);
}

HICON MakeTrayIcon() {
    const int sz = GetSystemMetrics(SM_CXSMICON);
    // リソースのアイコンを優先(無ければ GDI で描く)
    if (HICON res = static_cast<HICON>(LoadImageW(g.inst, L"IDI_APP", IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR)))
        return res;
    HDC sdc = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(sdc);
    HBITMAP color = CreateCompatibleBitmap(sdc, sz, sz);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    RECT r{ 0, 0, sz, sz };
    HGDIOBJ old = SelectObject(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    DrawLens(dc, sz, RGB(240, 240, 240), RGB(70, 150, 255));
    SelectObject(dc, mask);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    DrawLens(dc, sz, RGB(0, 0, 0), RGB(0, 0, 0));
    SelectObject(dc, old);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    ReleaseDC(nullptr, sdc);
    return ic ? ic : LoadIconW(nullptr, IDI_APPLICATION);
}

void TrayAdd() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.main;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g.icon;
    wcscpy_s(nid.szTip, L"WinZoom  (Ctrl+Alt+Z で拡大/解除)");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void TrayRemove() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.main;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void Balloon(const wchar_t* text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.main;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcscpy_s(nid.szInfoTitle, L"WinZoom");
    wcsncpy_s(nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------
// 見た目のカーソル
// ---------------------------------------------------------------------------
// 黒背景と白背景に描き分けて差分からアルファを復元する(モノクロ/反転/αカーソル全対応)。
// 反転ピクセル(Iビーム等)は黒＋白フチにして、どの背景でも見えるようにする。
bool RenderCursor(HCURSOR hc) {
    ICONINFO ii{};
    if (!GetIconInfo(hc, &ii)) return false;
    BITMAP bm{};
    int w = 0, h = 0;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) { w = bm.bmWidth; h = bm.bmHeight; }
    else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm)) { w = bm.bmWidth; h = bm.bmHeight / 2; }
    const POINT hot{ static_cast<LONG>(ii.xHotspot), static_cast<LONG>(ii.yHotspot) };
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    void *pb = nullptr, *pw = nullptr, *pr = nullptr;
    HBITMAP bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pb, nullptr, 0);
    HBITMAP bw = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pw, nullptr, 0);
    HBITMAP br = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pr, nullptr, 0);
    if (!bb || !bw || !br) {
        if (bb) DeleteObject(bb);
        if (bw) DeleteObject(bw);
        if (br) DeleteObject(br);
        DeleteDC(dc);
        return false;
    }
    const int n = w * h;
    auto* B = static_cast<uint32_t*>(pb);
    auto* W = static_cast<uint32_t*>(pw);
    auto* R = static_cast<uint32_t*>(pr);
    std::fill(B, B + n, 0x00000000u);
    std::fill(W, W + n, 0x00FFFFFFu);

    HGDIOBJ old = SelectObject(dc, bb);
    DrawIconEx(dc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
    SelectObject(dc, bw);
    DrawIconEx(dc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    GdiFlush();

    std::vector<uint8_t> inv(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        const int bR = (B[i] >> 16) & 255, bG = (B[i] >> 8) & 255, bB = B[i] & 255;
        const int wR = (W[i] >> 16) & 255, wG = (W[i] >> 8) & 255, wB = W[i] & 255;
        const int dR = wR - bR, dG = wG - bG, dB = wB - bB;
        if (dR < -64 || dG < -64 || dB < -64) {     // 背景を反転するピクセル
            inv[static_cast<size_t>(i)] = 1;
            R[i] = 0xFF000000u;
            continue;
        }
        const int d = (std::max(0, dR) + std::max(0, dG) + std::max(0, dB)) / 3;
        const int a = std::clamp(255 - d, 0, 255);
        const int cr = std::min(bR, a), cg = std::min(bG, a), cb = std::min(bB, a);
        R[i] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(cr) << 16) |
               (static_cast<uint32_t>(cg) << 8) | static_cast<uint32_t>(cb);
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!inv[static_cast<size_t>(y * w + x)]) continue;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const int j = ny * w + nx;
                    if (!inv[static_cast<size_t>(j)] && (R[j] >> 24) == 0) R[j] = 0xFFFFFFFFu;
                }
            }
        }
    }
    DeleteObject(bb);
    DeleteObject(bw);
    DeleteDC(dc);

    if (g.curBmp) DeleteObject(g.curBmp);
    g.curBmp = br;
    g.curSize = { w, h };
    g.curHot = hot;
    g.curDirty = true;
    return true;
}

void HideCursorWnd() {
    if (g.cursorWndVisible) {
        ShowWindow(g.cursorWnd, SW_HIDE);
        g.cursorWndVisible = false;
    }
}

void UpdateCursorWnd(double vx, double vy, bool pollShape) {
    if (pollShape || !g.curBmp) {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && ci.hCursor && ci.hCursor != g.curHandle) {
            if (RenderCursor(ci.hCursor)) g.curHandle = ci.hCursor;
        }
    }
    if (!g.curBmp) return;

    const POINT dst{ static_cast<LONG>(std::floor(vx)) - g.curHot.x,
                     static_cast<LONG>(std::floor(vy)) - g.curHot.y };
    if (g.curDirty) {
        HDC sdc = GetDC(nullptr);
        HDC mdc = CreateCompatibleDC(sdc);
        HGDIOBJ o = SelectObject(mdc, g.curBmp);
        SIZE sz = g.curSize;
        POINT src{ 0, 0 };
        POINT d = dst;
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g.cursorWnd, sdc, &d, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mdc, o);
        DeleteDC(mdc);
        ReleaseDC(nullptr, sdc);
        g.curDirty = false;
        g.cursorWndPos = dst;
    } else if (dst.x != g.cursorWndPos.x || dst.y != g.cursorWndPos.y) {
        SetWindowPos(g.cursorWnd, nullptr, dst.x, dst.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
        g.cursorWndPos = dst;
    }
    if (!g.cursorWndVisible) {
        SetWindowPos(g.cursorWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g.cursorWndVisible = true;
    }
}

// ---------------------------------------------------------------------------
// 追跡するウインドウ群(本体＋メニュー/ドロップダウン/ツールチップ/所有ダイアログ)
// ---------------------------------------------------------------------------
bool ForegroundIsTargetProcess() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == g.pid;
}

bool IsRelated(HWND h, bool fgOurs) {
    if (IsOurs(h)) return false;
    if (h == g.target) return true;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(h, &pid);
    if (pid != g.pid) return false;
    if (!IsWindowVisible(h) || IsIconic(h) || IsCloaked(h)) return false;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right <= r.left || r.bottom <= r.top) return false;
    // 対象が背面にいる時の最前面ツールチップ等は、拡大せず他アプリ扱いにする
    if (!fgOurs && (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST)) return false;
    for (HWND o = GetWindow(h, GW_OWNER); o; o = GetWindow(o, GW_OWNER))
        if (o == g.target) return true;
    if (tid == g.tid) {
        const LONG st = GetWindowLongW(h, GWL_STYLE);
        if ((st & WS_CAPTION) != WS_CAPTION) return true;   // 同スレッドのポップアップ類
    }
    return false;
}

struct EnumCtx { std::vector<HWND> list; bool fgOurs = false; };

BOOL CALLBACK EnumTopProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    if (IsRelated(h, c->fgOurs)) c->list.push_back(h);
    return TRUE;
}

// list は下→上。共通する下側はサムネイルを使い回し、ちらつきを防ぐ
void SyncTracked(const std::vector<HWND>& list) {
    size_t keep = 0;
    while (keep < g.wins.size() && keep < list.size() && g.wins[keep].hwnd == list[keep]) ++keep;
    if (keep == g.wins.size() && keep == list.size()) return;
    for (size_t i = keep; i < g.wins.size(); ++i)
        if (g.wins[i].thumb) DwmUnregisterThumbnail(g.wins[i].thumb);
    g.wins.resize(keep);
    for (size_t i = keep; i < list.size(); ++i) {
        Tracked t;
        t.hwnd = list[i];
        if (FAILED(DwmRegisterThumbnail(g.overlay, list[i], &t.thumb))) t.thumb = nullptr;
        g.wins.push_back(t);
    }
    g.layoutSig.clear();
}

void ClearTracked() {
    for (auto& t : g.wins) if (t.thumb) DwmUnregisterThumbnail(t.thumb);
    g.wins.clear();
}

bool WantsRoundCorners(HWND h) {
    if (!g.isWin11 || IsZoomed(h)) return false;
    const LONG st = GetWindowLongW(h, GWL_STYLE);
    return (st & WS_CAPTION) == WS_CAPTION || ClassIs(h, L"#32768");
}

// ---------------------------------------------------------------------------
// レイアウト
// ---------------------------------------------------------------------------
void Layout() {
    if (!g.active || g.suspended || g.wins.empty()) return;
    const Tracked* tgt = nullptr;
    for (auto& t : g.wins) {
        GetWindowRect(t.hwnd, &t.win);
        t.frame = FrameRect(t.hwnd);
        if (t.hwnd == g.target) tgt = &t;
    }
    if (!tgt) return;
    g.origin = { tgt->frame.left, tgt->frame.top };

    // サムネイルのソースが見えない枠を含むかどうかを実測して合わせる
    for (auto& t : g.wins) {
        t.src = t.win;
        SIZE ss{};
        if (t.thumb && SUCCEEDED(DwmQueryThumbnailSourceSize(t.thumb, &ss))) {
            if (std::abs(ss.cx - RW(t.frame)) <= 1 && std::abs(ss.cy - RH(t.frame)) <= 1 &&
                RW(t.frame) != RW(t.win))
                t.src = t.frame;
        }
    }

    std::vector<RECT> sig;
    sig.reserve(g.wins.size() * 2 + 1);
    RECT bb{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
    g.zones.clear();
    for (auto& t : g.wins) {
        const RECT mf = MapRect(t.frame);
        bb.left = std::min(bb.left, mf.left);
        bb.top = std::min(bb.top, mf.top);
        bb.right = std::max(bb.right, mf.right);
        bb.bottom = std::max(bb.bottom, mf.bottom);
        // 本体は見えないリサイズ枠ぶんも操作領域に含める(拡大側の縁からリサイズできる)
        g.zones.push_back(t.hwnd == g.target ? MapRect(t.win) : mf);
        sig.push_back(t.frame);
        sig.push_back(t.src);
    }
    sig.push_back(RECT{ static_cast<LONG>(std::lround(g.scale * 1000)), 0, 0, 0 });
    if (sig.size() == g.layoutSig.size() &&
        std::equal(sig.begin(), sig.end(), g.layoutSig.begin(), RectEq))
        return;
    g.layoutSig = sig;
    g.overlayRect = bb;

    SetWindowPos(g.overlay, nullptr, bb.left, bb.top, RW(bb), RH(bb),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);

    UINT dpi = GetDpiForWindow(g.overlay);
    if (!dpi) dpi = 96;
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    for (auto& t : g.wins) {
        RECT m = MapRect(t.frame);
        OffsetRect(&m, -bb.left, -bb.top);
        const int rad = WantsRoundCorners(t.hwnd) ? static_cast<int>(std::lround(8.0 * dpi / 96.0 * g.scale)) : 0;
        HRGN part = rad > 0 ? CreateRoundRectRgn(m.left, m.top, m.right + 1, m.bottom + 1, rad * 2, rad * 2)
                            : CreateRectRgnIndirect(&m);
        CombineRgn(rgn, rgn, part, RGN_OR);
        DeleteObject(part);
    }
    SetWindowRgn(g.overlay, rgn, TRUE);   // rgn の所有権はシステムへ

    for (auto& t : g.wins) {
        if (!t.thumb) continue;
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        RECT d = MapRect(t.src);
        OffsetRect(&d, -bb.left, -bb.top);
        p.rcDestination = d;
        p.fVisible = TRUE;
        p.opacity = 255;
        p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(t.thumb, &p);
    }
}

// オーバーレイを「対象ウインドウ群の一番上の真上」に置く
void ApplyZOrder() {
    if (!g.overlayVisible) return;
    HWND top = nullptr;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
        if (IsTracked(h)) { top = h; break; }
    if (!top) return;

    HWND below = GetWindow(g.overlay, GW_HWNDNEXT);
    while (below && below != top && (!IsWindowVisible(below) || IsOurs(below)))
        below = GetWindow(below, GW_HWNDNEXT);
    const bool topTopmost = (GetWindowLongW(top, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const bool ovTopmost = (GetWindowLongW(g.overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const UINT fl = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;

    if (below != top || topTopmost != ovTopmost) {
        if (topTopmost != ovTopmost)
            SetWindowPos(g.overlay, topTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, fl);
        HWND prev = GetWindow(top, GW_HWNDPREV);
        while (prev && IsOurs(prev)) prev = GetWindow(prev, GW_HWNDPREV);
        HWND after = prev ? prev : (topTopmost ? HWND_TOPMOST : HWND_TOP);
        SetWindowPos(g.overlay, after, 0, 0, 0, 0, fl);
    }
    if (g.cursorWndVisible && GetTopWindow(nullptr) != g.cursorWnd)
        SetWindowPos(g.cursorWnd, HWND_TOPMOST, 0, 0, 0, 0, fl);
}

void RebuildOccluders() {
    g.occluders.clear();
    for (HWND h = GetTopWindow(nullptr); h && h != g.overlay; h = GetWindow(h, GW_HWNDNEXT)) {
        if (IsOurs(h) || IsTracked(h)) continue;
        if (!IsWindowVisible(h) || IsIconic(h) || IsCloaked(h)) continue;
        const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) continue;    // クリック透過の飾り
        if (ex & WS_EX_LAYERED) {
            BYTE a = 255;
            DWORD fl = 0;
            COLORREF key = 0;
            if (GetLayeredWindowAttributes(h, &key, &a, &fl) && (fl & LWA_ALPHA) && a == 0) continue;
        }
        const RECT r = FrameRect(h);
        if (IsRectEmpty(&r)) continue;
        g.occluders.push_back(r);
    }
}

bool InZone(double x, double y) {
    for (auto& z : g.zones) if (PtIn(z, x, y)) return true;
    return false;
}

bool Occluded(double x, double y) {
    for (auto& r : g.occluders) if (PtIn(r, x, y)) return true;
    return false;
}

bool VirtualHit(double x, double y) {
    return g.active && !g.suspended && InZone(x, y) && !Occluded(x, y);
}

// ---------------------------------------------------------------------------
// カーソル写像
// ---------------------------------------------------------------------------
void MoveCursorTo(POINT p) {
    g.expect = p;
    g.expectValid = true;
    g.lastR = p;
    SetCursorPos(p.x, p.y);
}

void EnterMapped(double vx, double vy) {
    g.mapped = true;
    g.rfx = UnmapX(vx);
    g.rfy = UnmapY(vy);
    ShowSystemCursor(false);
    UpdateCursorWnd(vx, vy, true);
    MoveCursorTo({ static_cast<LONG>(std::floor(g.rfx)), static_cast<LONG>(std::floor(g.rfy)) });
    SetTimer(g.main, TIMER_CURSOR, 16, nullptr);
}

void LeaveMapped(double vx, double vy, bool moveCursor) {
    g.mapped = false;
    KillTimer(g.main, TIMER_CURSOR);
    if (moveCursor) {
        ClampToDesktop(vx, vy);
        MoveCursorTo({ static_cast<LONG>(std::lround(vx)), static_cast<LONG>(std::lround(vy)) });
    }
    ShowSystemCursor(true);
    HideCursorWnd();
}

// 戻り値 true = このイベントは握りつぶす(自分でカーソルを置いた)
bool OnMouseMove(POINT pt) {
    if (g.suspended) return false;
    if (!g.mapped) {
        if (g.buttons == 0 && VirtualHit(pt.x, pt.y)) {
            EnterMapped(pt.x, pt.y);
            return true;
        }
        return false;
    }

    POINT cur;
    GetCursorPos(&cur);
    if (cur.x != g.lastR.x || cur.y != g.lastR.y) {   // 他者がカーソルを動かした
        g.rfx = cur.x;
        g.rfy = cur.y;
        g.lastR = cur;
    }
    if (g.inMoveSize && g.moveKind == 1) {             // タイトルバーで移動中は等倍で素通し
        g.rfx = pt.x;
        g.rfy = pt.y;
        g.lastR = pt;
        UpdateCursorWnd(MapX(pt.x), MapY(pt.y), false);
        return false;
    }

    double vx = MapX(g.rfx) + (pt.x - cur.x);
    double vy = MapY(g.rfy) + (pt.y - cur.y);
    ClampToDesktop(vx, vy);
    const bool holding = g.buttons > 0 || g.inMoveSize;
    if (!holding && !VirtualHit(vx, vy)) {
        LeaveMapped(vx, vy, true);
        return true;
    }
    g.rfx = UnmapX(vx);
    g.rfy = UnmapY(vy);
    const POINT nr{ static_cast<LONG>(std::floor(g.rfx)), static_cast<LONG>(std::floor(g.rfy)) };
    if (nr.x != g.lastR.x || nr.y != g.lastR.y) MoveCursorTo(nr);
    UpdateCursorWnd(vx, vy, false);
    return true;
}

bool ButtonFlags(WPARAM wp, const MSLLHOOKSTRUCT* m, DWORD& down, DWORD& up, DWORD& data) {
    data = 0;
    switch (wp) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: down = MOUSEEVENTF_LEFTDOWN;   up = MOUSEEVENTF_LEFTUP;   return true;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP;  return true;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; return true;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        down = MOUSEEVENTF_XDOWN; up = MOUSEEVENTF_XUP; data = HIWORD(m->mouseData); return true;
    }
    return false;
}

// 拡大側は見えているが、実ウインドウ側が他アプリに隠れている場合だけ
// 「対象を前面化してからクリックを再送」する(普通は何もせず素通し)
bool OnButtonDown(WPARAM wp, const MSLLHOOKSTRUCT* m) {
    if (!g.mapped || g.suspended || g.reinjectPending || g.inMoveSize) return false;
    if (!Occluded(g.lastR.x, g.lastR.y)) return false;
    DWORD down = 0, up = 0, data = 0;
    if (!ButtonFlags(wp, m, down, up, data)) return false;
    g.reinjectPending = true;
    g.reinjectUpSeen = false;
    g.reinjectDownFlag = down;
    g.reinjectUpFlag = up;
    g.reinjectData = data;
    PostMessageW(g.main, WM_APP_REINJECT, 0, 0);
    return true;
}

bool OnButtonUp(WPARAM wp, const MSLLHOOKSTRUCT* m) {
    if (!g.reinjectPending) return false;
    DWORD down = 0, up = 0, data = 0;
    if (!ButtonFlags(wp, m, down, up, data) || up != g.reinjectUpFlag) return false;
    g.reinjectUpSeen = true;
    return true;
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g.active) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        const bool down = wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN;
        const bool up = wp == WM_LBUTTONUP || wp == WM_RBUTTONUP || wp == WM_MBUTTONUP || wp == WM_XBUTTONUP;

        if ((m->flags & LLMHF_INJECTED) && m->dwExtraInfo == kInjectMagic) {
            if (down) ++g.buttons;
            else if (up && g.buttons > 0) --g.buttons;
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        if (wp == WM_MOUSEMOVE) {
            if (g.expectValid && m->pt.x == g.expect.x && m->pt.y == g.expect.y) {
                g.expectValid = false;
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            if (OnMouseMove(m->pt)) return 1;
        } else if (down) {
            if (OnButtonDown(wp, m)) return 1;
            ++g.buttons;
        } else if (up) {
            if (OnButtonUp(wp, m)) return 1;
            if (g.buttons > 0) --g.buttons;
        } else if (wp == WM_MOUSEWHEEL && g.mapped &&
                   (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
            const short delta = static_cast<short>(HIWORD(m->mouseData));
            PostMessageW(g.main, WM_APP_SCALESTEP, static_cast<WPARAM>(delta > 0 ? 1 : -1), 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void ActivateTarget() {
    HWND fg = GetForegroundWindow();
    const DWORD me = GetCurrentThreadId();
    const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = fgTid && fgTid != me && AttachThreadInput(me, fgTid, TRUE);
    HWND act = GetLastActivePopup(g.target);
    if (!act || !IsWindowVisible(act)) act = g.target;
    SetForegroundWindow(act);
    BringWindowToTop(act);
    if (attached) AttachThreadInput(me, fgTid, FALSE);
}

void DoReinject() {
    if (!g.reinjectPending) return;
    ActivateTarget();
    Refresh();
    INPUT in[2]{};
    int n = 0;
    in[n].type = INPUT_MOUSE;
    in[n].mi.dwFlags = g.reinjectDownFlag;
    in[n].mi.mouseData = g.reinjectData;
    in[n].mi.dwExtraInfo = kInjectMagic;
    ++n;
    if (g.reinjectUpSeen) {
        in[n] = in[0];
        in[n].mi.dwFlags = g.reinjectUpFlag;
        ++n;
    }
    g.reinjectPending = false;
    SendInput(static_cast<UINT>(n), in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// セッション
// ---------------------------------------------------------------------------
void Suspend() {
    g.suspended = true;
    if (g.mapped) LeaveMapped(0, 0, false);   // 実カーソルはその場(元ウインドウ上)に残す
    if (g.overlayVisible) {
        ShowWindow(g.overlay, SW_HIDE);
        g.overlayVisible = false;
    }
    g.zones.clear();
    g.occluders.clear();
}

void Refresh() {
    g.refreshPosted = false;
    if (!g.active) return;
    if (!IsWindow(g.target)) { StopSession(false); return; }

    const bool sus = !IsWindowVisible(g.target) || IsIconic(g.target) || IsZoomed(g.target) || IsCloaked(g.target);
    if (sus) {
        if (!g.suspended) Suspend();
        return;
    }
    if (g.suspended) {
        g.suspended = false;
        g.layoutSig.clear();
    }

    EnumCtx ctx;
    ctx.fgOurs = ForegroundIsTargetProcess();
    EnumWindows(EnumTopProc, reinterpret_cast<LPARAM>(&ctx));
    std::reverse(ctx.list.begin(), ctx.list.end());
    SyncTracked(ctx.list);
    Layout();
    if (!g.overlayVisible) {
        ShowWindow(g.overlay, SW_SHOWNOACTIVATE);
        g.overlayVisible = true;
    }
    ApplyZOrder();
    RebuildOccluders();

    if (g.mapped) {
        const double vx = MapX(g.rfx), vy = MapY(g.rfy);
        if (!g.inMoveSize && g.buttons == 0 && !VirtualHit(vx, vy)) LeaveMapped(vx, vy, true);
        else UpdateCursorWnd(vx, vy, false);
    }
}

// 拡大後の画面がモニタの作業領域に収まるように倍率と位置を調整
void FitTarget(bool notify) {
    const RECT f = FrameRect(g.target);
    RECT w;
    GetWindowRect(g.target, &w);
    HMONITOR mon = MonitorFromRect(&f, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT wa = mi.rcWork;
    const double fw = std::max(1, RW(f)), fh = std::max(1, RH(f));
    const double maxS = std::min(RW(wa) / fw, RH(wa) / fh);
    if (g.scale > maxS + 1e-6) {
        g.scale = std::max(1.0, std::floor(maxS * 20.0) / 20.0);
        if (notify) {
            wchar_t buf[128];
            swprintf(buf, 128, L"画面に収まる最大の倍率 %d%% にしました。", static_cast<int>(std::lround(g.scale * 100)));
            Balloon(buf);
        }
    }
    const LONG sw = static_cast<LONG>(std::lround(fw * g.scale));
    const LONG sh = static_cast<LONG>(std::lround(fh * g.scale));
    LONG nx = f.left, ny = f.top;
    if (nx + sw > wa.right) nx = wa.right - sw;
    if (nx < wa.left) nx = wa.left;
    if (ny + sh > wa.bottom) ny = wa.bottom - sh;
    if (ny < wa.top) ny = wa.top;
    if ((nx != f.left || ny != f.top) && !IsHungAppWindow(g.target)) {
        if (!g.posAdjusted) {
            g.posAdjusted = true;
            g.origWinPos = { w.left, w.top };
        }
        SetWindowPos(g.target, nullptr, w.left + (nx - f.left), w.top + (ny - f.top), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        GetWindowRect(g.target, &w);
        g.adjWinPos = { w.left, w.top };
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD ev, HWND h, LONG idObj, LONG idChild, DWORD, DWORD);

void AddSessionHooks() {
    auto add = [](DWORD a, DWORD b, DWORD pid) {
        HWINEVENTHOOK hk = SetWinEventHook(a, b, nullptr, WinEventProc, pid, 0,
                                           WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hk) g.sessionHooks.push_back(hk);
    };
    add(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, g.pid);
    add(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, g.pid);
    add(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, g.pid);
    add(EVENT_OBJECT_DESTROY, EVENT_OBJECT_REORDER, 0);   // DESTROY/SHOW/HIDE/REORDER(全体)
    add(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, 0);
}

HWND PickTarget(HWND h) {
    h = GetAncestor(h, GA_ROOT);
    if (!h) return nullptr;
    // VB6/Delphi 等の「見えないオーナー」は越えない
    for (int i = 0; i < 32; ++i) {
        HWND o = GetWindow(h, GW_OWNER);
        if (!o || !IsWindowVisible(o) || IsIconic(o) || IsCloaked(o)) break;
        h = o;
    }
    return h;
}

bool StartSession(HWND h) {
    if (!h || !IsWindow(h) || IsOurs(h) || IsShellWindow(h) || !IsWindowVisible(h) || IsIconic(h)) {
        Balloon(L"拡大できるウインドウが見つかりませんでした。");
        return false;
    }
    if (IsZoomed(h)) {
        Balloon(L"最大化中のウインドウは拡大できません。元のサイズに戻してから実行してください。");
        return false;
    }

    g.active = true;
    g.target = h;
    g.tid = GetWindowThreadProcessId(h, &g.pid);
    g.exe = ExeNameOf(g.pid);
    g.scale = g.exe.empty() ? g.defaultScale : ReadScale(L"Scale", g.exe.c_str(), g.defaultScale);
    g.suspended = false;
    g.mapped = false;
    g.inMoveSize = false;
    g.moveKind = 0;
    g.reinjectPending = false;
    g.posAdjusted = false;
    g.expectValid = false;
    g.buttons = CountButtonsDown();
    g.layoutSig.clear();
    ClearTracked();

    FitTarget(false);
    if (g.scale < 1.1) {
        g.active = false;
        g.target = nullptr;
        Balloon(L"このウインドウは画面に対して大きすぎるため拡大できません。");
        return false;
    }

    g.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g.inst, 0);
    if (!g.mouseHook) {
        g.active = false;
        g.target = nullptr;
        Balloon(L"マウスフックの設定に失敗しました。");
        return false;
    }
    AddSessionHooks();
    SetTimer(g.main, TIMER_SYNC, 100, nullptr);
    Refresh();

    if (!g.selfElevated && ProcessElevation(g.pid) != 0)
        Balloon(L"対象は管理者権限で動作している可能性があります。操作が届かない場合は WinZoom も管理者として起動してください。");

    POINT p;
    GetCursorPos(&p);
    if (g.buttons == 0 && VirtualHit(p.x, p.y)) EnterMapped(p.x, p.y);
    return true;
}

void StopSession(bool restorePos) {
    if (!g.active) return;
    const bool wasMapped = g.mapped;
    if (g.mapped) {
        // 解除後は「同じ UI 部品の上」にカーソルが残るよう、実カーソル位置のまま表示に戻す
        g.mapped = false;
        KillTimer(g.main, TIMER_CURSOR);
        HideCursorWnd();
    }
    for (auto hk : g.sessionHooks) UnhookWinEvent(hk);
    g.sessionHooks.clear();
    if (g.mouseHook) {
        UnhookWindowsHookEx(g.mouseHook);
        g.mouseHook = nullptr;
    }
    KillTimer(g.main, TIMER_SYNC);
    ClearTracked();
    if (g.overlayVisible) {
        ShowWindow(g.overlay, SW_HIDE);
        g.overlayVisible = false;
    }

    if (restorePos && g.posAdjusted && IsWindow(g.target) && !IsZoomed(g.target) && !IsIconic(g.target)) {
        RECT w;
        GetWindowRect(g.target, &w);
        if (w.left == g.adjWinPos.x && w.top == g.adjWinPos.y) {
            const LONG dx = g.origWinPos.x - w.left, dy = g.origWinPos.y - w.top;
            SetWindowPos(g.target, nullptr, g.origWinPos.x, g.origWinPos.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         (IsHungAppWindow(g.target) ? SWP_ASYNCWINDOWPOS : 0));
            if (wasMapped) {
                POINT c;
                GetCursorPos(&c);
                SetCursorPos(c.x + dx, c.y + dy);
            }
        }
    }
    ShowSystemCursor(true);

    g.active = false;
    g.target = nullptr;
    g.pid = g.tid = 0;
    g.suspended = false;
    g.zones.clear();
    g.occluders.clear();
    g.layoutSig.clear();
    g.reinjectPending = false;
    g.inMoveSize = false;
    g.posAdjusted = false;
    g.expectValid = false;
}

void ToggleFor(HWND fg) {
    if (!fg || IsOurs(fg) || IsShellWindow(fg)) fg = g.lastFg;
    if (fg && !IsWindow(fg)) fg = nullptr;
    if (g.active) {
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        const bool same = !fg || pid == g.pid;
        StopSession(true);
        if (same) return;
    }
    if (fg) StartSession(PickTarget(fg));
    else Balloon(L"拡大するウインドウを前面にしてから Ctrl+Alt+Z を押してください。");
}

void ApplyScale(double s, bool fromUser) {
    if (!g.active) {
        g.defaultScale = s;
        WriteScale(L"General", L"DefaultScale", s);
        return;
    }
    const bool wasMapped = g.mapped;
    const double vx = wasMapped ? MapX(g.rfx) : 0;
    const double vy = wasMapped ? MapY(g.rfy) : 0;
    g.scale = s;
    FitTarget(fromUser);
    if (!g.exe.empty()) WriteScale(L"Scale", g.exe.c_str(), g.scale);
    g.layoutSig.clear();
    Refresh();
    if (wasMapped && g.mapped) {
        // 見た目のカーソル位置は動かさず、実カーソル側を合わせ直す
        if (VirtualHit(vx, vy)) {
            g.rfx = UnmapX(vx);
            g.rfy = UnmapY(vy);
            MoveCursorTo({ static_cast<LONG>(std::floor(g.rfx)), static_cast<LONG>(std::floor(g.rfy)) });
            UpdateCursorWnd(vx, vy, false);
        } else {
            LeaveMapped(vx, vy, true);
        }
    }
}

void StepScale(int dir) {
    const double cur = g.active ? g.scale : g.defaultScale;
    int idx = -1;
    if (dir > 0) {
        for (int i = 0; i < kScaleCount; ++i) if (kScales[i] > cur + 1e-3) { idx = i; break; }
    } else {
        for (int i = kScaleCount - 1; i >= 0; --i) if (kScales[i] < cur - 1e-3) { idx = i; break; }
    }
    if (idx >= 0) ApplyScale(kScales[idx], true);
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD ev, HWND h, LONG idObj, LONG idChild, DWORD, DWORD) {
    if (ev == EVENT_SYSTEM_FOREGROUND) {
        if (h && !IsOurs(h) && !IsShellWindow(h)) g.lastFg = h;
        if (g.active) PostRefresh();
        return;
    }
    if (!g.active || !h) return;
    if (ev == EVENT_OBJECT_REORDER) { PostRefresh(); return; }
    if (idObj != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (IsOurs(h)) return;
    if (ev == EVENT_OBJECT_DESTROY) {
        if (h == g.target || IsTracked(h)) PostRefresh();
        return;
    }
    if (GetAncestor(h, GA_PARENT) != GetDesktopWindow()) return;   // トップレベルのみ

    switch (ev) {
    case EVENT_SYSTEM_MOVESIZESTART:
        if (h == g.target) {
            RECT r;
            GetWindowRect(h, &r);
            g.inMoveSize = true;
            g.moveKind = 0;
            g.moveStartSize = { RW(r), RH(r) };
            g.moveStartPos = { r.left, r.top };
        }
        break;
    case EVENT_SYSTEM_MOVESIZEEND:
        if (h == g.target) {
            g.inMoveSize = false;
            g.moveKind = 0;
            PostRefresh();
        }
        break;
    case EVENT_OBJECT_LOCATIONCHANGE:
        if (IsTracked(h)) {
            if (h == g.target && g.inMoveSize && g.moveKind == 0) {
                RECT r;
                GetWindowRect(h, &r);
                if (RW(r) != g.moveStartSize.cx || RH(r) != g.moveStartSize.cy) g.moveKind = 2;
                else if (r.left != g.moveStartPos.x || r.top != g.moveStartPos.y) g.moveKind = 1;
            }
            Layout();   // ドラッグ中も遅れず追従させる
            if (g.mapped) UpdateCursorWnd(MapX(g.rfx), MapY(g.rfy), false);
        }
        PostRefresh();
        break;
    default:
        PostRefresh();
        break;
    }
}

// ---------------------------------------------------------------------------
// ウインドウプロシージャ
// ---------------------------------------------------------------------------
void ShowHelp() {
    std::wstring text = L"WinZoom ";
    text += kVersion;
    text += L" の使い方\n\n"
        L"Ctrl+Alt+Z\t前面のウインドウを拡大 / 解除\n"
        L"Ctrl+Alt+＋ / －\t倍率を変更(テンキーも可)\n"
        L"Ctrl+Alt+ホイール\t拡大表示の上で倍率を変更\n\n"
        L"・拡大表示の上では普段どおりクリック・ドラッグ・ホイール・キー入力ができます\n"
        L"・メニュー、ドロップダウン、ダイアログも一緒に拡大されます\n"
        L"・倍率はアプリ(exe)ごとに記憶されます\n"
        L"・はみ出す場合はウインドウを自動で移動し、解除時に元の位置へ戻します\n"
        L"・最大化/最小化中は一時停止し、元に戻すと再開します\n\n"
        L"https://github.com/Imaha486/WinZoom";
    MessageBoxW(nullptr, text.c_str(), L"WinZoom", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    if (g.active) {
        std::wstring s = L"拡大を解除";
        if (!g.exe.empty()) s += L" (" + g.exe + L")";
        s += L"\tCtrl+Alt+Z";
        AppendMenuW(m, MF_STRING, IDM_STOP, s.c_str());
    } else {
        AppendMenuW(m, MF_STRING | (g.lastFg ? 0u : MF_GRAYED), IDM_TOGGLE, L"直前のウインドウを拡大\tCtrl+Alt+Z");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const double cur = g.active ? g.scale : g.defaultScale;
    for (int i = 0; i < kScaleCount; ++i) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%d%%", static_cast<int>(std::lround(kScales[i] * 100)));
        AppendMenuW(m, MF_STRING | (std::fabs(cur - kScales[i]) < 1e-3 ? MF_CHECKED : 0u),
                    IDM_SCALE_BASE + static_cast<UINT>(i), buf);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_HELP, L"使い方");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"終了");

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g.main);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                      p.x, p.y, 0, g.main, nullptr));
    PostMessageW(g.main, WM_NULL, 0, 0);
    DestroyMenu(m);

    if (cmd == IDM_TOGGLE) {
        if (g.lastFg && IsWindow(g.lastFg)) {
            SetForegroundWindow(g.lastFg);
            StartSession(PickTarget(g.lastFg));
        }
    } else if (cmd == IDM_STOP) {
        StopSession(true);
    } else if (cmd == IDM_HELP) {
        ShowHelp();
    } else if (cmd == IDM_EXIT) {
        DestroyWindow(g.main);
    } else if (cmd >= IDM_SCALE_BASE && cmd < IDM_SCALE_BASE + static_cast<UINT>(kScaleCount)) {
        const double s = kScales[cmd - IDM_SCALE_BASE];
        g.defaultScale = s;
        WriteScale(L"General", L"DefaultScale", s);
        if (g.active) ApplyScale(s, true);
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g.msgTaskbarCreated && msg == g.msgTaskbarCreated) {
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        switch (static_cast<int>(wp)) {
        case HK_TOGGLE:   ToggleFor(GetForegroundWindow()); break;
        case HK_UP:
        case HK_UP_NUM:   StepScale(+1); break;
        case HK_DOWN:
        case HK_DOWN_NUM: StepScale(-1); break;
        }
        return 0;
    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_SYNC) {
            if (!AnyButtonDown() && !g.reinjectPending) g.buttons = 0;
            Refresh();
        } else if (wp == TIMER_CURSOR && g.mapped) {
            UpdateCursorWnd(MapX(g.rfx), MapY(g.rfy), true);
        }
        return 0;
    case WM_APP_REFRESH:
        Refresh();
        return 0;
    case WM_APP_REINJECT:
        DoReinject();
        return 0;
    case WM_APP_SCALESTEP:
        StepScale(static_cast<int>(static_cast<INT_PTR>(wp)));
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (g.active) {
            g.layoutSig.clear();
            PostRefresh();
        }
        break;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) StopSession(true);
        return 0;
    case WM_DESTROY:
        StopSession(true);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK PassThroughProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:    return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DPICHANGED:   return 0;   // 位置とサイズは自前で管理する
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DisableDwmExtras(HWND h) {
    BOOL t = TRUE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &t, sizeof(t));
    DwmSetWindowAttribute(h, DWMWA_EXCLUDED_FROM_PEEK, &t, sizeof(t));
    const DWORD kCornerPref = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
    int doNotRound = 1;             // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(h, kCornerPref, &doNotRound, sizeof(doNotRound));
}

bool CreateWindows() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIconW(g.inst, L"IDI_APP");
    wc.hIconSm = g.icon;
    if (!RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc = PassThroughProc;
    wc.lpszClassName = kOverlayClass;
    wc.hIcon = nullptr;
    wc.hIconSm = nullptr;
    wc.hbrBackground = CreateSolidBrush(RGB(32, 32, 32));
    if (!RegisterClassExW(&wc)) return false;

    wc.lpszClassName = kCursorClass;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) return false;

    g.main = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"WinZoom", WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, g.inst, nullptr);
    g.overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kOverlayClass, L"WinZoom Overlay", WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr, g.inst, nullptr);
    g.cursorWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                  kCursorClass, L"WinZoom Cursor", WS_POPUP,
                                  0, 0, 1, 1, nullptr, nullptr, g.inst, nullptr);
    if (!g.main || !g.overlay || !g.cursorWnd) return false;
    SetLayeredWindowAttributes(g.overlay, 0, 255, LWA_ALPHA);
    DisableDwmExtras(g.overlay);
    DisableDwmExtras(g.cursorWnd);
    return true;
}

void RegisterHotkeys() {
    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    bool ok = RegisterHotKey(g.main, HK_TOGGLE, mods, 'Z') != FALSE;
    RegisterHotKey(g.main, HK_UP, MOD_CONTROL | MOD_ALT, VK_OEM_PLUS);
    RegisterHotKey(g.main, HK_DOWN, MOD_CONTROL | MOD_ALT, VK_OEM_MINUS);
    RegisterHotKey(g.main, HK_UP_NUM, MOD_CONTROL | MOD_ALT, VK_ADD);
    RegisterHotKey(g.main, HK_DOWN_NUM, MOD_CONTROL | MOD_ALT, VK_SUBTRACT);
    if (!ok) Balloon(L"Ctrl+Alt+Z が他のアプリで使用中です。トレイメニューから操作してください。");
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WinZoom.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"WinZoom はすでに起動しています(タスクトレイを確認してください)。",
                    L"WinZoom", MB_OK | MB_ICONINFORMATION);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g.inst = hInst;
    g.isWin11 = DetectWin11();
    g.selfElevated = ProcessElevation(0) == 1;
    SetUnhandledExceptionFilter(CrashFilter);
    InitIniPath();
    g.defaultScale = ReadScale(L"General", L"DefaultScale", 1.5);
    InitMagnification();
    g.icon = MakeTrayIcon();

    if (!CreateWindows()) {
        MessageBoxW(nullptr, L"ウインドウの作成に失敗しました。", L"WinZoom", MB_OK | MB_ICONERROR);
        return 1;
    }
    g.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g.main, g.msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    TrayAdd();
    RegisterHotkeys();
    g.fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc,
                               0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g.lastFg = GetForegroundWindow();
    if (g.lastFg && (IsOurs(g.lastFg) || IsShellWindow(g.lastFg))) g.lastFg = nullptr;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopSession(false);
    if (g.fgHook) UnhookWinEvent(g.fgHook);
    TrayRemove();
    ShowSystemCursor(true);
    if (g.magUninit) g.magUninit();
    if (g.curBmp) DeleteObject(g.curBmp);
    if (mutex) CloseHandle(mutex);
    return 0;
}
