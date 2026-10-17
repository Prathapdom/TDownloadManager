// TDM: native Win32 download manager (segmented HTTP/HTTPS, Tor, BitTorrent).
#include "settings.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <unordered_map>
#include <set>
#include <cwchar>
#include <ctime>

using namespace tdm;
using engine::View;

constexpr UINT WM_ENGINE = WM_APP + 1, WM_FINISHED = WM_APP + 2, WM_TRAY = WM_APP + 3, WM_RUNARGS = WM_APP + 4, WM_TESTQUERY = WM_APP + 100;
constexpr ULONG_PTR CopyDataArgs = 0x7D4A11C3;
constexpr int SearchEditId = 200;
constexpr UINT_PTR TimerSave = 1, TimerSearch = 2, TimerBanner = 3;
const wchar_t* const AppName = L"TDM";
const wchar_t* const AppTitle = L"TDM Download Manager";
const wchar_t* const MainClass = L"TDM.Main";
const wchar_t* const ListClass = L"TDM.List";
const wchar_t* const DetailsClass = L"TDM.Details";
const wchar_t* const DialogClass = L"TDM.Dialog";

enum Cmd : int {
    CmdAddUrl = 100, CmdAddTorrent, CmdResume, CmdPause, CmdDelete, CmdDeleteFiles, CmdResumeAll, CmdPauseAll, CmdLimit, CmdSettings, CmdTheme, CmdMore,
    CmdOpen, CmdOpenFolder, CmdCopyUrl, CmdRestart, CmdRecheck, CmdMoveTop, CmdMoveUp, CmdMoveDown, CmdMoveBottom, CmdProperties,
    CmdRemoveCompleted, CmdCreateTorrent, CmdToggleSidebar, CmdToggleDetails, CmdRegister, CmdSettingsFolder, CmdDownloadsFolder, CmdAbout, CmdExit,
    CmdSelectAll, CmdFind, CmdPaste, CmdBannerAdd, CmdBannerClose, CmdShow, CmdToggleItem,
    CmdFilterBase = 1000, CmdThemeBase = 3100, CmdLimitBase = 3200, CmdColumnBase = 3300, CmdDetailTabBase = 3400, CmdRecentFolderBase = 3500,
};

// ---------------------------------------------------------------- appearance

struct Palette {
    bool dark;
    COLORREF chrome, bg, surface, text, muted, border, hover, pressed, accent, accentText, accentSoft, selection, track, danger, success, warning;
};
const Palette LightPalette{false, RGB(243, 243, 243), RGB(255, 255, 255), RGB(255, 255, 255), RGB(28, 28, 28), RGB(96, 96, 96), RGB(212, 212, 212), RGB(230, 230, 230), RGB(218, 218, 218),
                           RGB(0, 95, 184), RGB(255, 255, 255), RGB(214, 232, 248), RGB(204, 228, 247), RGB(229, 229, 229), RGB(196, 43, 28), RGB(15, 123, 15), RGB(157, 93, 0)};
const Palette DarkPalette{true, RGB(39, 39, 39), RGB(28, 28, 28), RGB(45, 45, 45), RGB(232, 232, 232), RGB(160, 160, 160), RGB(68, 68, 68), RGB(55, 55, 55), RGB(64, 64, 64),
                          RGB(76, 194, 255), RGB(0, 0, 0), RGB(36, 72, 102), RGB(38, 79, 120), RGB(58, 58, 58), RGB(255, 120, 110), RGB(108, 203, 95), RGB(252, 225, 0)};
const Palette AmoledPalette{true, RGB(0, 0, 0), RGB(0, 0, 0), RGB(16, 16, 16), RGB(225, 225, 225), RGB(140, 140, 140), RGB(48, 48, 48), RGB(28, 28, 28), RGB(40, 40, 40),
                            RGB(76, 194, 255), RGB(0, 0, 0), RGB(18, 50, 80), RGB(24, 58, 92), RGB(36, 36, 36), RGB(255, 120, 110), RGB(108, 203, 95), RGB(252, 225, 0)};

// Undocumented uxtheme exports (Windows 10 1903+) for dark context menus and scrollbars.
namespace darkmode {
using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn = void(WINAPI*)();
SetPreferredAppModeFn setPreferredAppMode = nullptr;
AllowDarkModeForWindowFn allowForWindow = nullptr;
FlushMenuThemesFn flushMenuThemes = nullptr;
void init() {
    using RtlGetNtVersionNumbersFn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
    auto rtl = (RtlGetNtVersionNumbersFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers");
    DWORD major = 0, minor = 0, build = 0;
    if (rtl) rtl(&major, &minor, &build);
    build &= ~0xF0000000u;
    if (major < 10 || build < 18362) return;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    setPreferredAppMode = (SetPreferredAppModeFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    allowForWindow = (AllowDarkModeForWindowFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    flushMenuThemes = (FlushMenuThemesFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
}
void app(bool dark) { if (setPreferredAppMode) setPreferredAppMode(dark ? 2 : 3); if (flushMenuThemes) flushMenuThemes(); }
void window(HWND h, bool dark, const wchar_t* light = L"Explorer", const wchar_t* darkName = L"DarkMode_Explorer") {
    if (!h) return;
    if (allowForWindow) allowForWindow(h, dark);
    SetWindowTheme(h, dark ? darkName : light, nullptr);
}
} // namespace darkmode

HINSTANCE gInst;
HWND gMain, gList, gDetails, gSearch, gTip;
settings::Settings S;
std::wstring gSettingsPath;
engine::Engine E;
settings::ItemSaver gSaver;
Palette P = LightPalette;
int dpi = 96;
int px(int v) { return MulDiv(v, dpi, 96); }
HFONT uiFont, uiBold, uiLarge, uiSmall, iconFont, iconSmall;
std::wstring iconFace = L"Segoe Fluent Icons";
HBRUSH surfaceBrush, chromeBrush;
std::atomic<bool> enginePosted{false};
bool editGuard = false;

void fill(HDC dc, const RECT& r, COLORREF c) { SetDCBrushColor(dc, c); FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH)); }
void roundRect(HDC dc, const RECT& r, COLORREF face, COLORREF border, int radius) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, face); SetDCPenColor(dc, border);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
}
void drawText(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt = DT_LEFT) {
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}
int textWidth(HDC dc, HFONT f, const std::wstring& s) { SelectObject(dc, f); SIZE sz{}; GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz); return sz.cx; }
// Shortens a path from the middle, keeping the drive and the file name visible.
std::wstring compactPath(HDC dc, HFONT f, const std::wstring& s, int width) {
    if (width <= 0 || textWidth(dc, f, s) <= width) return s;
    size_t name = s.find_last_of(L"\\/");
    size_t keepTail = name == std::wstring::npos ? s.size() / 2 : s.size() - name;
    size_t head = std::min<size_t>(s.size() - keepTail, 24);
    for (;;) {
        std::wstring c = s.substr(0, head) + L"…" + s.substr(s.size() - keepTail);
        if (textWidth(dc, f, c) <= width) return c;
        if (head > 3) --head;
        else if (keepTail > 4) --keepTail;
        else return c;
    }
}
bool hasGlyph(wchar_t c) {
    static std::unordered_map<wchar_t, bool> known;
    auto it = known.find(c);
    if (it != known.end()) return it->second;
    HDC dc = GetDC(nullptr); HGDIOBJ o = SelectObject(dc, iconFont);
    WORD g = 0xFFFF; GetGlyphIndicesW(dc, &c, 1, &g, GGI_MARK_NONEXISTING_GLYPHS);
    SelectObject(dc, o); ReleaseDC(nullptr, dc);
    return known[c] = g != 0xFFFF;
}
void drawGlyph(HDC dc, const std::wstring& g, RECT r, COLORREF c, HFONT f = nullptr) {
    if (g.empty() || !hasGlyph(g[0])) return;
    drawText(dc, g, r, c, f ? f : iconFont, DT_CENTER);
}
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c) { RECT r{x1, y1, std::max(x2, x1 + 1), std::max(y2, y1 + 1)}; fill(dc, r, c); }
COLORREF blend(COLORREF a, COLORREF b, int pct) {
    auto m = [&](int x, int y) { return (x * (100 - pct) + y * pct) / 100; };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

std::wstring fmtNum(uint64_t v) {
    std::wstring s = std::to_wstring(v), o;
    for (size_t i = 0; i < s.size(); ++i) { if (i && (s.size() - i) % 3 == 0) o += L','; o += s[i]; }
    return o;
}
std::wstring fmtSize(int64_t b) {
    if (b < 0) return L"Unknown";
    wchar_t buf[64];
    if (b < 1024) swprintf(buf, 64, L"%lld B", (long long)b);
    else if (b < 1024 * 1024) swprintf(buf, 64, L"%.1f KB", b / 1024.0);
    else if (b < 1024ll * 1024 * 1024) swprintf(buf, 64, L"%.1f MB", b / (1024.0 * 1024));
    else if (b < 1024ll * 1024 * 1024 * 1024) swprintf(buf, 64, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    else swprintf(buf, 64, L"%.2f TB", b / (1024.0 * 1024 * 1024 * 1024));
    return buf;
}
std::wstring fmtSpeed(double bps) { return bps < 1 ? std::wstring() : fmtSize((int64_t)bps) + L"/s"; }
std::wstring fmtDuration(int64_t s) {
    if (s < 0) return L"";
    wchar_t buf[64];
    if (s < 60) swprintf(buf, 64, L"%llds", (long long)s);
    else if (s < 3600) swprintf(buf, 64, L"%lldm %02llds", (long long)(s / 60), (long long)(s % 60));
    else if (s < 86400) swprintf(buf, 64, L"%lldh %02lldm", (long long)(s / 3600), (long long)(s / 60 % 60));
    else swprintf(buf, 64, L"%lldd %lldh", (long long)(s / 86400), (long long)(s / 3600 % 24));
    return buf;
}
std::wstring fmtTime(int64_t t) {
    if (t <= 0) return L"";
    time_t tt = (time_t)t; tm lt{}; localtime_s(&lt, &tt);
    time_t now = time(nullptr); tm ln{}; localtime_s(&ln, &now);
    wchar_t buf[64];
    if (lt.tm_year == ln.tm_year && lt.tm_yday == ln.tm_yday) wcsftime(buf, 64, L"Today %H:%M", &lt);
    else wcsftime(buf, 64, L"%Y-%m-%d %H:%M", &lt);
    return buf;
}
std::wstring windowText(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(n + 1, L'\0'); GetWindowTextW(h, &s[0], n + 1); s.resize(n); return s; }
int64_t parseI64(const std::wstring& s, int64_t def = 0) { std::wstring t = trimW(s); if (t.empty()) return def; wchar_t* e; long long v = wcstoll(t.c_str(), &e, 10); return e == t.c_str() ? def : (int64_t)v; }

void titleBar(HWND h) {
    BOOL dark = P.dark;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    COLORREF caption = S.ui.theme == 3 ? RGB(0, 0, 0) : 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
    DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR, Windows 11*/, &caption, sizeof caption);
}

// ---------------------------------------------------------------- model

std::vector<View> views;                 // all items, queue order
std::vector<int> rows;                   // indices into views: filtered and sorted
std::set<uint32_t> selected;
uint32_t focusId = 0, anchorId = 0;
int filter = 0;                          // CmdFilterBase offsets: see filterName
std::wstring searchText;
double totalDown = 0, totalUp = 0;
int activeCount = 0;

enum Filter { FAll = 0, FActive, FQueued, FPaused, FCompleted, FFailed, FTorrents, FTor, FCategory = 20 };
const wchar_t* filterName(int f) {
    static const wchar_t* n[] = {L"All downloads", L"Downloading", L"Queued", L"Paused", L"Completed", L"Failed", L"Torrents", L"Through Tor"};
    if (f >= FCategory) { int c = f - FCategory; return c < (int)engine::categories().size() ? engine::categories()[c].c_str() : L""; }
    return f >= 0 && f < 8 ? n[f] : L"";
}
const wchar_t* filterGlyph(int f) {
    static const wchar_t* g[] = {L"", L"", L"", L"", L"", L"", L"", L""};
    static const wchar_t* cg[] = {L"", L"", L"", L"", L"", L"", L"", L""};
    if (f >= FCategory) { int c = f - FCategory; return c < 8 ? cg[c] : L""; }
    return f >= 0 && f < 8 ? g[f] : L"";
}
const wchar_t* categoryGlyph(const std::wstring& c) {
    auto& cs = engine::categories();
    for (size_t i = 0; i < cs.size(); ++i) if (cs[i] == c) return filterGlyph(FCategory + (int)i);
    return L"";
}
bool viaTor(const View& v) {
    if (v.tor == 1) return true;
    if (v.tor == 2) return false;
    if (v.kind == engine::KindTorrent) return E.config().btViaTor;
    net::Url u;
    return (net::parseUrl(v.url, u) && u.onion()) || E.config().torAll;
}
bool isActive(const View& v) { return v.status == engine::Starting || v.status == engine::Downloading || v.status == engine::Metadata || v.status == engine::Checking; }
bool matches(const View& v, int f) {
    switch (f) {
    case FAll: return true;
    case FActive: return isActive(v) || v.status == engine::Seeding;
    case FQueued: return v.status == engine::Queued;
    case FPaused: return v.status == engine::Paused;
    case FCompleted: return v.status == engine::Completed || v.status == engine::Seeding;
    case FFailed: return v.status == engine::Failed;
    case FTorrents: return v.kind == engine::KindTorrent;
    case FTor: return viaTor(v);
    }
    if (f >= FCategory) { int c = f - FCategory; return c < (int)engine::categories().size() && v.category == engine::categories()[c]; }
    return false;
}
int countFor(int f) { int n = 0; for (auto& v : views) if (matches(v, f)) ++n; return n; }

double progressOf(const View& v) {
    if (v.status == engine::Completed && v.kind == engine::KindHttp) return 1.0;
    if (v.size > 0) return std::min(1.0, (double)v.done / (double)v.size);
    if (v.size == 0 && v.kind == engine::KindTorrent && !v.files.empty()) return 1.0;
    return 0;
}
int64_t etaOf(const View& v) {
    if (!isActive(v) || v.speed < 1 || v.size <= 0) return -1;
    return (int64_t)((double)(v.size - v.done) / v.speed);
}
std::wstring statusText(const View& v) {
    std::wstring s = engine::statusName(v.status);
    if (v.status == engine::Downloading && v.kind == engine::KindHttp && v.conns > 1) s += L" (" + std::to_wstring(v.conns) + L" connections)";
    if ((v.status == engine::Downloading || v.status == engine::Seeding) && v.kind == engine::KindTorrent) s += L" (" + std::to_wstring(v.conns) + (v.conns == 1 ? L" peer)" : L" peers)");
    return s;
}

// Columns of the download list.
enum ColId { ColName, ColSize, ColProgress, ColSpeed, ColEta, ColStatus, ColAdded, ColCount };
struct Column { const wchar_t* title; int def; UINT align; };
const Column Columns[ColCount] = {
    {L"Name", 300, DT_LEFT}, {L"Size", 90, DT_RIGHT}, {L"Progress", 150, DT_LEFT}, {L"Speed", 95, DT_RIGHT},
    {L"Time left", 85, DT_RIGHT}, {L"Status", 170, DT_LEFT}, {L"Added", 125, DT_LEFT},
};
int colWidth(int c) { return c < (int)S.ui.columns.size() ? S.ui.columns[c] : Columns[c].def; }
void setColWidth(int c, int w) {
    while ((int)S.ui.columns.size() < ColCount) S.ui.columns.push_back(Columns[S.ui.columns.size()].def);
    S.ui.columns[c] = w;
}

int compareViews(const View& a, const View& b, int col) {
    auto cmp = [](auto x, auto y) { return x < y ? -1 : y < x ? 1 : 0; };
    switch (col) {
    case ColName: return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) - 2;
    case ColSize: return cmp(a.size, b.size);
    case ColProgress: return cmp(progressOf(a), progressOf(b));
    case ColSpeed: return cmp(a.speed, b.speed);
    case ColEta: { int64_t x = etaOf(a), y = etaOf(b); if (x < 0) x = INT64_MAX; if (y < 0) y = INT64_MAX; return cmp(x, y); }
    case ColStatus: return cmp(a.status, b.status);
    case ColAdded: return cmp(a.added, b.added);
    }
    return 0;
}
bool searchHit(const View& v) {
    if (searchText.empty()) return true;
    std::wstring s = lowerW(searchText);
    return lowerW(v.name).find(s) != std::wstring::npos || lowerW(widenLoose(v.url)).find(s) != std::wstring::npos;
}
void rebuildRows() {
    rows.clear();
    for (int i = 0; i < (int)views.size(); ++i) if (matches(views[i], filter) && searchHit(views[i])) rows.push_back(i);
    if (S.ui.sortColumn >= 0) {
        int col = S.ui.sortColumn; bool asc = S.ui.sortAscending;
        std::stable_sort(rows.begin(), rows.end(), [&](int x, int y) { int c = compareViews(views[x], views[y], col); return asc ? c < 0 : c > 0; });
    }
    std::set<uint32_t> keep;
    for (int r : rows) if (selected.count(views[r].id)) keep.insert(views[r].id);
    selected.swap(keep);
    bool focusVisible = false;
    for (int r : rows) if (views[r].id == focusId) focusVisible = true;
    if (!focusVisible) focusId = selected.empty() ? 0 : *selected.begin();
}
const View* viewById(uint32_t id) { for (auto& v : views) if (v.id == id) return &v; return nullptr; }
int rowOf(uint32_t id) { for (size_t i = 0; i < rows.size(); ++i) if (views[rows[i]].id == id) return (int)i; return -1; }
std::vector<uint32_t> selection() {
    std::vector<uint32_t> v;
    for (int r : rows) if (selected.count(views[r].id)) v.push_back(views[r].id);
    return v;
}

// ---------------------------------------------------------------- chrome (command bar, banner, sidebar, status)

enum HitKind { KButton, KSide, KBannerButton };
struct Hit { RECT r; int cmd; int kind; std::wstring glyph, label, tip, count; bool on = false, dropdown = false, enabled = true; };
std::vector<Hit> hits;
std::vector<int> separators;
int hoverHit = -1, pressHit = -1, toolCount = 0;
RECT rcCmd, rcBanner, rcSide, rcSearchBox, rcList, rcSplit, rcDetails, rcStatus;
std::string bannerUrl;                   // link offered from the clipboard
std::string lastClipboard;
bool ignoreClipboard = false;

void setTool(int id, const RECT& r, const std::wstring& tip) {
    TOOLINFOW ti{sizeof ti};
    ti.uFlags = TTF_SUBCLASS; ti.hwnd = gMain; ti.uId = id; ti.rect = r; ti.lpszText = const_cast<wchar_t*>(tip.c_str());
    SendMessageW(gTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}
std::wstring limitLabel() {
    int64_t l = E.config().speedLimit;
    return l <= 0 ? std::wstring(L"Unlimited") : fmtSize(l) + L"/s";
}
int detailsHeightPx(int avail) {
    int h = px(S.ui.detailsHeight);
    return std::max(px(110), std::min(h, avail - px(120)));
}
void layoutChildren();
void buildChrome() {
    RECT c; GetClientRect(gMain, &c);
    int w = c.right;
    rcCmd = {0, 0, w, px(48)};
    rcBanner = {0, rcCmd.bottom, w, rcCmd.bottom + (bannerUrl.empty() ? 0 : px(42))};
    int top = rcBanner.bottom;
    rcStatus = {0, std::max<LONG>(top, c.bottom - px(28)), w, c.bottom};
    int sideW = S.ui.sidebar ? px(216) : 0;
    rcSide = {0, top, sideW, rcStatus.top};
    int avail = rcStatus.top - top;
    if (S.ui.details && avail > px(260)) {
        int dh = detailsHeightPx(avail);
        rcDetails = {sideW, rcStatus.top - dh, w, rcStatus.top};
        rcSplit = {sideW, rcDetails.top - px(5), w, rcDetails.top};
        rcList = {sideW, top, w, rcSplit.top};
    } else {
        rcDetails = rcSplit = {sideW, rcStatus.top, w, rcStatus.top};
        rcList = {sideW, top, w, rcStatus.top};
    }
    for (int i = 0; i < toolCount; ++i) { TOOLINFOW ti{sizeof ti}; ti.hwnd = gMain; ti.uId = i + 1; SendMessageW(gTip, TTM_DELTOOLW, 0, (LPARAM)&ti); }
    hits.clear(); separators.clear();
    HDC dc = GetDC(gMain);
    bool sel = !selected.empty();

    struct B { int cmd; const wchar_t* glyph; std::wstring label, tip; bool dropdown, enabled; };
    std::vector<B> left{
        {CmdAddUrl, L"", L"Add URL", L"Download a link (Ctrl+N)", false, true},
        {CmdAddTorrent, L"", L"Add torrent", L"Open a .torrent file (Ctrl+O)", false, true},
        {0, nullptr, L"", L"", false, false},
        {CmdResume, L"", L"Resume", L"Start or resume the selected downloads", false, sel},
        {CmdPause, L"", L"Pause", L"Pause the selected downloads", false, sel},
        {CmdDelete, L"", L"Delete", L"Remove the selected downloads (Del)", false, sel},
        {0, nullptr, L"", L"", false, false},
        {CmdLimit, L"", limitLabel(), L"Download speed limit", true, true},
    };
    std::vector<B> right{
        {CmdSettings, L"", L"Settings", L"Settings (Ctrl+,)", false, true},
        {CmdTheme, P.dark ? L"" : L"", L"Theme", L"Light, dark or AMOLED black", true, true},
        {CmdMore, L"", L"", L"More options", false, true},
    };
    auto widthOf = [&](const B& b, bool compact) {
        int wv = px(12) + px(18) + px(12);
        if (!compact && !b.label.empty()) wv += px(8) + textWidth(dc, uiFont, b.label);
        if (b.dropdown) wv += px(16);
        return wv;
    };
    int total = 0;
    for (auto& b : left) total += b.cmd ? widthOf(b, false) + px(4) : px(13);
    for (auto& b : right) total += widthOf(b, false) + px(4);
    bool compact = total > w - px(24);
    int bh = px(34), by = (rcCmd.bottom - bh) / 2, x = px(8);
    for (auto& b : left) {
        if (!b.cmd) { separators.push_back(x + px(4)); x += px(13); continue; }
        int bw = widthOf(b, compact);
        Hit h; h.r = {x, by, x + bw, by + bh}; h.cmd = b.cmd; h.kind = KButton; h.glyph = b.glyph; h.label = compact ? L"" : b.label; h.tip = b.tip; h.dropdown = b.dropdown; h.enabled = b.enabled;
        hits.push_back(h); x += bw + px(4);
    }
    int rx = w - px(8);
    for (auto it = right.rbegin(); it != right.rend(); ++it) {
        int bw = widthOf(*it, compact);
        Hit h; h.r = {rx - bw, by, rx, by + bh}; h.cmd = it->cmd; h.kind = KButton; h.glyph = it->glyph; h.label = compact ? L"" : it->label; h.tip = it->tip; h.dropdown = it->dropdown;
        hits.push_back(h); rx -= bw + px(4);
    }

    if (!bannerUrl.empty()) {
        int bx = w - px(12);
        auto bb = [&](int cmd, const std::wstring& label, const wchar_t* glyph) {
            int bw = label.empty() ? px(32) : textWidth(dc, uiBold, label) + px(28);
            Hit h; h.r = {bx - bw, rcBanner.top + px(5), bx, rcBanner.bottom - px(5)}; h.cmd = cmd; h.kind = KBannerButton; h.label = label; h.glyph = glyph ? glyph : L"";
            h.tip = cmd == CmdBannerAdd ? L"Download this link" : L"Dismiss";
            hits.push_back(h); bx -= bw + px(6);
        };
        bb(CmdBannerClose, L"", L"");
        bb(CmdBannerAdd, L"Download", nullptr);
    }

    if (S.ui.sidebar) {
        rcSearchBox = {px(12), rcSide.top + px(10), rcSide.right - px(12), rcSide.top + px(42)};
        int y = rcSearchBox.bottom + px(10);
        auto side = [&](int f) {
            Hit h; h.r = {px(8), y, rcSide.right - px(8), y + px(34)}; h.cmd = CmdFilterBase + f; h.kind = KSide;
            h.glyph = filterGlyph(f); h.label = filterName(f); h.on = filter == f;
            int n = countFor(f); h.count = n ? fmtNum(n) : L"";
            hits.push_back(h); y += px(36);
        };
        for (int f = FAll; f <= FTor; ++f) {
            if ((f == FTorrents || f == FTor || f == FFailed) && !countFor(f) && filter != f) continue;
            side(f);
        }
        bool anyCat = false;
        for (int i = 0; i < (int)engine::categories().size(); ++i) if (countFor(FCategory + i) || filter == FCategory + i) anyCat = true;
        if (anyCat) {
            y += px(26); // "Categories" heading
            separators.push_back(-(y - px(13)));
            for (int i = 0; i < (int)engine::categories().size(); ++i) {
                int f = FCategory + i;
                if (!countFor(f) && filter != f) continue;
                if (y + px(34) > rcSide.bottom) break;
                side(f);
            }
        }
    } else rcSearchBox = {0, 0, 0, 0};
    ReleaseDC(gMain, dc);
    toolCount = 0;
    for (auto& h : hits) if (!h.tip.empty()) setTool(++toolCount, h.r, h.tip);
    layoutChildren();
}

void paintChrome(HDC target) {
    RECT c; GetClientRect(gMain, &c);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, c.right, c.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    fill(dc, c, P.chrome);
    { RECT l{0, rcCmd.bottom - 1, c.right, rcCmd.bottom}; fill(dc, l, P.border); }
    for (int s : separators) {
        if (s >= 0) { RECT r{s, rcCmd.top + px(14), s + 1, rcCmd.bottom - px(14)}; fill(dc, r, P.border); }
        else {
            int y = -s;
            RECT t{px(20), y - px(10), rcSide.right - px(12), y + px(12)};
            drawText(dc, L"CATEGORIES", t, P.muted, uiSmall);
        }
    }
    if (!bannerUrl.empty()) {
        fill(dc, rcBanner, P.accentSoft);
        RECT l{0, rcBanner.bottom - 1, c.right, rcBanner.bottom}; fill(dc, l, P.border);
        RECT g{px(12), rcBanner.top, px(36), rcBanner.bottom}; drawGlyph(dc, L"", g, P.accent, iconSmall);
        int right = c.right - px(12);
        for (auto& h : hits) if (h.kind == KBannerButton) right = std::min<int>(right, h.r.left);
        RECT t{px(40), rcBanner.top, right - px(12), rcBanner.bottom};
        std::wstring label = L"Download the link you copied?  ";
        int lw = textWidth(dc, uiBold, label);
        drawText(dc, label, t, P.text, uiBold);
        t.left += lw;
        drawText(dc, compactPath(dc, uiFont, widenLoose(bannerUrl), t.right - t.left), t, P.muted, uiFont);
    }
    if (S.ui.sidebar) {
        RECT l{rcSide.right - 1, rcSide.top, rcSide.right, rcSide.bottom}; fill(dc, l, P.border);
        bool focused = GetFocus() == gSearch;
        roundRect(dc, rcSearchBox, P.surface, focused ? P.accent : P.border, px(8));
        if (focused) { RECT u{rcSearchBox.left + px(6), rcSearchBox.bottom - px(2), rcSearchBox.right - px(6), rcSearchBox.bottom - px(1)}; fill(dc, u, P.accent); }
        RECT g{rcSearchBox.left + px(8), rcSearchBox.top, rcSearchBox.left + px(30), rcSearchBox.bottom}; drawGlyph(dc, L"", g, P.muted, iconSmall);
    }
    for (size_t i = 0; i < hits.size(); ++i) {
        const Hit& h = hits[i];
        bool hot = (int)i == hoverHit && h.enabled, pressed = hot && (int)i == pressHit;
        if (h.kind == KButton) {
            if (hot || pressed) roundRect(dc, h.r, pressed ? P.pressed : P.hover, pressed ? P.pressed : P.hover, px(8));
            COLORREF fg = h.enabled ? P.text : P.border;
            bool accent = h.enabled && (h.cmd == CmdAddUrl || h.cmd == CmdAddTorrent);
            RECT r = h.r;
            if (h.label.empty()) { RECT g = r; if (h.dropdown) g.right -= px(16); drawGlyph(dc, h.glyph, g, accent ? P.accent : fg); }
            else {
                int x = r.left + px(12);
                RECT g{x, r.top, x + px(18), r.bottom}; drawGlyph(dc, h.glyph, g, accent ? P.accent : fg);
                x += px(26);
                RECT tr{x, r.top, r.right - (h.dropdown ? px(16) : px(4)), r.bottom};
                drawText(dc, h.label, tr, fg, uiFont);
            }
            if (h.dropdown) { RECT d{r.right - px(22), r.top, r.right - px(6), r.bottom}; drawGlyph(dc, L"", d, P.muted, iconSmall); }
        } else if (h.kind == KBannerButton) {
            bool primary = h.cmd == CmdBannerAdd;
            COLORREF face = primary ? (pressed ? blend(P.accent, P.text, 20) : hot ? blend(P.accent, P.bg, 12) : P.accent) : (pressed ? P.pressed : hot ? P.hover : P.accentSoft);
            roundRect(dc, h.r, face, face, px(8));
            if (h.label.empty()) drawGlyph(dc, h.glyph, h.r, P.text, iconSmall);
            else drawText(dc, h.label, h.r, primary ? P.accentText : P.text, uiBold, DT_CENTER);
        } else if (h.kind == KSide) {
            if (h.on) roundRect(dc, h.r, P.accentSoft, P.accentSoft, px(8));
            else if (hot) roundRect(dc, h.r, P.hover, P.hover, px(8));
            if (h.on) { RECT bar{h.r.left, h.r.top + px(9), h.r.left + px(3), h.r.bottom - px(9)}; roundRect(dc, bar, P.accent, P.accent, px(3)); }
            RECT g{h.r.left + px(10), h.r.top, h.r.left + px(32), h.r.bottom}; drawGlyph(dc, h.glyph, g, h.on ? P.accent : P.muted, iconSmall);
            int cw = h.count.empty() ? 0 : textWidth(dc, uiSmall, h.count) + px(14);
            RECT t{h.r.left + px(40), h.r.top, h.r.right - px(8) - cw, h.r.bottom}; drawText(dc, h.label, t, P.text, h.on ? uiBold : uiFont);
            if (cw) {
                RECT pill{h.r.right - px(8) - cw, h.r.top + px(8), h.r.right - px(8), h.r.bottom - px(8)};
                roundRect(dc, pill, h.on ? P.surface : P.track, h.on ? P.surface : P.track, px(10));
                drawText(dc, h.count, pill, P.muted, uiSmall, DT_CENTER);
            }
        }
    }
    // Status bar: summary on the left, network state on the right.
    { RECT l{0, rcStatus.top, c.right, rcStatus.top + 1}; fill(dc, l, P.border); }
    {
        RECT r = rcStatus; r.left += px(12); r.right -= px(12);
        std::vector<std::pair<std::wstring, COLORREF>> segs;
        int torState; std::wstring torMsg;
        { std::lock_guard<std::mutex> lk(E.tor.m); torState = E.tor.state; torMsg = E.tor.message; }
        bool anyTor = false, anyTorrent = false;
        for (auto& v : views) {
            if (viaTor(v) && (isActive(v) || v.status == engine::Queued || v.status == engine::Failed)) anyTor = true;
            if (v.kind == engine::KindTorrent && (isActive(v) || v.status == engine::Seeding)) anyTorrent = true;
        }
        if (torState == 2) segs.push_back({torMsg.empty() ? std::wstring(L"Tor is ready") : torMsg, P.success});
        else if (torState == 1) segs.push_back({torMsg.empty() ? std::wstring(L"Tor is starting") : torMsg, P.warning});
        else if (torState == 3 && anyTor) segs.push_back({torMsg, P.danger});
        if (anyTorrent) {
            int n = engine::btServices().dhtNodes();
            if (n >= 0) segs.push_back({L"DHT " + fmtNum(n) + L" nodes", P.muted});
        }
        segs.push_back({L"↓ " + (totalDown >= 1 ? fmtSpeed(totalDown) : std::wstring(L"0 B/s")), totalDown >= 1 ? P.accent : P.muted});
        segs.push_back({L"↑ " + (totalUp >= 1 ? fmtSpeed(totalUp) : std::wstring(L"0 B/s")), P.muted});
        std::wstring summary = fmtNum(views.size()) + (views.size() == 1 ? L" download" : L" downloads");
        if (activeCount) summary += L"  ·  " + fmtNum(activeCount) + L" active";
        if (!selected.empty()) summary += L"  ·  " + fmtNum(selected.size()) + L" selected";
        int sw = textWidth(dc, uiFont, summary);
        { RECT sr{r.left, r.top, r.left + sw, r.bottom}; drawText(dc, summary, sr, P.muted, uiFont); }
        int rx2 = r.right;
        for (size_t i = segs.size(); i-- > 0;) {
            int wv = textWidth(dc, uiFont, segs[i].first);
            if (rx2 - wv < r.left + sw + px(20)) break;
            RECT sr{rx2 - wv, r.top, rx2, r.bottom};
            drawText(dc, segs[i].first, sr, segs[i].second, uiFont);
            rx2 -= wv + px(20);
        }
    }
    BitBlt(target, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
}
int hitAt(POINT p) { for (size_t i = 0; i < hits.size(); ++i) if (PtInRect(&hits[i].r, p)) return (int)i; return -1; }

// ---------------------------------------------------------------- download list

int listTop = 0, listHover = -1, headerHover = -1;
int dragCol = -1, dragStartX = 0, dragStartW = 0, headerPress = -1;
int headerH() { return px(32); }
int rowH() { return px(34); }
struct ColPos { int col, x, w; };
std::vector<ColPos> colLayout(int width) {
    std::vector<ColPos> v;
    int others = 0;
    for (int c = 1; c < ColCount; ++c) if (colWidth(c) > 0) others += px(colWidth(c));
    int nameW = std::max(px(180), width - others);
    int x = 0;
    v.push_back({ColName, x, nameW}); x += nameW;
    for (int c = 1; c < ColCount; ++c) if (colWidth(c) > 0) { v.push_back({c, x, px(colWidth(c))}); x += px(colWidth(c)); }
    return v;
}
int visibleRows() { RECT r; GetClientRect(gList, &r); return std::max(1, (int)(r.bottom - headerH()) / rowH()); }
void clampListTop() {
    int maxTop = std::max(0, (int)rows.size() - visibleRows());
    listTop = std::max(0, std::min(listTop, maxTop));
}
void updateListScroll() {
    clampListTop();
    SCROLLINFO si{sizeof si};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0; si.nMax = std::max(0, (int)rows.size() - 1); si.nPage = (UINT)visibleRows(); si.nPos = listTop;
    if ((int)rows.size() <= visibleRows()) { si.nMax = 0; si.nPage = 2; si.fMask &= ~SIF_DISABLENOSCROLL; }
    SetScrollInfo(gList, SB_VERT, &si, TRUE);
}
void ensureRowVisible(int r) {
    if (r < 0) return;
    if (r < listTop) listTop = r;
    else if (r >= listTop + visibleRows()) listTop = r - visibleRows() + 1;
    updateListScroll();
}
COLORREF statusColor(const View& v) {
    switch (v.status) {
    case engine::Downloading: case engine::Starting: case engine::Metadata: case engine::Checking: return P.accent;
    case engine::Completed: case engine::Seeding: return P.success;
    case engine::Failed: return P.danger;
    }
    return P.muted;
}

void paintProgress(HDC dc, RECT r, double f, COLORREF c) {
    roundRect(dc, r, P.track, P.track, r.bottom - r.top);
    if (f <= 0) return;
    RECT b = r; b.right = r.left + std::max<LONG>(r.bottom - r.top, (LONG)((r.right - r.left) * std::min(1.0, f)));
    roundRect(dc, b, c, c, r.bottom - r.top);
}

void paintList(HDC target) {
    RECT c; GetClientRect(gList, &c);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, std::max<LONG>(1, c.right), std::max<LONG>(1, c.bottom));
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    fill(dc, c, P.bg);
    auto cols = colLayout(c.right);
    // Header.
    RECT hr{0, 0, c.right, headerH()};
    fill(dc, hr, P.bg);
    line(dc, 0, hr.bottom - 1, c.right, hr.bottom, P.border);
    for (size_t i = 0; i < cols.size(); ++i) {
        auto& cp = cols[i];
        RECT r{cp.x, 0, cp.x + cp.w, hr.bottom - 1};
        if ((int)i == headerHover && dragCol < 0) fill(dc, r, P.hover);
        RECT t{r.left + px(12), r.top, r.right - px(8), r.bottom};
        bool sorted = S.ui.sortColumn == cp.col;
        if (sorted) {
            RECT a = t; if (Columns[cp.col].align == DT_RIGHT) { a.right = a.left + px(12); t.left += px(14); } else { a.left = a.right - px(12); t.right -= px(14); }
            drawGlyph(dc, S.ui.sortAscending ? L"" : L"", a, P.muted, iconSmall);
        }
        drawText(dc, Columns[cp.col].title, t, sorted ? P.text : P.muted, sorted ? uiBold : uiFont, Columns[cp.col].align);
        if (i + 1 < cols.size()) line(dc, r.right - 1, r.top + px(9), r.right, r.bottom - px(9), P.border);
    }
    // Rows.
    bool focused = GetFocus() == gList;
    int rh = rowH();
    for (int i = listTop; i < (int)rows.size(); ++i) {
        int y = headerH() + (i - listTop) * rh;
        if (y >= c.bottom) break;
        const View& v = views[rows[i]];
        RECT rr{0, y, c.right, y + rh};
        bool sel = selected.count(v.id) > 0;
        if (sel) fill(dc, rr, focused ? P.selection : blend(P.selection, P.bg, 45));
        else if (i == listHover) fill(dc, rr, P.hover);
        if (sel && v.id == focusId && focused) { RECT bar{0, y + px(8), px(3), y + rh - px(8)}; fill(dc, bar, P.accent); }
        double prog = progressOf(v);
        for (auto& cp : cols) {
            RECT r{cp.x + px(12), y, cp.x + cp.w - px(8), y + rh};
            switch (cp.col) {
            case ColName: {
                RECT g{r.left - px(2), y, r.left + px(18), y + rh};
                drawGlyph(dc, v.kind == engine::KindTorrent ? L"" : categoryGlyph(v.category), g, statusColor(v), iconSmall);
                RECT t{r.left + px(26), y, r.right, y + rh};
                if (viaTor(v)) {
                    RECT lk{t.right - px(16), y, t.right, y + rh};
                    drawGlyph(dc, L"", lk, P.muted, iconSmall);
                    t.right -= px(20);
                }
                drawText(dc, v.name, t, P.text, uiFont);
                break;
            }
            case ColSize: {
                std::wstring s = v.size >= 0 ? fmtSize(v.size) : (v.done > 0 ? fmtSize(v.done) : std::wstring(L"—"));
                drawText(dc, s, r, P.text, uiFont, DT_RIGHT);
                break;
            }
            case ColProgress: {
                std::wstring pct = v.size > 0 || v.status == engine::Completed ? std::to_wstring((int)(prog * 100)) + L"%" : (v.done > 0 ? fmtSize(v.done) : L"");
                int pw = px(40);
                RECT bar{r.left, y + rh / 2 - px(3), std::max<LONG>(r.left + px(10), r.right - pw - px(6)), y + rh / 2 + px(3)};
                COLORREF col = v.status == engine::Completed || v.status == engine::Seeding ? P.success : v.status == engine::Failed ? P.danger : v.status == engine::Paused || v.status == engine::Queued ? P.muted : P.accent;
                paintProgress(dc, bar, prog, col);
                RECT t{r.right - pw, y, r.right, y + rh};
                drawText(dc, pct, t, P.muted, uiSmall, DT_RIGHT);
                break;
            }
            case ColSpeed: {
                std::wstring s = fmtSpeed(v.speed);
                if (s.empty() && v.upSpeed >= 1) s = L"↑ " + fmtSpeed(v.upSpeed);
                drawText(dc, s, r, P.text, uiFont, DT_RIGHT);
                break;
            }
            case ColEta: drawText(dc, fmtDuration(etaOf(v)), r, P.text, uiFont, DT_RIGHT); break;
            case ColStatus: {
                std::wstring s = statusText(v);
                COLORREF col = statusColor(v);
                if (v.status == engine::Failed && !v.error.empty()) s = v.error;
                else if (!v.error.empty() && isActive(v)) { s = v.error; col = P.warning; }
                else if (v.status == engine::Completed && !v.checkResult.empty() && v.checkResult.find(L"MISMATCH") != std::wstring::npos) { s = v.checkResult; col = P.danger; }
                drawText(dc, s, r, col, uiFont);
                break;
            }
            case ColAdded: drawText(dc, fmtTime(v.added), r, P.muted, uiFont); break;
            }
        }
    }
    if (rows.empty()) {
        RECT t{0, headerH() + px(40), c.right, headerH() + px(80)};
        bool none = views.empty();
        drawText(dc, none ? L"No downloads yet" : L"Nothing here", t, P.text, uiLarge, DT_CENTER);
        RECT s{0, t.bottom, c.right, t.bottom + px(28)};
        drawText(dc, none ? L"Add a URL with Ctrl+N, paste a magnet link, or drop a .torrent file here." : L"No downloads match this view.", s, P.muted, uiFont, DT_CENTER);
    }
    BitBlt(target, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
}

// ---------------------------------------------------------------- details pane

int detailTab = 0, detailScroll = 0, detailHover = -1;
struct DHit { RECT r; int kind; int index; }; // kind 0 tab, 1 file checkbox, 2 all-files checkbox
std::vector<DHit> dhits;

std::vector<uint8_t> haveBits(const std::string& hx, int np) { return engine::haveFromHex(hx, np); }

void paintMap(HDC dc, RECT r, const View& v) {
    roundRect(dc, r, P.track, P.track, px(4));
    int W = r.right - r.left;
    if (W <= 2) return;
    RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    W = in.right - in.left;
    if (v.kind == engine::KindHttp) {
        if (v.status == engine::Completed) { roundRect(dc, r, P.success, P.success, px(4)); return; }
        if (v.size <= 0) return;
        for (auto& s : v.segs) {
            int a = in.left + (int)((double)s.start / (double)v.size * W), b = in.left + (int)((double)s.pos / (double)v.size * W);
            if (b > a) { RECT f{a, in.top, b, in.bottom}; fill(dc, f, P.accent); }
            if (s.pos < s.end && s.active) { RECT k{std::max<LONG>(in.left, b - 1), in.top, std::min<LONG>(in.right, b + px(2)), in.bottom}; fill(dc, k, P.text); }
        }
        return;
    }
    int np = v.pieceCount;
    auto have = haveBits(v.have, np);
    if (np <= 0 || have.empty()) return;
    COLORREF prev = CLR_INVALID; int runStart = in.left;
    for (int x = 0; x <= W; ++x) {
        COLORREF col = CLR_INVALID;
        if (x < W) {
            int a = (int)((int64_t)x * np / W), b = std::max(a + 1, (int)((int64_t)(x + 1) * np / W));
            int n = 0; for (int i = a; i < b && i < np; ++i) n += have[i];
            int pct = n * 100 / std::max(1, b - a);
            col = pct == 0 ? P.track : blend(P.track, v.status == engine::Seeding || v.status == engine::Completed ? P.success : P.accent, 35 + pct * 65 / 100);
        }
        if (col != prev || x == W) {
            if (prev != CLR_INVALID && prev != P.track) { RECT f{runStart, in.top, in.left + x, in.bottom}; fill(dc, f, prev); }
            prev = col; runStart = in.left + x;
        }
    }
}

void paintDetails(HDC target) {
    RECT c; GetClientRect(gDetails, &c);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, std::max<LONG>(1, c.right), std::max<LONG>(1, c.bottom));
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    fill(dc, c, P.bg);
    line(dc, 0, 0, c.right, 1, P.border);
    dhits.clear();
    const View* v = viewById(focusId);
    if (!v) {
        RECT t{0, 0, c.right, c.bottom};
        drawText(dc, views.empty() ? L"" : L"Select a download to see its details.", t, P.muted, uiFont, DT_CENTER);
    } else {
        bool torrent = v->kind == engine::KindTorrent;
        if (!torrent) detailTab = 0;
        // Tabs.
        std::vector<std::wstring> tabs{L"General"};
        if (torrent) { tabs.push_back(L"Files (" + fmtNum(v->files.size()) + L")"); tabs.push_back(L"Peers (" + fmtNum(v->peerList.size()) + L")"); }
        int tx = px(12);
        for (size_t i = 0; i < tabs.size(); ++i) {
            int tw = textWidth(dc, uiBold, tabs[i]) + px(24);
            RECT r{tx, px(4), tx + tw, px(38)};
            bool on = (int)i == detailTab;
            if (!on && detailHover == (int)dhits.size()) roundRect(dc, RECT{r.left, r.top + px(3), r.right, r.bottom - px(3)}, P.hover, P.hover, px(6));
            drawText(dc, tabs[i], r, on ? P.text : P.muted, on ? uiBold : uiFont, DT_CENTER);
            if (on) { RECT u{r.left + px(10), r.bottom - px(3), r.right - px(10), r.bottom}; roundRect(dc, u, P.accent, P.accent, px(3)); }
            dhits.push_back({r, 0, (int)i});
            tx += tw + px(4);
        }
        line(dc, 0, px(39), c.right, px(40), P.border);
        RECT area{px(16), px(48), c.right - px(16), c.bottom - px(8)};
        if (detailTab == 0) {
            int y = area.top;
            RECT g{area.left, y, area.left + px(24), y + px(28)};
            drawGlyph(dc, torrent ? L"" : categoryGlyph(v->category), g, statusColor(*v));
            RECT nt{area.left + px(32), y, area.right, y + px(28)};
            drawText(dc, v->name, nt, P.text, uiLarge);
            y += px(30);
            RECT pt{area.left + px(32), y, area.right, y + px(20)};
            std::wstring where = torrent && v->files.size() > 1 ? joinPath(v->folder, v->name) : v->path();
            drawText(dc, compactPath(dc, uiFont, where, pt.right - pt.left), pt, P.muted, uiFont);
            y += px(30);
            int colW = (area.right - area.left) / 2, labelW = px(104), lh = px(22);
            auto field = [&](int col, int row, const std::wstring& label, const std::wstring& value, COLORREF vc) {
                int x = area.left + col * colW, yy = y + row * lh;
                if (yy + lh > area.bottom - px(26)) return;
                RECT l{x, yy, x + labelW, yy + lh}; drawText(dc, label, l, P.muted, uiFont);
                RECT r{x + labelW, yy, x + colW - px(16), yy + lh}; drawText(dc, value, r, vc, uiFont);
            };
            double prog = progressOf(*v);
            std::wstring st = statusText(*v);
            field(0, 0, L"Status", st, statusColor(*v));
            std::wstring sz = v->size >= 0 ? fmtSize(v->done) + L" of " + fmtSize(v->size) + L"  (" + std::to_wstring((int)(prog * 100)) + L"%)" : fmtSize(v->done) + L" (size unknown)";
            field(0, 1, L"Downloaded", sz, P.text);
            std::wstring sp = fmtSpeed(v->speed);
            if (torrent) sp = L"↓ " + (sp.empty() ? L"0 B/s" : sp) + L"   ↑ " + (v->upSpeed >= 1 ? fmtSpeed(v->upSpeed) : L"0 B/s");
            field(0, 2, L"Speed", sp.empty() ? L"—" : sp, P.text);
            int64_t eta = etaOf(*v);
            field(0, 3, L"Time left", eta >= 0 ? fmtDuration(eta) : L"—", P.text);
            if (torrent) field(0, 4, L"Peers", fmtNum(v->conns) + L" connected, " + fmtNum(v->seeds) + L" seeds, " + fmtNum(v->peers) + L" known", P.text);
            else field(0, 4, L"Connections", fmtNum(v->conns) + (v->resumable ? L"  (resumable)" : v->size >= 0 || v->done > 0 ? L"  (no resume support)" : L""), P.text);
            field(1, 0, L"Added", fmtTime(v->added), P.text);
            field(1, 1, L"Finished", v->finished ? fmtTime(v->finished) : L"—", P.text);
            if (torrent) {
                wchar_t ratio[32]; swprintf(ratio, 32, L"%.2f", v->size > 0 ? (double)v->uploaded / (double)v->size : 0.0);
                field(1, 2, L"Uploaded", fmtSize(v->uploaded) + L"  (ratio " + ratio + L")", P.text);
                field(1, 3, L"Info hash", widen(v->infoHash), P.text);
                field(1, 4, L"Network", viaTor(*v) ? L"Through Tor (HTTP trackers only)" : L"Direct", P.text);
            } else {
                std::wstring u = widenLoose(v->finalUrl.empty() ? v->url : v->finalUrl);
                field(1, 2, L"Address", u, P.text);
                field(1, 3, L"Network", viaTor(*v) ? L"Through Tor" : L"Direct", viaTor(*v) ? P.accent : P.text);
                field(1, 4, L"Checksum", v->checkResult.empty() ? L"—" : v->checkResult, v->checkResult.find(L"MISMATCH") != std::wstring::npos ? P.danger : v->checkResult.find(L"verified") != std::wstring::npos ? P.success : P.text);
            }
            int ey = y + 5 * lh + px(4);
            if (!v->error.empty() && ey + lh <= area.bottom - px(26)) {
                RECT er{area.left, ey, area.right, ey + lh};
                drawText(dc, v->error, er, v->status == engine::Failed ? P.danger : P.warning, uiFont);
            }
            RECT map{area.left, area.bottom - px(14), area.right, area.bottom};
            if (map.top > y + px(40)) paintMap(dc, map, *v);
        } else if (detailTab == 1) {
            int rh = px(28);
            RECT head{area.left, area.top - px(4), area.right, area.top + rh - px(4)};
            int sizeW = px(90), progW = px(150);
            bool all = !v->files.empty(), none = true;
            for (auto& f : v->files) { if (f.wanted) none = false; else all = false; }
            RECT cb{head.left, head.top, head.left + px(24), head.bottom};
            drawGlyph(dc, all ? L"" : none ? L"" : L"", cb, all || !none ? P.accent : P.muted, iconSmall);
            dhits.push_back({cb, 2, -1});
            RECT nh{head.left + px(32), head.top, head.right - sizeW - progW - px(16), head.bottom}; drawText(dc, L"Name", nh, P.muted, uiFont);
            RECT sh{head.right - sizeW - progW - px(8), head.top, head.right - progW - px(8), head.bottom}; drawText(dc, L"Size", sh, P.muted, uiFont, DT_RIGHT);
            RECT ph{head.right - progW, head.top, head.right, head.bottom}; drawText(dc, L"Progress", ph, P.muted, uiFont);
            line(dc, area.left, head.bottom, area.right, head.bottom + 1, P.border);
            int visible = std::max(1, (int)(area.bottom - head.bottom - px(2)) / rh);
            detailScroll = std::max(0, std::min(detailScroll, (int)v->files.size() - visible));
            std::wstring prefix = v->files.size() > 1 ? v->name + L"\\" : L"";
            for (int i = detailScroll; i < (int)v->files.size() && i < detailScroll + visible; ++i) {
                const auto& f = v->files[i];
                int yy = head.bottom + px(2) + (i - detailScroll) * rh;
                RECT box{area.left, yy, area.left + px(24), yy + rh};
                drawGlyph(dc, f.wanted ? L"" : L"", box, f.wanted ? P.accent : P.muted, iconSmall);
                dhits.push_back({RECT{area.left, yy, area.right - sizeW - progW - px(16), yy + rh}, 1, i});
                std::wstring nm = f.path;
                if (!prefix.empty() && nm.compare(0, prefix.size(), prefix) == 0) nm = nm.substr(prefix.size());
                RECT nr{area.left + px(32), yy, area.right - sizeW - progW - px(16), yy + rh}; drawText(dc, nm, nr, f.wanted ? P.text : P.muted, uiFont);
                RECT sr{area.right - sizeW - progW - px(8), yy, area.right - progW - px(8), yy + rh}; drawText(dc, fmtSize(f.size), sr, P.text, uiFont, DT_RIGHT);
                double fp = f.size > 0 ? (double)f.done / (double)f.size : 1.0;
                RECT bar{area.right - progW, yy + rh / 2 - px(3), area.right - px(44), yy + rh / 2 + px(3)};
                paintProgress(dc, bar, fp, fp >= 1 ? P.success : P.accent);
                RECT pr{area.right - px(40), yy, area.right, yy + rh}; drawText(dc, std::to_wstring((int)(fp * 100)) + L"%", pr, P.muted, uiSmall, DT_RIGHT);
            }
        } else {
            int rh = px(26);
            RECT head{area.left, area.top - px(4), area.right, area.top + rh - px(4)};
            int w = area.right - area.left;
            int cx[6] = {0, w * 30 / 100, w * 55 / 100, w * 68 / 100, w * 81 / 100, w * 92 / 100};
            const wchar_t* titles[6] = {L"Address", L"Client", L"Down", L"Up", L"Has", L"Flags"};
            for (int k = 0; k < 6; ++k) {
                RECT t{area.left + cx[k], head.top, area.left + (k < 5 ? cx[k + 1] - px(10) : w), head.bottom};
                drawText(dc, titles[k], t, P.muted, uiFont, k >= 2 && k <= 4 ? DT_RIGHT : DT_LEFT);
            }
            line(dc, area.left, head.bottom, area.right, head.bottom + 1, P.border);
            int visible = std::max(1, (int)(area.bottom - head.bottom - px(2)) / rh);
            detailScroll = std::max(0, std::min(detailScroll, (int)v->peerList.size() - visible));
            if (v->peerList.empty()) {
                RECT t{area.left, head.bottom + px(8), area.right, head.bottom + px(36)};
                drawText(dc, isActive(*v) || v->status == engine::Seeding ? L"No peers connected yet." : L"Peers appear here while the torrent runs.", t, P.muted, uiFont);
            }
            for (int i = detailScroll; i < (int)v->peerList.size() && i < detailScroll + visible; ++i) {
                const auto& p = v->peerList[i];
                int yy = head.bottom + px(2) + (i - detailScroll) * rh;
                std::wstring flags = std::wstring(p.incoming ? L"in " : L"out ") + (p.seed ? L"seed" : L"") + (p.choked ? L" choked" : L"");
                std::wstring cells[6] = {widen(p.addr), widenLoose(p.client), fmtSpeed(p.down), fmtSpeed(p.up), std::to_wstring(p.pct) + L"%", flags};
                for (int k = 0; k < 6; ++k) {
                    RECT t{area.left + cx[k], yy, area.left + (k < 5 ? cx[k + 1] - px(10) : w), yy + rh};
                    drawText(dc, cells[k], t, k == 5 ? P.muted : P.text, uiFont, k >= 2 && k <= 4 ? DT_RIGHT : DT_LEFT);
                }
            }
        }
    }
    BitBlt(target, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
}

// ---------------------------------------------------------------- dialogs

std::unordered_map<HWND, int> buttonKind; // 0 normal, 1 primary, 2 toggle, 4 navigation
std::unordered_map<HWND, bool> toggleOn;
std::unordered_map<HWND, COLORREF> staticColor;

struct Dialog {
    HWND hwnd = nullptr;
    bool done = false;
    std::vector<std::pair<RECT, HWND>> outlines; // edit fields drawn as rounded boxes
    virtual ~Dialog() {}
    virtual void create() = 0;
    virtual bool command(int, int, HWND) { return false; }
    virtual void paintExtra(HDC) {}
    virtual bool drawItem(DRAWITEMSTRUCT*) { return false; }
    virtual bool measureItem(MEASUREITEMSTRUCT*) { return false; }
    virtual LRESULT message(UINT, WPARAM, LPARAM, bool& handled) { handled = false; return 0; }
    HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), hwnd, (HMENU)(INT_PTR)id, gInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)uiFont, TRUE);
        return c;
    }
    HWND label(const std::wstring& text, int x, int y, int w, int h = 20, COLORREF color = CLR_INVALID) {
        HWND l = child(L"STATIC", text.c_str(), SS_NOPREFIX | SS_ENDELLIPSIS, -1, x, y, w, h);
        if (color != CLR_INVALID) staticColor[l] = color;
        return l;
    }
    HWND button(int id, const wchar_t* text, int x, int y, int w, int h, int kind = 0) {
        HWND b = child(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, id, x, y, w, h);
        buttonKind[b] = kind;
        return b;
    }
    HWND toggle(int id, const wchar_t* text, int x, int y, int w, bool on) {
        HWND b = button(id, text, x, y, w, 32, 2);
        toggleOn[b] = on;
        return b;
    }
    HWND edit(int id, int x, int y, int w, int h = 32, const std::wstring& text = L"", DWORD extra = 0) {
        RECT box{px(x), px(y), px(x + w), px(y + h)};
        HDC dc = GetDC(hwnd); SelectObject(dc, uiFont); TEXTMETRICW tm; GetTextMetricsW(dc, &tm); ReleaseDC(hwnd, dc);
        bool multi = (extra & ES_MULTILINE) != 0;
        int eh = multi ? box.bottom - box.top - px(12) : tm.tmHeight;
        int ey = multi ? box.top + px(6) : box.top + (box.bottom - box.top - tm.tmHeight) / 2;
        HWND e = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | (multi ? ES_AUTOVSCROLL | WS_VSCROLL : ES_AUTOHSCROLL) | extra,
                                 box.left + px(10), ey, box.right - box.left - px(multi ? 12 : 20), eh, hwnd, (HMENU)(INT_PTR)id, gInst, nullptr);
        SendMessageW(e, WM_SETFONT, (WPARAM)uiFont, TRUE);
        if (multi) darkmode::window(e, P.dark);
        outlines.push_back({box, e});
        return e;
    }
    HWND combo(int id, int x, int y, int w, const std::vector<std::wstring>& items, int sel) {
        HWND c = child(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id, x, y, w, 300);
        for (auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
        SendMessageW(c, CB_SETCURSEL, sel, 0);
        SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, px(24));
        darkmode::window(c, P.dark, L"Explorer", L"DarkMode_CFD");
        return c;
    }
    bool on(HWND b) { return toggleOn[b]; }
    void cue(HWND e, const wchar_t* text) { SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)text); }
};
void drawOwnerButton(DRAWITEMSTRUCT* d) {
    HWND b = d->hwndItem;
    int kind = buttonKind.count(b) ? buttonKind[b] : 0;
    RECT r = d->rcItem;
    bool pressed = d->itemState & ODS_SELECTED, focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT), disabled = d->itemState & ODS_DISABLED;
    fill(d->hDC, r, P.chrome);
    std::wstring label = windowText(b);
    bool on = (kind == 2 || kind == 4) && toggleOn[b];
    if (kind == 4) {
        if (on || pressed) roundRect(d->hDC, r, on ? P.accentSoft : P.pressed, on ? P.accentSoft : P.pressed, px(8));
        if (focus && !on) roundRect(d->hDC, r, P.chrome, P.border, px(8));
        if (on) { RECT bar{r.left, r.top + px(9), r.left + px(3), r.bottom - px(9)}; roundRect(d->hDC, bar, P.accent, P.accent, px(3)); }
        RECT tr = r; tr.left += px(14);
        drawText(d->hDC, label, tr, P.text, on ? uiBold : uiFont, DT_LEFT);
        return;
    }
    if (kind == 2) {
        // Checkbox row: no button face, just the box and the label.
        if (focus) roundRect(d->hDC, r, P.chrome, P.border, px(8));
        RECT g{r.left + px(4), r.top, r.left + px(26), r.bottom};
        if (hasGlyph(0xE73A)) drawText(d->hDC, on ? L"" : L"", g, disabled ? P.border : on ? P.accent : P.muted, iconFont, DT_CENTER);
        else drawText(d->hDC, on ? L"☑" : L"☐", g, P.text, uiFont, DT_CENTER);
        RECT tr = r; tr.left = g.right + px(6);
        drawText(d->hDC, label, tr, disabled ? P.muted : P.text, uiFont, DT_LEFT);
        return;
    }
    COLORREF face = kind == 1 ? (pressed ? blend(P.accent, P.text, 20) : P.accent) : pressed ? P.pressed : P.surface;
    COLORREF border = focus ? (kind == 1 ? P.text : P.accent) : kind == 1 ? P.accent : P.border;
    roundRect(d->hDC, r, face, border, px(8));
    COLORREF fg = disabled ? P.muted : kind == 1 ? P.accentText : P.text;
    if (label.size() == 1 && label[0] >= 0xE000) drawText(d->hDC, label, r, fg, iconSmall, DT_CENTER);
    else drawText(d->hDC, label, r, fg, kind == 1 ? uiBold : uiFont, DT_CENTER);
}
LRESULT CALLBACK DialogProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Dialog* d = (Dialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (d && m != WM_NCCREATE) { bool handled = false; LRESULT r = d->message(m, w, l, handled); if (handled) return r; }
    switch (m) {
    case WM_NCCREATE: { auto cs = (CREATESTRUCTW*)l; d = (Dialog*)cs->lpCreateParams; d->hwnd = h; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d); break; }
    case WM_CTLCOLORSTATIC: {
        auto it = staticColor.find((HWND)l);
        SetTextColor((HDC)w, it != staticColor.end() ? it->second : P.text); SetBkColor((HDC)w, P.chrome);
        return (LRESULT)chromeBrush;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: SetTextColor((HDC)w, P.text); SetBkColor((HDC)w, P.surface); return (LRESULT)surfaceBrush;
    case WM_MEASUREITEM: if (d && d->measureItem((MEASUREITEMSTRUCT*)l)) return TRUE; break;
    case WM_DRAWITEM: {
        auto di = (DRAWITEMSTRUCT*)l;
        if (d && d->drawItem(di)) return TRUE;
        if (di->CtlType == ODT_BUTTON) { drawOwnerButton(di); return TRUE; }
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT c; GetClientRect(h, &c); fill(dc, c, P.chrome);
        if (d) {
            HWND f = GetFocus();
            for (auto& o : d->outlines) {
                if (!IsWindowVisible(o.second)) continue;
                bool en = IsWindowEnabled(o.second);
                roundRect(dc, o.first, P.surface, f == o.second ? P.accent : P.border, px(8));
                if (!en) { RECT in = o.first; InflateRect(&in, -1, -1); fill(dc, in, P.chrome); }
                if (f == o.second) { RECT u{o.first.left + px(6), o.first.bottom - px(2), o.first.right - px(6), o.first.bottom - px(1)}; fill(dc, u, P.accent); }
            }
            d->paintExtra(dc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND: {
        if (!d) break;
        int id = LOWORD(w), code = HIWORD(w);
        HWND ctl = (HWND)l;
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) { for (auto& o : d->outlines) { RECT r = o.first; InflateRect(&r, 2, 2); InvalidateRect(h, &r, FALSE); } }
        if (ctl && code == BN_CLICKED && buttonKind.count(ctl) && buttonKind[ctl] == 2) { toggleOn[ctl] = !toggleOn[ctl]; InvalidateRect(ctl, nullptr, FALSE); }
        if (d->command(id, code, ctl)) return 0;
        if (id == IDOK || id == IDCANCEL) { d->done = true; return 0; }
        return 0;
    }
    case WM_CLOSE: if (d) d->done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
bool runModal(Dialog& d, const wchar_t* title, int w, int h) {
    HWND owner = IsWindowVisible(gMain) && !IsIconic(gMain) ? gMain : nullptr;
    RECT pr;
    if (owner) GetWindowRect(gMain, &pr);
    else { HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY); MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi); pr = mi.rcWork; }
    RECT r{0, 0, px(w), px(h)};
    AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int W = r.right - r.left, H = r.bottom - r.top;
    int x = pr.left + (pr.right - pr.left - W) / 2, y = pr.top + std::max<int>(0, (pr.bottom - pr.top - H) / 3);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | (owner ? 0 : WS_EX_APPWINDOW | WS_EX_TOPMOST), DialogClass, title, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, W, H, owner, nullptr, gInst, &d);
    if (!hwnd) return false;
    titleBar(hwnd);
    d.create();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    if (owner) EnableWindow(gMain, FALSE);
    MSG msg;
    while (!d.done) {
        BOOL r2 = GetMessageW(&msg, nullptr, 0, 0);
        if (r2 <= 0) { PostQuitMessage(r2 == 0 ? (int)msg.wParam : 0); break; }
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) { EnableWindow(gMain, TRUE); SetActiveWindow(gMain); }
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) { buttonKind.erase(c); toggleOn.erase(c); staticColor.erase(c); }
    DestroyWindow(hwnd);
    return true;
}
bool dialogOpen = false;

struct MessageDialog : Dialog {
    std::wstring text; bool ok = false, confirm = false; std::wstring okLabel = L"OK";
    int lines = 1;
    void create() override {
        label(text, 20, 18, 420, 20 * lines + 4);
        int y = 30 + 20 * lines;
        if (confirm) button(IDCANCEL, L"Cancel", 236, y, 96, 32);
        button(IDOK, okLabel.c_str(), 340, y, 100, 32, 1);
    }
    bool command(int id, int, HWND) override { if (id == IDOK) { ok = true; done = true; return true; } return false; }
};
bool showMessage(const std::wstring& title, const std::wstring& text, bool confirm = false, const std::wstring& okLabel = L"OK") {
    MessageDialog d; d.text = text; d.confirm = confirm; d.okLabel = okLabel;
    // Rough wrap estimate: about 64 characters per line.
    int n = 1, run = 0;
    for (wchar_t c : text) { if (c == L'\n' || ++run > 64) { ++n; run = 0; } }
    d.lines = std::min(n, 18);
    dialogOpen = true;
    runModal(d, title.c_str(), 460, 80 + 20 * d.lines);
    dialogOpen = false;
    return d.ok;
}

// Shell pickers.
bool pickFolder(HWND owner, std::wstring& path, const wchar_t* title) {
    IFileOpenDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&fd))) return false;
    DWORD opt = 0; fd->GetOptions(&opt); fd->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    fd->SetTitle(title);
    if (!path.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem, (void**)&si))) { fd->SetFolder(si); si->Release(); }
    }
    bool ok = false;
    if (SUCCEEDED(fd->Show(owner))) {
        IShellItem* res = nullptr;
        if (SUCCEEDED(fd->GetResult(&res))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); ok = true; }
            res->Release();
        }
    }
    fd->Release();
    return ok;
}
std::vector<std::wstring> pickFiles(HWND owner, const wchar_t* filter, const std::wstring& dir, bool multi) {
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW of{sizeof of};
    of.hwndOwner = owner; of.lpstrFilter = filter; of.lpstrFile = buf.data(); of.nMaxFile = (DWORD)buf.size();
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | (multi ? OFN_ALLOWMULTISELECT : 0);
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&of)) return out;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) { out.push_back(first); return out; }
    while (*p) { out.push_back(joinPath(first, p)); p += wcslen(p) + 1; }
    return out;
}
std::wstring pickSave(HWND owner, const wchar_t* filter, const std::wstring& suggested, const wchar_t* ext) {
    std::vector<wchar_t> buf(32768, 0);
    std::wstring name = fileNameOf(suggested);
    wcsncpy(buf.data(), name.c_str(), buf.size() - 1);
    std::wstring dir = folderOf(suggested);
    OPENFILENAMEW of{sizeof of};
    of.hwndOwner = owner; of.lpstrFilter = filter; of.lpstrFile = buf.data(); of.nMaxFile = (DWORD)buf.size(); of.lpstrDefExt = ext;
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetSaveFileNameW(&of) ? std::wstring(buf.data()) : std::wstring();
}

void scheduleSave();
void rememberFolder(const std::wstring& f) {
    if (f.empty()) return;
    auto& r = S.ui.recentFolders;
    r.erase(std::remove_if(r.begin(), r.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), f.c_str()) == 0; }), r.end());
    r.insert(r.begin(), f);
    if (r.size() > 8) r.resize(8);
    scheduleSave();
}
// Recent folders menu below a button; returns the chosen folder or "".
std::wstring recentFolderMenu(HWND button) {
    HMENU m = CreatePopupMenu();
    std::wstring def = E.config().folder;
    std::vector<std::wstring> list{def};
    for (auto& f : S.ui.recentFolders) if (_wcsicmp(f.c_str(), def.c_str()) != 0) list.push_back(f);
    for (size_t i = 0; i < list.size(); ++i) {
        std::wstring label = list[i] + (i == 0 ? L"   (default)" : L"");
        std::wstring esc; for (wchar_t c : label) { if (c == L'&') esc += L'&'; esc += c; }
        AppendMenuW(m, MF_STRING, 1 + i, esc.c_str());
    }
    RECT r; GetWindowRect(button, &r);
    int sel = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom + px(2), GetParent(button), nullptr);
    DestroyMenu(m);
    return sel > 0 && sel <= (int)list.size() ? list[sel - 1] : std::wstring();
}

std::string clipboardText() {
    std::string out;
    // The copying app may still hold the clipboard open when the change notification arrives.
    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(gMain) != 0); ++i) Sleep(15);
    if (!open) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto p = (const wchar_t*)GlobalLock(h)) { size_t n = wcsnlen(p, 1 << 20); out = narrow(std::wstring(p, n)); GlobalUnlock(h); }
    }
    CloseClipboard();
    return out;
}
void setClipboardText(const std::wstring& s) {
    if (!OpenClipboard(gMain)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) { memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); ignoreClipboard = true; SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard();
}
bool looksLikeLink(const std::string& s) {
    std::string l = lower(trim(s));
    return startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "magnet:?");
}
std::vector<std::string> linksIn(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find_first_of("\r\n", i);
        std::string ln = trim(text.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = nl == std::string::npos ? text.size() : nl + 1;
        if (looksLikeLink(ln) && ln.find(' ') == std::string::npos) out.push_back(ln);
    }
    return out;
}

void refreshModel();
void selectOnly(uint32_t id);

struct AddUrlDialog : Dialog {
    std::string initial;
    HWND urls = nullptr, folder = nullptr, name = nullptr, conns = nullptr, net = nullptr, checksum = nullptr, user = nullptr, pass = nullptr, start = nullptr, err = nullptr, recent = nullptr;
    std::vector<uint32_t> added;
    void create() override {
        label(L"Address  (one link per line: http, https, .onion or magnet)", 20, 16, 520);
        urls = edit(101, 20, 38, 520, 66, widen(initial), ES_MULTILINE | ES_WANTRETURN);
        label(L"Save to", 20, 114, 300);
        folder = edit(102, 20, 136, 384, 32, E.config().folder);
        button(103, L"Browse…", 412, 136, 88, 32);
        recent = button(104, L"", 508, 136, 32, 32);
        label(L"File name  (optional)", 20, 180, 260);
        name = edit(105, 20, 202, 276, 32);
        label(L"Connections", 308, 180, 100);
        conns = edit(106, 308, 202, 92, 32, L"", ES_NUMBER);
        cue(conns, (L"Default " + std::to_wstring(E.config().connections)).c_str());
        label(L"Network", 412, 180, 128);
        net = combo(107, 412, 203, 128, {L"Automatic", L"Through Tor", L"Direct"}, 0);
        label(L"Checksum  (optional: sha256, sha1 or md5 hex)", 20, 246, 520);
        checksum = edit(108, 20, 268, 520, 32);
        label(L"User name  (optional)", 20, 312, 256);
        user = edit(109, 20, 334, 256, 32);
        label(L"Password", 284, 312, 256);
        pass = edit(110, 284, 334, 256, 32, L"", ES_PASSWORD);
        err = label(L"", 20, 376, 520, 20, P.danger);
        start = toggle(111, L"Start now", 16, 404, 140, true);
        button(IDCANCEL, L"Cancel", 336, 404, 96, 32);
        button(IDOK, L"Download", 440, 404, 100, 32, 1);
        SetFocus(initial.empty() ? urls : folder);
        SendMessageW(urls, EM_SETSEL, 0, -1);
    }
    bool command(int id, int code, HWND) override {
        if (id == 103) { std::wstring f = windowText(folder); if (pickFolder(hwnd, f, L"Save downloads to")) SetWindowTextW(folder, f.c_str()); return true; }
        if (id == 104) { std::wstring f = recentFolderMenu(recent); if (!f.empty()) SetWindowTextW(folder, f.c_str()); return true; }
        if (id == 101 && code == EN_CHANGE) {
            bool single = linksIn(narrow(windowText(urls))).size() <= 1;
            EnableWindow(name, single); InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        if (id != IDOK) return false;
        auto links = linksIn(narrow(windowText(urls)));
        if (links.empty()) { SetWindowTextW(err, L"Enter an http, https or magnet link."); SetFocus(urls); return true; }
        std::string cs = trim(narrow(windowText(checksum)));
        std::string algo, dig;
        if (!cs.empty() && !engine::parseChecksum(cs, algo, dig)) { SetWindowTextW(err, L"The checksum is not a valid sha256, sha1, sha512 or md5 hex digest."); SetFocus(checksum); return true; }
        std::wstring fold = trimW(windowText(folder));
        if (!fold.empty() && !makeDirs(fold)) { SetWindowTextW(err, L"Cannot create that folder."); SetFocus(folder); return true; }
        std::wstring errors;
        for (auto& u : links) {
            engine::AddRequest r;
            r.url = u;
            r.folder = _wcsicmp(fold.c_str(), E.config().folder.c_str()) == 0 ? L"" : fold;
            if (links.size() == 1) r.name = trimW(windowText(name));
            r.connections = (int)std::max<int64_t>(0, std::min<int64_t>(32, parseI64(windowText(conns))));
            int ni = (int)SendMessageW(net, CB_GETCURSEL, 0, 0);
            r.tor = ni == 1 ? 1 : ni == 2 ? 2 : 0;
            r.checksum = links.size() == 1 ? cs : "";
            r.user = narrow(windowText(user)); r.pass = narrow(windowText(pass));
            r.start = on(start);
            std::wstring e;
            uint32_t id2 = E.add(r, e);
            if (id2) added.push_back(id2);
            else errors += (errors.empty() ? L"" : L"\n") + e;
        }
        if (!fold.empty()) rememberFolder(fold);
        if (!errors.empty() && added.empty()) { SetWindowTextW(err, errors.c_str()); return true; }
        if (!errors.empty()) showMessage(AppTitle, errors);
        done = true;
        return true;
    }
};

// Owner-drawn list of torrent files with check boxes.
struct AddTorrentDialog : Dialog {
    std::wstring sourcePath;
    std::string data;
    bt::Meta meta;
    std::vector<size_t> vis;
    std::vector<char> want;
    HWND folder = nullptr, list = nullptr, start = nullptr, err = nullptr, recent = nullptr, summary = nullptr;
    uint32_t added = 0;
    static LRESULT CALLBACK ListSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        auto self = (AddTorrentDialog*)ref;
        if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
            LRESULT r = SendMessageW(h, LB_ITEMFROMPOINT, 0, l);
            int i = LOWORD(r);
            if (!HIWORD(r) && i >= 0 && i < (int)self->want.size()) { self->want[i] = !self->want[i]; self->changed(); }
            SetFocus(h); SendMessageW(h, LB_SETCURSEL, i, 0);
            return 0;
        }
        if (m == WM_KEYDOWN && w == VK_SPACE) {
            int i = (int)SendMessageW(h, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)self->want.size()) { self->want[i] = !self->want[i]; self->changed(); }
            return 0;
        }
        if (m == WM_GETDLGCODE) return DLGC_WANTARROWS | DLGC_WANTCHARS;
        return DefSubclassProc(h, m, w, l);
    }
    void changed() {
        InvalidateRect(list, nullptr, FALSE);
        int n = 0; int64_t bytes = 0;
        for (size_t i = 0; i < want.size(); ++i) if (want[i]) { ++n; bytes += meta.files[vis[i]].length; }
        std::wstring s = fmtNum(n) + L" of " + fmtNum(want.size()) + L" files  ·  " + fmtSize(bytes);
        SetWindowTextW(summary, s.c_str());
    }
    void create() override {
        label(L"Save to", 20, 70, 300);
        folder = edit(102, 20, 92, 444, 32, S.ui.lastTorrentFolder.empty() ? E.config().folder : S.ui.lastTorrentFolder);
        button(103, L"Browse…", 472, 92, 88, 32);
        recent = button(104, L"", 568, 92, 32, 32);
        label(L"Files", 20, 138, 80);
        summary = label(L"", 100, 138, 290, 20, P.muted);
        button(105, L"All", 452, 134, 70, 28);
        button(106, L"None", 530, 134, 70, 28);
        list = child(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, 107, 20, 170, 580, 236);
        darkmode::window(list, P.dark);
        SetWindowSubclass(list, ListSub, 1, (DWORD_PTR)this);
        for (size_t i = 0; i < vis.size(); ++i) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)L"");
        err = label(L"", 20, 412, 580, 20, P.danger);
        start = toggle(111, L"Start now", 16, 440, 140, true);
        button(IDCANCEL, L"Cancel", 396, 440, 96, 32);
        button(IDOK, L"Add torrent", 500, 440, 100, 32, 1);
        changed();
        SetFocus(GetDlgItem(hwnd, IDOK));
    }
    void paintExtra(HDC dc) override {
        RECT g{px(20), px(14), px(48), px(46)}; drawGlyph(dc, L"", g, P.accent);
        RECT t{px(56), px(12), px(600), px(36)}; drawText(dc, meta.name, t, P.text, uiLarge);
        std::wstring sub = fmtSize(meta.total) + L"  ·  " + fmtNum(meta.pieceCount()) + L" pieces of " + fmtSize(meta.pieceLen) + (meta.priv ? L"  ·  private" : L"");
        if (!meta.comment.empty()) sub += L"  ·  " + meta.comment;
        RECT s{px(56), px(36), px(600), px(56)}; drawText(dc, sub, s, P.muted, uiFont);
        RECT box{px(20) - 1, px(170) - 1, px(600) + 1, px(406) + 1};
        roundRect(dc, box, P.surface, P.border, px(4));
    }
    bool measureItem(MEASUREITEMSTRUCT* m) override { if (m->CtlID != 107) return false; m->itemHeight = px(28); return true; }
    bool drawItem(DRAWITEMSTRUCT* d) override {
        if (d->CtlID != 107) return false;
        RECT r = d->rcItem;
        fill(d->hDC, r, (d->itemState & ODS_SELECTED) ? P.selection : P.surface);
        int i = (int)d->itemID;
        if (i < 0 || i >= (int)vis.size()) return true;
        const bt::File& f = meta.files[vis[i]];
        RECT cb{r.left + px(6), r.top, r.left + px(28), r.bottom};
        drawText(d->hDC, want[i] ? L"" : L"", cb, want[i] ? P.accent : P.muted, iconSmall, DT_CENTER);
        std::wstring nm = f.path;
        if (meta.multi && nm.compare(0, meta.name.size() + 1, meta.name + L"\\") == 0) nm = nm.substr(meta.name.size() + 1);
        RECT sr{r.right - px(100), r.top, r.right - px(10), r.bottom};
        drawText(d->hDC, fmtSize(f.length), sr, P.muted, uiFont, DT_RIGHT);
        RECT nr{r.left + px(36), r.top, sr.left - px(8), r.bottom};
        drawText(d->hDC, nm, nr, want[i] ? P.text : P.muted, uiFont);
        return true;
    }
    bool command(int id, int, HWND) override {
        if (id == 103) { std::wstring f = windowText(folder); if (pickFolder(hwnd, f, L"Save the torrent to")) SetWindowTextW(folder, f.c_str()); return true; }
        if (id == 104) { std::wstring f = recentFolderMenu(recent); if (!f.empty()) SetWindowTextW(folder, f.c_str()); return true; }
        if (id == 105 || id == 106) { for (auto& x : want) x = id == 105; changed(); return true; }
        if (id != IDOK) return false;
        std::string sel;
        bool any = false;
        for (char x : want) { sel += x ? '1' : '0'; any |= x != 0; }
        if (!any) { SetWindowTextW(err, L"Select at least one file."); return true; }
        std::wstring fold = trimW(windowText(folder));
        if (fold.empty()) fold = E.config().folder;
        if (!makeDirs(fold)) { SetWindowTextW(err, L"Cannot create that folder."); return true; }
        engine::AddRequest r;
        r.torrentData = data; r.folder = fold; r.fileSel = sel; r.start = on(start);
        std::wstring e;
        added = E.add(r, e);
        if (!added) { SetWindowTextW(err, e.c_str()); return true; }
        S.ui.lastTorrentFolder = fold;
        rememberFolder(fold);
        done = true;
        return true;
    }
};

struct DeleteDialog : Dialog {
    std::wstring text; bool withFiles = false, ok = false; HWND files = nullptr;
    void create() override {
        label(text, 20, 18, 400, 40);
        files = toggle(101, L"Also delete the downloaded files", 16, 62, 300, withFiles);
        button(IDCANCEL, L"Cancel", 220, 112, 96, 32);
        button(IDOK, L"Remove", 324, 112, 96, 32, 1);
        SetFocus(GetDlgItem(hwnd, IDOK));
    }
    bool command(int id, int, HWND) override { if (id == IDOK) { ok = true; withFiles = on(files); done = true; return true; } return false; }
};

struct PropertiesDialog : Dialog {
    uint32_t id = 0;
    HWND url = nullptr, limit = nullptr, conns = nullptr, net = nullptr, err = nullptr;
    void create() override {
        auto it = E.find(id);
        if (!it) { done = true; return; }
        std::string u; int64_t lim; int c, t, kind;
        { std::lock_guard<std::mutex> lk(it->m); u = it->url; lim = it->limit; c = it->connections; t = it->tor; kind = it->kind; }
        label(kind == engine::KindHttp ? L"Address  (change it when a link has expired)" : L"Magnet link", 20, 16, 480);
        url = edit(101, 20, 38, 480, 32, widen(u), kind == engine::KindHttp ? 0 : ES_READONLY);
        label(L"Speed limit (KB/s, 0 = none)", 20, 82, 230);
        limit = edit(102, 20, 104, 230, 32, std::to_wstring(lim / 1024), ES_NUMBER);
        label(L"Connections", 262, 82, 100);
        conns = edit(103, 262, 104, 100, 32, c ? std::to_wstring(c) : L"", ES_NUMBER);
        cue(conns, L"Default");
        if (kind != engine::KindHttp) EnableWindow(conns, FALSE);
        label(L"Network", 374, 82, 126);
        net = combo(104, 374, 105, 126, {L"Automatic", L"Through Tor", L"Direct"}, t);
        err = label(L"", 20, 146, 480, 20, P.danger);
        button(IDCANCEL, L"Cancel", 296, 172, 96, 32);
        button(IDOK, L"Save", 400, 172, 100, 32, 1);
    }
    bool command(int cid, int, HWND) override {
        if (cid != IDOK) return false;
        auto it = E.find(id);
        if (!it) { done = true; return true; }
        std::string u = trim(narrow(windowText(url)));
        bool http = it->kind == engine::KindHttp;
        net::Url pu;
        if (http && !net::parseUrl(u, pu)) { SetWindowTextW(err, L"That is not a valid http or https URL."); return true; }
        if (http && u != it->url) E.setUrl(id, pu.str());
        {
            std::lock_guard<std::mutex> lk(it->m);
            it->limit = std::max<int64_t>(0, parseI64(windowText(limit))) * 1024;
            it->limiter.rate = it->limit;
            if (http) it->connections = (int)std::max<int64_t>(0, std::min<int64_t>(32, parseI64(windowText(conns))));
            it->tor = (int)SendMessageW(net, CB_GETCURSEL, 0, 0);
            it->dirty = true;
        }
        E.changed();
        done = true;
        return true;
    }
};

bool registerHandlers(std::wstring& err);
constexpr UINT WM_TORTEST = WM_APP + 50;

struct SettingsDialog : Dialog {
    engine::Config c;
    settings::Ui u;
    bool ok = false;
    int page = 0;
    std::vector<HWND> nav;
    std::vector<std::vector<HWND>> pages{5};
    int cur = -1;
    // Downloads
    HWND folder, catFolders, maxActive, connections, speed, retries, timeout, prealloc;
    // Connection
    HWND proxyKind, proxyHost, proxyPort, proxyUser, proxyPass, verifyTls, userAgent;
    // Tor
    HWND torAll, torIsolate, torHost, torPort, torConns, torLaunch, torPath, torStatus;
    // BitTorrent
    HWND btPort, btPeers, btDht, seedRatio, upLimit, btTor;
    // Interface
    HWND clip, tray, closeTray, notify, confirmDel, startMin, regStatus;
    HWND* group = nullptr;
    int at = 0; // page being built

    template <class F> HWND add(F make) { HWND h = make(); pages[at].push_back(h); return h; }
    HWND L(const std::wstring& t, int x, int y, int w, COLORREF col = CLR_INVALID) { return add([&] { return label(t, x, y, w, 20, col); }); }
    HWND Ed(int id, int x, int y, int w, const std::wstring& t, DWORD ex = 0) { return add([&] { return edit(id, x, y, w, 32, t, ex); }); }
    HWND Tg(int id, const wchar_t* t, int x, int y, int w, bool v) { return add([&] { return toggle(id, t, x, y, w, v); }); }
    HWND Bt(int id, const wchar_t* t, int x, int y, int w) { return add([&] { return button(id, t, x, y, w, 32); }); }
    static std::wstring num(int64_t v) { return std::to_wstring((long long)v); }

    void create() override {
        const wchar_t* names[] = {L"Downloads", L"Connection", L"Tor", L"BitTorrent", L"Interface"};
        for (int i = 0; i < 5; ++i) { HWND b = button(300 + i, names[i], 12, 14 + i * 40, 150, 36, 4); nav.push_back(b); }
        int X = 184, W = 440;
        at = 0;
        L(L"Download folder", X, 14, W);
        folder = Ed(101, X, 36, W - 96, c.folder);
        Bt(102, L"Browse…", X + W - 88, 36, 88);
        catFolders = Tg(103, L"Save into a sub-folder per category (Video, Music, …)", X - 4, 74, W, c.categoryFolders);
        L(L"Downloads at once", X, 116, 210); L(L"Connections per download", X + 230, 116, 210);
        maxActive = Ed(104, X, 138, 210, num(c.maxActive), ES_NUMBER); connections = Ed(105, X + 230, 138, 210, num(c.connections), ES_NUMBER);
        L(L"Speed limit, KB/s (0 = unlimited)", X, 182, 440);
        speed = Ed(106, X, 204, 210, num(c.speedLimit / 1024), ES_NUMBER);
        L(L"Retries", X, 248, 210); L(L"Timeout, seconds", X + 230, 248, 210);
        retries = Ed(107, X, 270, 210, num(c.retries), ES_NUMBER); timeout = Ed(108, X + 230, 270, 210, num(c.timeoutSec), ES_NUMBER);
        prealloc = Tg(109, L"Reserve disk space when a download starts", X - 4, 312, W, c.preallocate);

        at = 1;
        L(L"Proxy for regular downloads", X, 14, W);
        proxyKind = add([&] { return combo(120, X, 37, 210, {L"No proxy", L"SOCKS5", L"HTTP (CONNECT)"}, c.proxy.kind); });
        L(L"Host", X, 80, 300); L(L"Port", X + 320, 80, 120);
        proxyHost = Ed(121, X, 102, 310, widen(c.proxy.host)); proxyPort = Ed(122, X + 320, 102, 120, c.proxy.port ? num(c.proxy.port) : L"", ES_NUMBER);
        L(L"User name", X, 146, 210); L(L"Password", X + 230, 146, 210);
        proxyUser = Ed(123, X, 168, 210, widen(c.proxy.user)); proxyPass = Ed(124, X + 230, 168, 210, widen(c.proxy.pass), ES_PASSWORD);
        verifyTls = Tg(125, L"Verify HTTPS certificates", X - 4, 212, W, c.verifyTls);
        L(L"User agent", X, 254, W);
        userAgent = Ed(126, X, 276, W, widen(c.userAgent));

        at = 2;
        L(L"TDM uses Tor's SOCKS port for .onion links and for downloads you send through Tor.", X, 14, W, P.muted);
        torAll = Tg(140, L"Send every download through Tor", X - 4, 40, W, c.torAll);
        torIsolate = Tg(141, L"Use a separate Tor circuit for each connection", X - 4, 74, W, c.torIsolate);
        L(L"SOCKS host", X, 114, 300); L(L"Port (0 = find 9050 / 9150)", X + 230, 114, 220);
        torHost = Ed(142, X, 136, 210, widen(c.torHost)); torPort = Ed(143, X + 230, 136, 210, num(c.torPort), ES_NUMBER);
        L(L"Connections per download over Tor", X, 180, W);
        torConns = Ed(144, X, 202, 210, num(c.torConnections), ES_NUMBER);
        torLaunch = Tg(145, L"Start tor.exe myself when Tor is needed", X - 4, 244, W, c.torLaunch);
        L(L"tor.exe (from the Tor Expert Bundle or Tor Browser)", X, 282, W);
        torPath = Ed(146, X, 304, W - 96, c.torPath);
        Bt(147, L"Browse…", X + W - 88, 304, 88);
        Bt(148, L"Test Tor", X, 348, 110);
        torStatus = L(L"", X + 122, 354, W - 122, P.muted);

        at = 3;
        L(L"Listening port", X, 14, 210); L(L"Peers per torrent", X + 230, 14, 210);
        btPort = Ed(160, X, 36, 210, num(c.btPort), ES_NUMBER); btPeers = Ed(161, X + 230, 36, 210, num(c.btMaxPeers), ES_NUMBER);
        btDht = Tg(162, L"Find peers with DHT (distributed hash table)", X - 4, 78, W, c.btDht);
        wchar_t ratio[32]; swprintf(ratio, 32, L"%.2f", c.seedRatio);
        L(L"Seed until ratio (0 = stop when done)", X, 118, 220); L(L"Upload limit, KB/s (0 = unlimited)", X + 230, 118, 220);
        seedRatio = Ed(163, X, 140, 210, ratio); upLimit = Ed(164, X + 230, 140, 210, num(c.uploadLimit / 1024), ES_NUMBER);
        btTor = Tg(165, L"Route trackers and peers through Tor", X - 4, 184, W, c.btViaTor);
        L(L"Through Tor, only HTTP trackers and outgoing TCP peers work (no DHT, no incoming peers).", X, 218, W, P.muted);

        at = 4;
        clip = Tg(180, L"Offer to download links I copy", X - 4, 10, W, u.clipboard);
        tray = Tg(181, L"Show an icon in the notification area", X - 4, 44, W, u.tray);
        closeTray = Tg(182, L"Closing the window keeps TDM running there", X - 4, 78, W, u.closeToTray);
        notify = Tg(183, L"Notify me when a download finishes", X - 4, 112, W, u.notify);
        confirmDel = Tg(184, L"Ask before removing downloads", X - 4, 146, W, u.confirmDelete);
        startMin = Tg(185, L"Start minimized", X - 4, 180, W, u.startMinimized);
        Bt(186, L"Open magnet links and .torrent files with TDM", X, 226, 330);
        regStatus = L(L"", X, 266, W, P.muted);

        button(IDCANCEL, L"Cancel", 420, 412, 96, 32);
        button(IDOK, L"Save", 524, 412, 100, 32, 1);
        show(page);
    }
    void show(int p) {
        cur = p;
        for (int i = 0; i < 5; ++i) {
            for (HWND h : pages[i]) ShowWindow(h, i == p ? SW_SHOW : SW_HIDE);
            toggleOn[nav[i]] = i == p; InvalidateRect(nav[i], nullptr, FALSE);
        }
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    void paintExtra(HDC dc) override {
        RECT l{px(172), px(10), px(173), px(396)}; fill(dc, l, P.border);
    }
    LRESULT message(UINT m, WPARAM, LPARAM l, bool& handled) override {
        if (m != WM_TORTEST) { handled = false; return 0; }
        handled = true;
        auto s = (std::pair<bool, std::wstring>*)l;
        staticColor[torStatus] = s->first ? P.success : P.danger;
        SetWindowTextW(torStatus, s->second.c_str());
        delete s;
        return 0;
    }
    engine::Config gather() {
        engine::Config n = c;
        auto I = [](HWND h, int64_t lo, int64_t hi, int64_t def) { int64_t v = parseI64(windowText(h), def); return std::max(lo, std::min(hi, v)); };
        std::wstring f = trimW(windowText(folder)); if (!f.empty()) n.folder = f;
        n.categoryFolders = on(catFolders);
        n.maxActive = (int)I(maxActive, 1, 50, 4); n.connections = (int)I(connections, 1, 32, 8);
        n.speedLimit = I(speed, 0, 10000000, 0) * 1024;
        n.retries = (int)I(retries, 0, 1000, 12); n.timeoutSec = (int)I(timeout, 5, 600, 30);
        n.preallocate = on(prealloc);
        n.proxy.kind = (int)SendMessageW(proxyKind, CB_GETCURSEL, 0, 0);
        n.proxy.host = trim(narrow(windowText(proxyHost))); n.proxy.port = (int)I(proxyPort, 0, 65535, 0);
        n.proxy.user = narrow(windowText(proxyUser)); n.proxy.pass = narrow(windowText(proxyPass));
        n.verifyTls = on(verifyTls);
        std::string ua = trim(narrow(windowText(userAgent))); n.userAgent = ua.empty() ? http::Options().userAgent : ua;
        n.torAll = on(torAll); n.torIsolate = on(torIsolate);
        std::string th = trim(narrow(windowText(torHost))); n.torHost = th.empty() ? "127.0.0.1" : th;
        n.torPort = (int)I(torPort, 0, 65535, 0); n.torConnections = (int)I(torConns, 1, 16, 4);
        n.torLaunch = on(torLaunch); n.torPath = trimW(windowText(torPath));
        n.btPort = (int)I(btPort, 1024, 65535, 6881); n.btMaxPeers = (int)I(btPeers, 5, 500, 50);
        n.btDht = on(btDht);
        double r = _wtof(windowText(seedRatio).c_str()); n.seedRatio = r < 0 ? 0 : r > 1000 ? 1000 : r;
        n.uploadLimit = I(upLimit, 0, 10000000, 0) * 1024;
        n.btViaTor = on(btTor);
        return n;
    }
    bool command(int id, int, HWND) override {
        if (id >= 300 && id < 305) { show(id - 300); return true; }
        if (id == 102) { std::wstring f = windowText(folder); if (pickFolder(hwnd, f, L"Download folder")) SetWindowTextW(folder, f.c_str()); return true; }
        if (id == 147) {
            auto f = pickFiles(hwnd, L"tor.exe\0tor.exe\0Programs (*.exe)\0*.exe\0", folderOf(windowText(torPath)), false);
            if (!f.empty()) SetWindowTextW(torPath, f[0].c_str());
            return true;
        }
        if (id == 148) {
            engine::Config n = gather();
            SetWindowTextW(torStatus, n.torLaunch ? L"Starting Tor… (can take a minute)" : L"Looking for Tor…");
            staticColor[torStatus] = P.muted;
            HWND target = hwnd;
            std::thread([n, target] {
                E.forgetTor();
                std::wstring err;
                int port = E.torPort(n, err);
                auto res = new std::pair<bool, std::wstring>(port != 0, port ? L"Tor is working on port " + std::to_wstring(port) + L"." : err);
                if (!PostMessageW(target, WM_TORTEST, 0, (LPARAM)res)) delete res;
            }).detach();
            return true;
        }
        if (id == 186) {
            std::wstring err;
            bool r = registerHandlers(err);
            staticColor[regStatus] = r ? P.success : P.danger;
            SetWindowTextW(regStatus, r ? L"Done. Magnet links and .torrent files now open in TDM." : err.c_str());
            return true;
        }
        if (id != IDOK) return false;
        c = gather();
        u.clipboard = on(clip); u.tray = on(tray); u.closeToTray = on(closeTray); u.notify = on(notify); u.confirmDelete = on(confirmDel); u.startMinimized = on(startMin);
        makeDirs(c.folder);
        ok = true; done = true;
        return true;
    }
};

struct CreateTorrentDialog : Dialog {
    HWND source, trackers, piece, priv, seed, status;
    bool busy = false;
    std::atomic<bool> cancel{false};
    uint32_t added = 0;
    void create() override {
        label(L"File or folder to share", 20, 16, 400);
        source = edit(101, 20, 38, 336, 32);
        button(102, L"File…", 364, 38, 84, 32);
        button(103, L"Folder…", 456, 38, 84, 32);
        label(L"Trackers  (one per line, optional)", 20, 82, 400);
        trackers = edit(104, 20, 104, 520, 92, L"udp://tracker.opentrackr.org:1337/announce", ES_MULTILINE | ES_WANTRETURN);
        label(L"Piece size", 20, 208, 200);
        piece = combo(105, 20, 231, 200, {L"Automatic", L"256 KB", L"512 KB", L"1 MB", L"2 MB", L"4 MB", L"8 MB", L"16 MB"}, 0);
        priv = toggle(106, L"Private (trackers only, no DHT or PEX)", 236, 226, 304, false);
        seed = toggle(107, L"Start seeding it in TDM", 16, 270, 300, true);
        status = label(L"", 20, 312, 520, 20, P.muted);
        button(IDCANCEL, L"Cancel", 336, 344, 96, 32);
        button(IDOK, L"Create…", 440, 344, 100, 32, 1);
    }
    bool command(int id, int, HWND) override {
        if (id == IDCANCEL && busy) { cancel = true; return true; }
        if (busy) return true;
        if (id == 102) { auto f = pickFiles(hwnd, L"All files\0*.*\0", L"", false); if (!f.empty()) SetWindowTextW(source, f[0].c_str()); return true; }
        if (id == 103) { std::wstring f = windowText(source); if (pickFolder(hwnd, f, L"Folder to share")) SetWindowTextW(source, f.c_str()); return true; }
        if (id != IDOK) return false;
        std::wstring src = trimW(windowText(source));
        while (src.size() > 3 && (src.back() == L'\\' || src.back() == L'/')) src.pop_back();
        if (src.empty() || !fileExists(src)) { staticColor[status] = P.danger; SetWindowTextW(status, L"Choose a file or folder that exists."); return true; }
        std::wstring out = pickSave(hwnd, L"Torrent files (*.torrent)\0*.torrent\0", joinPath(folderOf(src), fileNameOf(src) + L".torrent"), L"torrent");
        if (out.empty()) return true;
        std::vector<std::string> trs = linksInTrackers(narrow(windowText(trackers)));
        static const int64_t sizes[] = {0, 256 << 10, 512 << 10, 1 << 20, 2 << 20, 4 << 20, 8 << 20, 16 << 20};
        int64_t pl = sizes[std::max(0, std::min(7, (int)SendMessageW(piece, CB_GETCURSEL, 0, 0)))];
        bool isPriv = on(priv);
        busy = true; cancel = false;
        staticColor[status] = P.muted;
        SetWindowTextW(status, L"Hashing pieces…");
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_APPSTARTING));
        std::string data, err;
        std::atomic<bool> finished{false};
        std::thread t([&] { data = bt::createTorrent(src, pl, trs, isPriv, err, &cancel); finished = true; });
        MSG msg;
        while (!finished) {
            if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) == WAIT_OBJECT_0)
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
        }
        t.join();
        SetCursor(old);
        busy = false;
        if (cancel) { SetWindowTextW(status, L"Cancelled."); return true; }
        if (data.empty()) { staticColor[status] = P.danger; SetWindowTextW(status, widen(err.empty() ? std::string("Could not create the torrent") : err).c_str()); return true; }
        if (!engine::writeWholeFile(out, data)) { staticColor[status] = P.danger; SetWindowTextW(status, (L"Cannot save: " + errorText(GetLastError())).c_str()); return true; }
        if (on(seed)) {
            engine::AddRequest r; r.torrentData = data; r.folder = folderOf(src); r.start = true;
            std::wstring e;
            added = E.add(r, e);
            if (!added) { staticColor[status] = P.danger; SetWindowTextW(status, e.c_str()); return true; }
        }
        done = true;
        return true;
    }
    static std::vector<std::string> linksInTrackers(const std::string& text) {
        std::vector<std::string> out;
        for (auto& l : linksIn(text)) out.push_back(l);
        size_t i = 0;
        while (i < text.size()) {
            size_t nl = text.find_first_of("\r\n", i);
            std::string ln = trim(text.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
            i = nl == std::string::npos ? text.size() : nl + 1;
            if (startsWith(lower(ln), "udp://")) out.push_back(ln);
        }
        return out;
    }
};

// ---------------------------------------------------------------- actions

void layoutChildren() {
    if (!gList) return;
    MoveWindow(gList, rcList.left, rcList.top, rcList.right - rcList.left, rcList.bottom - rcList.top, TRUE);
    bool det = rcDetails.bottom > rcDetails.top;
    ShowWindow(gDetails, det ? SW_SHOWNA : SW_HIDE);
    if (det) MoveWindow(gDetails, rcDetails.left, rcDetails.top, rcDetails.right - rcDetails.left, rcDetails.bottom - rcDetails.top, TRUE);
    ShowWindow(gSearch, S.ui.sidebar ? SW_SHOWNA : SW_HIDE);
    if (S.ui.sidebar) {
        int eh = 0;
        { HDC d2 = GetDC(gSearch); SelectObject(d2, uiFont); TEXTMETRICW tm; GetTextMetricsW(d2, &tm); eh = tm.tmHeight; ReleaseDC(gSearch, d2); }
        MoveWindow(gSearch, rcSearchBox.left + px(34), rcSearchBox.top + (rcSearchBox.bottom - rcSearchBox.top - eh) / 2, rcSearchBox.right - rcSearchBox.left - px(42), eh, TRUE);
    }
    updateListScroll();
}
void refreshChrome() { buildChrome(); InvalidateRect(gMain, nullptr, FALSE); }

ITaskbarList3* taskbar = nullptr;
UINT WM_TASKBARCREATED = 0, WM_TASKBARBUTTON = 0;
NOTIFYICONDATAW nid{};
bool trayAdded = false, reallyQuit = false;
uint64_t lastTrayTip = 0;

void updateTray(bool force = false) {
    if (!trayAdded) return;
    uint64_t now = nowMs();
    if (!force && now - lastTrayTip < 1000) return;
    lastTrayTip = now;
    std::wstring tip = AppName;
    if (activeCount) tip += L"  ↓ " + (totalDown >= 1 ? fmtSpeed(totalDown) : std::wstring(L"0 B/s")) + L"  ·  " + fmtNum(activeCount) + L" active";
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy(nid.szTip, tip.c_str(), 127);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
void setTray(bool on) {
    if (on == trayAdded) return;
    if (on) {
        nid = NOTIFYICONDATAW{};
        nid.cbSize = sizeof nid; nid.hWnd = gMain; nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP; nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = (HICON)LoadImageW(gInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (!nid.hIcon) nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcsncpy(nid.szTip, AppName, 127);
        trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != 0;
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayAdded = false;
    }
}
void balloon(const std::wstring& title, const std::wstring& text) {
    if (!trayAdded) {
        if (GetForegroundWindow() != gMain) { FLASHWINFO fw{sizeof fw, gMain, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0}; FlashWindowEx(&fw); }
        return;
    }
    nid.uFlags = NIF_INFO;
    wcsncpy(nid.szInfoTitle, title.c_str(), 63); nid.szInfoTitle[63] = 0;
    wcsncpy(nid.szInfo, text.c_str(), 255); nid.szInfo[255] = 0;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
void showMain() {
    if (!IsWindowVisible(gMain)) ShowWindow(gMain, SW_SHOW);
    if (IsIconic(gMain)) ShowWindow(gMain, SW_RESTORE);
    SetForegroundWindow(gMain);
}

void updateTaskbar() {
    if (!taskbar) return;
    int64_t done = 0, size = 0; bool any = false, unknown = false;
    for (auto& v : views) {
        if (!isActive(v)) continue;
        any = true;
        if (v.size > 0) { done += v.done; size += v.size; } else unknown = true;
    }
    if (!any) taskbar->SetProgressState(gMain, TBPF_NOPROGRESS);
    else if (size <= 0 && unknown) taskbar->SetProgressState(gMain, TBPF_INDETERMINATE);
    else { taskbar->SetProgressState(gMain, TBPF_NORMAL); taskbar->SetProgressValue(gMain, (ULONGLONG)done, (ULONGLONG)std::max<int64_t>(1, size)); }
}

void refreshModel() {
    views = E.snapshot(focusId != 0, focusId);
    totalDown = totalUp = 0; activeCount = 0;
    for (auto& v : views) { totalDown += v.speed; totalUp += v.upSpeed; if (isActive(v)) ++activeCount; }
    rebuildRows();
    if (gList) { updateListScroll(); InvalidateRect(gList, nullptr, FALSE); InvalidateRect(gDetails, nullptr, FALSE); }
    if (gMain) refreshChrome();
    updateTaskbar();
    updateTray();
    SetThreadExecutionState(ES_CONTINUOUS | (activeCount ? ES_SYSTEM_REQUIRED : 0));
}
void selectionChanged() { detailScroll = 0; refreshModel(); }
void selectOnly(uint32_t id) {
    selected.clear();
    if (id) selected.insert(id);
    focusId = anchorId = id;
    if (id && !viewById(id)) { filter = FAll; searchText.clear(); if (gSearch) { editGuard = true; SetWindowTextW(gSearch, L""); editGuard = false; } }
    selectionChanged();
    ensureRowVisible(rowOf(id));
}

std::vector<std::wstring> pendingArgs;
bool pendingSilent = false;
void runPendingArgs();

void openAddUrl(const std::string& initial) {
    if (dialogOpen) return;
    AddUrlDialog d; d.initial = initial;
    dialogOpen = true;
    runModal(d, L"Add download", 560, 450);
    dialogOpen = false;
    if (!d.added.empty()) selectOnly(d.added.back());
    runPendingArgs();
}
void openTorrentFile(const std::wstring& path) {
    if (dialogOpen) return;
    std::string data, err;
    if (!engine::readWholeFile(path, data, 64u << 20)) { showMessage(AppTitle, L"Cannot read " + path + L"\n" + errorText(GetLastError())); return; }
    AddTorrentDialog d;
    d.sourcePath = path; d.data = data;
    if (!bt::parseTorrent(data, d.meta, err)) { showMessage(AppTitle, fileNameOf(path) + L": " + widen(err)); return; }
    d.vis = engine::visibleFiles(d.meta);
    d.want.assign(d.vis.size(), 1);
    dialogOpen = true;
    runModal(d, L"Add torrent", 620, 486);
    dialogOpen = false;
    if (d.added) selectOnly(d.added);
}
void addTorrentFiles() {
    auto files = pickFiles(gMain, L"Torrent files (*.torrent)\0*.torrent\0All files\0*.*\0", S.ui.lastTorrentFolder, true);
    for (auto& f : files) openTorrentFile(f);
}
// Arguments from the command line, another instance, or a drop: links, magnets and .torrent files.
void handleArgs(const std::vector<std::wstring>& args, bool silent) {
    std::string links;
    std::vector<std::wstring> torrents;
    for (auto& a : args) {
        std::string n = trim(narrow(a));
        if (n.empty()) continue;
        if (looksLikeLink(n)) { links += n + "\r\n"; continue; }
        if (extensionOf(a) == L"torrent" && fileExists(a)) torrents.push_back(a);
    }
    if (links.empty() && torrents.empty()) return;
    if (silent) {
        uint32_t last = 0; std::wstring errors;
        for (auto& l : linksIn(links)) { engine::AddRequest r; r.url = l; std::wstring e; uint32_t id = E.add(r, e); if (id) last = id; else errors += e + L"\n"; }
        for (auto& t : torrents) {
            engine::AddRequest r; std::wstring e;
            if (!engine::readWholeFile(t, r.torrentData, 64u << 20)) continue;
            uint32_t id = E.add(r, e); if (id) last = id; else errors += e + L"\n";
        }
        if (last) selectOnly(last);
        return;
    }
    if (!links.empty()) openAddUrl(links);
    for (auto& t : torrents) openTorrentFile(t);
}
void runPendingArgs() {
    if (pendingArgs.empty() || dialogOpen) return;
    auto a = pendingArgs; bool s = pendingSilent;
    pendingArgs.clear(); pendingSilent = false;
    handleArgs(a, s);
}

void openItem(const View& v) {
    std::wstring p = v.kind == engine::KindTorrent && v.files.size() > 1 ? joinPath(v.folder, v.name) : v.path();
    if (v.kind == engine::KindTorrent && v.files.size() == 1) p = joinPath(v.folder, v.files[0].path);
    bool complete = v.status == engine::Completed || v.status == engine::Seeding;
    if (complete && fileExists(p)) { ShellExecuteW(gMain, nullptr, p.c_str(), nullptr, folderOf(p).c_str(), SW_SHOWNORMAL); return; }
    std::wstring f = fileExists(p) ? folderOf(p) : v.folder;
    ShellExecuteW(gMain, nullptr, f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void openFolder(const View& v) {
    std::wstring p = v.kind == engine::KindTorrent && v.files.size() > 1 ? joinPath(v.folder, v.name) : v.kind == engine::KindTorrent && v.files.size() == 1 ? joinPath(v.folder, v.files[0].path) : v.path();
    if (!fileExists(p) && fileExists(p + L".tdm")) p += L".tdm";
    if (fileExists(p)) { std::wstring args = L"/select,\"" + p + L"\""; ShellExecuteW(gMain, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL); }
    else { makeDirs(v.folder); ShellExecuteW(gMain, nullptr, v.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
}
void deleteSelected(bool withFiles) {
    auto ids = selection();
    if (ids.empty() || dialogOpen) return;
    if (S.ui.confirmDelete || withFiles) {
        DeleteDialog d;
        const View* v = viewById(ids[0]);
        d.text = ids.size() == 1 && v ? L"Remove “" + v->name + L"” from the list?" : L"Remove " + fmtNum(ids.size()) + L" downloads from the list?";
        d.withFiles = withFiles;
        dialogOpen = true;
        runModal(d, L"Remove downloads", 440, 160);
        dialogOpen = false;
        if (!d.ok) return;
        withFiles = d.withFiles;
    }
    int r = rowOf(focusId);
    for (auto id : ids) E.remove(id, withFiles);
    selected.clear(); focusId = 0;
    refreshModel();
    // Keep a row selected near the removed ones.
    std::vector<int> left;
    for (int i = 0; i < (int)rows.size(); ++i) if (std::find(ids.begin(), ids.end(), views[rows[i]].id) == ids.end()) left.push_back(i);
    if (!left.empty()) {
        int pick = left.back();
        for (int i : left) if (i >= r) { pick = i; break; }
        selectOnly(views[rows[pick]].id);
    }
}

bool registerHandlers(std::wstring& err) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"%1\"", icon = L"\"" + std::wstring(exe) + L"\",0";
    auto set = [&](const std::wstring& key, const wchar_t* name, const std::wstring& val) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
        LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
        return r == ERROR_SUCCESS;
    };
    std::wstring base = L"Software\\Classes\\";
    bool ok = set(base + L"magnet", nullptr, L"URL:Magnet link") && set(base + L"magnet", L"URL Protocol", L"") &&
              set(base + L"magnet\\DefaultIcon", nullptr, icon) && set(base + L"magnet\\shell\\open\\command", nullptr, cmd) &&
              set(base + L"TDM.torrent", nullptr, L"BitTorrent file") && set(base + L"TDM.torrent\\DefaultIcon", nullptr, icon) &&
              set(base + L"TDM.torrent\\shell\\open\\command", nullptr, cmd) && set(base + L".torrent", nullptr, L"TDM.torrent") &&
              set(base + L".torrent\\OpenWithProgids", L"TDM.torrent", L"");
    if (!ok) { err = L"Could not write the file associations."; return false; }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

// ---------------------------------------------------------------- appearance and settings sync

bool systemDark() {
    DWORD v = 1, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    return v == 0;
}
void applyTheme() {
    HIGHCONTRASTW hc{sizeof hc};
    bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    bool dark = !highContrast && (S.ui.theme == 2 || S.ui.theme == 3 || (S.ui.theme == 0 && systemDark()));
    P = !dark ? LightPalette : S.ui.theme == 3 ? AmoledPalette : DarkPalette;
    darkmode::app(dark);
    titleBar(gMain);
    darkmode::window(gList, dark);
    darkmode::window(gDetails, dark);
    darkmode::window(gTip, dark);
    if (surfaceBrush) DeleteObject(surfaceBrush);
    if (chromeBrush) DeleteObject(chromeBrush);
    surfaceBrush = CreateSolidBrush(P.surface);
    chromeBrush = CreateSolidBrush(P.chrome);
    SetWindowPos(gMain, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    buildChrome();
    RedrawWindow(gMain, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}
void createFonts() {
    for (HFONT* f : {&uiFont, &uiBold, &uiLarge, &uiSmall, &iconFont, &iconSmall}) if (*f) { DeleteObject(*f); *f = nullptr; }
    auto make = [](int height, int weight, const wchar_t* face) {
        return CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    uiFont = make(-MulDiv(9, dpi, 72), FW_NORMAL, L"Segoe UI");
    uiBold = make(-MulDiv(9, dpi, 72), FW_SEMIBOLD, L"Segoe UI");
    uiLarge = make(-MulDiv(13, dpi, 72), FW_SEMIBOLD, L"Segoe UI");
    uiSmall = make(-MulDiv(8, dpi, 72), FW_NORMAL, L"Segoe UI");
    iconFont = make(-px(16), FW_NORMAL, iconFace.c_str());
    iconSmall = make(-px(13), FW_NORMAL, iconFace.c_str());
    if (gSearch) SendMessageW(gSearch, WM_SETFONT, (WPARAM)uiFont, TRUE);
}
void pickIconFace() {
    HDC dc = GetDC(nullptr);
    HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe Fluent Icons");
    HGDIOBJ o = SelectObject(dc, f);
    wchar_t face[LF_FACESIZE]{}; GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, o); DeleteObject(f); ReleaseDC(nullptr, dc);
    iconFace = wcscmp(face, L"Segoe Fluent Icons") == 0 ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}
void saveNow() {
    KillTimer(gMain, TimerSave);
    S.cfg = E.config();
    if (gMain) {
        WINDOWPLACEMENT wp{sizeof wp};
        if (GetWindowPlacement(gMain, &wp)) { S.ui.window = wp.rcNormalPosition; S.ui.haveWindow = true; S.ui.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (IsZoomed(gMain) != 0); }
    }
    settings::save(gSettingsPath, S);
    gSaver.save(E);
}
void scheduleSave() { if (gMain) SetTimer(gMain, TimerSave, 400, nullptr); }

// ---------------------------------------------------------------- menus and commands

void execute(int cmd);
int popup(HMENU m, POINT pt, UINT align = TPM_LEFTALIGN) {
    int r = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | align | TPM_TOPALIGN, pt.x, pt.y, gMain, nullptr);
    DestroyMenu(m);
    return r;
}
POINT below(int cmd, bool right = false) {
    for (auto& h : hits) if (h.cmd == cmd) { POINT p{right ? h.r.right : h.r.left, h.r.bottom + px(2)}; ClientToScreen(gMain, &p); return p; }
    POINT p; GetCursorPos(&p); return p;
}
void addItem(HMENU m, int id, const std::wstring& text, bool checked = false, bool enabled = true) {
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text.c_str());
}
void sep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }
const int64_t LimitSteps[] = {0, 128 << 10, 256 << 10, 512 << 10, 1 << 20, 2 << 20, 5 << 20, 10 << 20, 20 << 20, 50 << 20};
void showLimitMenu() {
    HMENU m = CreatePopupMenu();
    int64_t cur = E.config().speedLimit;
    bool matched = false;
    for (int i = 0; i < (int)(sizeof LimitSteps / sizeof LimitSteps[0]); ++i) {
        bool on = cur == LimitSteps[i]; matched |= on;
        addItem(m, CmdLimitBase + i, i == 0 ? L"Unlimited" : fmtSize(LimitSteps[i]) + L"/s", on);
    }
    if (!matched) { sep(m); addItem(m, 0, L"Custom: " + limitLabel(), true, false); }
    sep(m);
    addItem(m, CmdSettings, L"More in Settings…");
    if (int r = popup(m, below(CmdLimit))) execute(r);
}
void showThemeMenu() {
    HMENU m = CreatePopupMenu();
    const wchar_t* names[] = {L"Use system setting", L"Light", L"Dark", L"AMOLED black"};
    for (int i = 0; i < 4; ++i) addItem(m, CmdThemeBase + i, names[i], S.ui.theme == i);
    if (int r = popup(m, below(CmdTheme))) execute(r);
}
void showMoreMenu() {
    HMENU m = CreatePopupMenu();
    addItem(m, CmdResumeAll, L"Resume all");
    addItem(m, CmdPauseAll, L"Pause all");
    addItem(m, CmdRemoveCompleted, L"Clear completed", false, countFor(FCompleted) > 0);
    sep(m);
    addItem(m, CmdCreateTorrent, L"Create torrent…");
    addItem(m, CmdPaste, L"Download links from clipboard\tCtrl+V");
    sep(m);
    addItem(m, CmdToggleSidebar, L"Sidebar", S.ui.sidebar);
    addItem(m, CmdToggleDetails, L"Details pane", S.ui.details);
    sep(m);
    addItem(m, CmdDownloadsFolder, L"Open download folder");
    addItem(m, CmdSettingsFolder, L"Open settings folder");
    addItem(m, CmdRegister, L"Open magnet links and .torrent files with TDM");
    sep(m);
    addItem(m, CmdAbout, L"About TDM");
    addItem(m, CmdExit, L"Exit\tCtrl+Q");
    if (int r = popup(m, below(CmdMore, true), TPM_RIGHTALIGN)) execute(r);
}
void showItemMenu(POINT pt) {
    auto ids = selection();
    if (ids.empty()) return;
    const View* v = viewById(focusId ? focusId : ids[0]);
    bool anyRunning = false, anyStopped = false, anyHttp = false, anyTorrentIdle = false;
    for (auto id : ids) if (const View* x = viewById(id)) {
        bool run = isActive(*x) || x->status == engine::Seeding || x->status == engine::Queued;
        anyRunning |= run;
        anyStopped |= !run && (x->status != engine::Completed || x->kind == engine::KindTorrent);
        anyHttp |= x->kind == engine::KindHttp;
        anyTorrentIdle |= x->kind == engine::KindTorrent && !isActive(*x) && x->status != engine::Seeding;
    }
    HMENU m = CreatePopupMenu();
    bool done = v && (v->status == engine::Completed || v->status == engine::Seeding);
    addItem(m, CmdOpen, done ? L"Open\tEnter" : L"Open folder\tEnter", false, v != nullptr);
    addItem(m, CmdOpenFolder, L"Show in folder", false, v != nullptr);
    addItem(m, CmdCopyUrl, ids.size() == 1 && v && v->kind == engine::KindTorrent ? L"Copy magnet link" : L"Copy address");
    sep(m);
    addItem(m, CmdResume, L"Resume", false, anyStopped);
    addItem(m, CmdPause, L"Pause", false, anyRunning);
    addItem(m, CmdRestart, L"Restart from the beginning", false, anyHttp);
    addItem(m, CmdRecheck, L"Verify torrent data", false, anyTorrentIdle);
    sep(m);
    addItem(m, CmdMoveTop, L"Move to top");
    addItem(m, CmdMoveUp, L"Move up");
    addItem(m, CmdMoveDown, L"Move down");
    addItem(m, CmdMoveBottom, L"Move to bottom");
    sep(m);
    addItem(m, CmdProperties, L"Properties…", false, ids.size() == 1);
    addItem(m, CmdDelete, L"Remove…\tDel");
    addItem(m, CmdDeleteFiles, L"Remove and delete files…\tShift+Del");
    if (int r = popup(m, pt)) execute(r);
}
void showColumnMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    for (int c = 1; c < ColCount; ++c) addItem(m, CmdColumnBase + c, Columns[c].title, colWidth(c) > 0);
    sep(m);
    addItem(m, CmdColumnBase, L"Reset columns");
    if (int r = popup(m, pt)) execute(r);
}
void showTrayMenu() {
    HMENU m = CreatePopupMenu();
    addItem(m, CmdShow, L"Show TDM");
    addItem(m, CmdAddUrl, L"Add URL…");
    sep(m);
    addItem(m, CmdResumeAll, L"Resume all");
    addItem(m, CmdPauseAll, L"Pause all");
    HMENU lim = CreatePopupMenu();
    int64_t cur = E.config().speedLimit;
    for (int i = 0; i < (int)(sizeof LimitSteps / sizeof LimitSteps[0]); ++i) addItem(lim, CmdLimitBase + i, i == 0 ? L"Unlimited" : fmtSize(LimitSteps[i]) + L"/s", cur == LimitSteps[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)lim, L"Speed limit");
    sep(m);
    addItem(m, CmdExit, L"Exit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(gMain);
    int r = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, p.x, p.y, gMain, nullptr);
    DestroyMenu(m);
    PostMessageW(gMain, WM_NULL, 0, 0);
    if (r) execute(r);
}

void openSettings() {
    if (dialogOpen) return;
    SettingsDialog d; d.c = E.config(); d.u = S.ui;
    dialogOpen = true;
    runModal(d, L"Settings", 640, 456);
    dialogOpen = false;
    if (!d.ok) return;
    E.setConfig(d.c);
    S.cfg = d.c;
    S.ui.clipboard = d.u.clipboard; S.ui.tray = d.u.tray; S.ui.closeToTray = d.u.closeToTray && d.u.tray; S.ui.notify = d.u.notify;
    S.ui.confirmDelete = d.u.confirmDelete; S.ui.startMinimized = d.u.startMinimized;
    setTray(S.ui.tray);
    saveNow();
    refreshModel();
}

void execute(int cmd) {
    if (cmd >= CmdFilterBase && cmd < CmdFilterBase + 100) {
        filter = cmd - CmdFilterBase; listTop = 0;
        rebuildRows(); selectionChanged();
        return;
    }
    if (cmd >= CmdThemeBase && cmd < CmdThemeBase + 4) { S.ui.theme = cmd - CmdThemeBase; applyTheme(); scheduleSave(); return; }
    if (cmd >= CmdLimitBase && cmd < CmdLimitBase + 20) {
        engine::Config c = E.config(); c.speedLimit = LimitSteps[cmd - CmdLimitBase]; E.setConfig(c); S.cfg = c;
        scheduleSave(); refreshChrome(); return;
    }
    if (cmd >= CmdColumnBase && cmd < CmdColumnBase + ColCount) {
        int c = cmd - CmdColumnBase;
        if (c == 0) S.ui.columns.clear();
        else setColWidth(c, colWidth(c) > 0 ? 0 : Columns[c].def);
        InvalidateRect(gList, nullptr, FALSE); scheduleSave(); return;
    }
    auto ids = selection();
    switch (cmd) {
    case CmdAddUrl: {
        auto links = linksIn(clipboardText());
        std::string init;
        for (auto& l : links) init += l + "\r\n";
        openAddUrl(init);
        break;
    }
    case CmdPaste: {
        auto links = linksIn(clipboardText());
        if (links.empty()) { openAddUrl(""); break; }
        std::string init; for (auto& l : links) init += l + "\r\n";
        openAddUrl(init);
        break;
    }
    case CmdAddTorrent: addTorrentFiles(); break;
    case CmdResume: for (auto id : ids) E.resume(id); break;
    case CmdPause: for (auto id : ids) E.pause(id); break;
    case CmdToggleItem:
        for (auto id : ids) if (const View* v = viewById(id)) {
            if (isActive(*v) || v->status == engine::Seeding || v->status == engine::Queued) E.pause(id); else E.resume(id);
        }
        break;
    case CmdDelete: deleteSelected(false); break;
    case CmdDeleteFiles: deleteSelected(true); break;
    case CmdResumeAll: for (auto& v : views) if (v.status == engine::Paused || v.status == engine::Failed) E.resume(v.id, false); break;
    case CmdPauseAll: for (auto& v : views) if (isActive(v) || v.status == engine::Queued || v.status == engine::Seeding) E.pause(v.id); break;
    case CmdLimit: showLimitMenu(); break;
    case CmdSettings: openSettings(); break;
    case CmdTheme: showThemeMenu(); break;
    case CmdMore: showMoreMenu(); break;
    case CmdOpen: if (const View* v = viewById(focusId)) openItem(*v); break;
    case CmdOpenFolder: if (const View* v = viewById(focusId)) openFolder(*v); break;
    case CmdCopyUrl: {
        std::wstring s;
        for (auto id : ids) if (const View* v = viewById(id)) s += (s.empty() ? L"" : L"\r\n") + widenLoose(v->url);
        if (!s.empty()) setClipboardText(s);
        break;
    }
    case CmdRestart:
        for (auto id : ids) if (const View* v = viewById(id)) if (v->kind == engine::KindHttp) {
            if (isActive(*v)) E.pause(id);
            for (int i = 0; i < 50; ++i) { auto it = E.find(id); if (!it || !it->running) break; Sleep(40); }
            E.restart(id);
        }
        break;
    case CmdRecheck: for (auto id : ids) E.recheck(id); break;
    case CmdMoveTop: for (auto it = ids.rbegin(); it != ids.rend(); ++it) E.move(*it, -1000); break;
    case CmdMoveBottom: for (auto id : ids) E.move(id, 1000); break;
    case CmdMoveUp: for (auto id : ids) E.move(id, -1); break;
    case CmdMoveDown: for (auto it = ids.rbegin(); it != ids.rend(); ++it) E.move(*it, 1); break;
    case CmdProperties: {
        if (ids.size() != 1 || dialogOpen) break;
        PropertiesDialog d; d.id = ids[0];
        dialogOpen = true; runModal(d, L"Properties", 520, 220); dialogOpen = false;
        break;
    }
    case CmdRemoveCompleted: for (auto& v : views) if (v.status == engine::Completed) E.remove(v.id, false); break;
    case CmdCreateTorrent: {
        if (dialogOpen) break;
        CreateTorrentDialog d;
        dialogOpen = true; runModal(d, L"Create torrent", 560, 392); dialogOpen = false;
        if (d.added) selectOnly(d.added);
        break;
    }
    case CmdToggleSidebar: S.ui.sidebar = !S.ui.sidebar; refreshChrome(); scheduleSave(); break;
    case CmdToggleDetails: S.ui.details = !S.ui.details; refreshChrome(); scheduleSave(); break;
    case CmdRegister: {
        std::wstring err;
        if (registerHandlers(err)) showMessage(AppTitle, L"Magnet links and .torrent files now open in TDM.");
        else showMessage(AppTitle, err);
        break;
    }
    case CmdSettingsFolder: ShellExecuteW(gMain, nullptr, folderOf(gSettingsPath).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case CmdDownloadsFolder: { std::wstring f = E.config().folder; makeDirs(f); ShellExecuteW(gMain, nullptr, f.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break; }
    case CmdAbout:
        showMessage(L"About TDM", L"TDM Download Manager 1.0\n\nSegmented HTTP and HTTPS downloads with resume, Tor (.onion and SOCKS5) and BitTorrent "
                                  L"(magnet links, DHT, PEX, seeding).\n\nSettings: " + gSettingsPath);
        break;
    case CmdExit: reallyQuit = true; PostMessageW(gMain, WM_CLOSE, 0, 0); break;
    case CmdSelectAll: selected.clear(); for (int r : rows) selected.insert(views[r].id); if (!rows.empty() && !focusId) focusId = views[rows[0]].id; selectionChanged(); break;
    case CmdFind: if (!S.ui.sidebar) { S.ui.sidebar = true; refreshChrome(); } SetFocus(gSearch); SendMessageW(gSearch, EM_SETSEL, 0, -1); break;
    case CmdBannerAdd: { std::string u = bannerUrl; bannerUrl.clear(); KillTimer(gMain, TimerBanner); refreshChrome(); openAddUrl(u); break; }
    case CmdBannerClose: bannerUrl.clear(); KillTimer(gMain, TimerBanner); refreshChrome(); break;
    case CmdShow: showMain(); break;
    }
    if (cmd == CmdResume || cmd == CmdPause || cmd == CmdToggleItem || cmd == CmdResumeAll || cmd == CmdPauseAll || cmd == CmdRecheck || cmd == CmdRestart ||
        (cmd >= CmdMoveTop && cmd <= CmdMoveBottom) || cmd == CmdRemoveCompleted || cmd == CmdProperties)
        refreshModel();
}

void onClipboard() {
    if (ignoreClipboard) { ignoreClipboard = false; return; }
    if (!S.ui.clipboard || dialogOpen) return;
    std::string t = trim(clipboardText());
    if (t.empty() || t.size() > 4096 || t.find_first_of("\r\n ") != std::string::npos || !looksLikeLink(t) || t == lastClipboard) return;
    lastClipboard = t;
    bool want = startsWith(lower(t), "magnet:");
    if (!want) {
        net::Url u;
        if (!net::parseUrl(t, u)) return;
        std::wstring name = widenLoose(http::urlFilename(u));
        std::wstring ext = extensionOf(name);
        want = ext == L"torrent" || (!ext.empty() && engine::categoryFor(name, engine::KindHttp) != L"Other" && ext != L"html" && ext != L"htm" && ext != L"php" && ext != L"aspx");
    }
    if (!want) return;
    for (auto& v : views) if (v.url == t) return;
    bannerUrl = t;
    SetTimer(gMain, TimerBanner, 30000, nullptr);
    refreshChrome();
}

// ---------------------------------------------------------------- window procedures

LRESULT CALLBACK SearchSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { if (GetWindowTextLengthW(h)) SetWindowTextW(h, L""); else SetFocus(gList); return 0; }
        if (w == VK_DOWN || w == VK_RETURN) { SetFocus(gList); return 0; }
        if (w == 'A' && GetKeyState(VK_CONTROL) < 0) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    }
    if (m == WM_CHAR && (w == L'\r' || w == 27 || w == 1)) return 0;
    return DefSubclassProc(h, m, w, l);
}

void listClick(int r, bool ctrl, bool shift) {
    if (r < 0 || r >= (int)rows.size()) { if (!ctrl && !shift) { selected.clear(); focusId = 0; selectionChanged(); } return; }
    uint32_t id = views[rows[r]].id;
    if (shift) {
        int a = rowOf(anchorId); if (a < 0) a = r;
        if (!ctrl) selected.clear();
        for (int i = std::min(a, r); i <= std::max(a, r); ++i) selected.insert(views[rows[i]].id);
    } else if (ctrl) {
        if (selected.count(id)) selected.erase(id); else selected.insert(id);
        anchorId = id;
    } else { selected.clear(); selected.insert(id); anchorId = id; }
    focusId = id;
    selectionChanged();
}
int listRowAt(int y) { if (y < headerH()) return -2; int r = listTop + (y - headerH()) / rowH(); return r < (int)rows.size() ? r : -1; }
int headerEdgeAt(int x) {
    RECT c; GetClientRect(gList, &c);
    auto cols = colLayout(c.right);
    for (size_t i = 1; i < cols.size(); ++i) { int e = cols[i].x + cols[i].w; if (std::abs(x - e) <= px(4)) return cols[i].col; }
    return -1;
}
int headerColAt(int x) {
    RECT c; GetClientRect(gList, &c);
    auto cols = colLayout(c.right);
    for (size_t i = 0; i < cols.size(); ++i) if (x >= cols[i].x && x < cols[i].x + cols[i].w) return (int)i;
    return -1;
}

LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paintList(dc); EndPaint(h, &ps); return 0; }
    case WM_SIZE: updateListScroll(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof si}; si.fMask = SIF_ALL; GetScrollInfo(h, SB_VERT, &si);
        int t = listTop;
        switch (LOWORD(w)) {
        case SB_LINEUP: t--; break; case SB_LINEDOWN: t++; break;
        case SB_PAGEUP: t -= visibleRows(); break; case SB_PAGEDOWN: t += visibleRows(); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: t = si.nTrackPos; break;
        case SB_TOP: t = 0; break; case SB_BOTTOM: t = (int)rows.size(); break;
        }
        listTop = t; updateListScroll(); InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        static int acc = 0;
        acc += GET_WHEEL_DELTA_WPARAM(w);
        int n = acc / WHEEL_DELTA; acc %= WHEEL_DELTA;
        if (n) { listTop -= n * 3; updateListScroll(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
        if (dragCol >= 0 || (p.y < headerH() && headerEdgeAt(p.x) >= 0)) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
        break;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (dragCol >= 0) {
            setColWidth(dragCol, std::max(40, MulDiv(dragStartW + x - dragStartX, 96, dpi)));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        int r = listRowAt(y), hh = y < headerH() ? headerColAt(x) : -1;
        if (r != listHover || hh != headerHover) {
            listHover = r; headerHover = hh; InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: listHover = headerHover = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (y < headerH()) {
            int edge = headerEdgeAt(x);
            if (edge >= 0) { dragCol = edge; dragStartX = x; dragStartW = px(colWidth(edge)); SetCapture(h); return 0; }
            headerPress = headerColAt(x);
            return 0;
        }
        listClick(listRowAt(y), (w & MK_CONTROL) != 0, (w & MK_SHIFT) != 0);
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (dragCol >= 0) { dragCol = -1; ReleaseCapture(); scheduleSave(); return 0; }
        if (headerPress >= 0 && y < headerH() && headerColAt(x) == headerPress) {
            RECT c; GetClientRect(h, &c);
            int col = colLayout(c.right)[headerPress].col;
            if (S.ui.sortColumn != col) { S.ui.sortColumn = col; S.ui.sortAscending = true; }
            else if (S.ui.sortAscending) S.ui.sortAscending = false;
            else S.ui.sortColumn = -1;
            rebuildRows(); InvalidateRect(h, nullptr, FALSE); scheduleSave();
        }
        headerPress = -1;
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int r = listRowAt(GET_Y_LPARAM(l));
        if (r >= 0) execute(CmdOpen);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        SetFocus(h);
        int r = listRowAt(GET_Y_LPARAM(l));
        if (r >= 0 && !selected.count(views[rows[r]].id)) listClick(r, false, false);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}, s = p;
        ClientToScreen(h, &s);
        if (p.y < headerH()) { showColumnMenu(s); return 0; }
        if (listRowAt(p.y) >= 0) showItemMenu(s);
        return 0;
    }
    case WM_CONTEXTMENU: {
        if (l != -1) return 0;
        int r = rowOf(focusId);
        POINT s{px(40), headerH() + (std::max(0, r - listTop) + 1) * rowH()}; ClientToScreen(h, &s);
        showItemMenu(s);
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        int r = rowOf(focusId), n = (int)rows.size();
        int to = -1;
        switch (w) {
        case VK_UP: to = r < 0 ? 0 : std::max(0, r - 1); break;
        case VK_DOWN: to = r < 0 ? 0 : std::min(n - 1, r + 1); break;
        case VK_PRIOR: to = std::max(0, r - visibleRows()); break;
        case VK_NEXT: to = std::min(n - 1, std::max(0, r) + visibleRows()); break;
        case VK_HOME: to = 0; break;
        case VK_END: to = n - 1; break;
        case VK_DELETE: execute(shift ? CmdDeleteFiles : CmdDelete); return 0;
        case VK_RETURN: execute(CmdOpen); return 0;
        case VK_SPACE: execute(CmdToggleItem); return 0;
        case VK_F2: execute(CmdProperties); return 0;
        }
        if (to >= 0 && to < n) {
            if (shift) listClick(to, true, true);
            else if (ctrl) { focusId = views[rows[to]].id; selectionChanged(); }
            else listClick(to, false, false);
            ensureRowVisible(to);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_DROPFILES: return SendMessageW(gMain, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

int dhitAt(POINT p) { for (size_t i = 0; i < dhits.size(); ++i) if (PtInRect(&dhits[i].r, p)) return (int)i; return -1; }
LRESULT CALLBACK DetailsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paintDetails(dc); EndPaint(h, &ps); return 0; }
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int hh = dhitAt(p);
        if (hh >= 0 && dhits[hh].kind != 0) hh = -1;
        if (hh != detailHover) { detailHover = hh; InvalidateRect(h, nullptr, FALSE); TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0}; TrackMouseEvent(&tme); }
        return 0;
    }
    case WM_MOUSELEAVE: detailHover = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEWHEEL: {
        static int acc = 0;
        acc += GET_WHEEL_DELTA_WPARAM(w);
        int n = acc / WHEEL_DELTA; acc %= WHEEL_DELTA;
        if (n) { detailScroll = std::max(0, detailScroll - n * 3); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int i = dhitAt(p);
        if (i < 0) return 0;
        const DHit d = dhits[i];
        if (d.kind == 0) { detailTab = d.index; detailScroll = 0; InvalidateRect(h, nullptr, FALSE); return 0; }
        const View* v = viewById(focusId);
        if (!v || v->kind != engine::KindTorrent) return 0;
        std::string sel;
        bool all = true;
        for (auto& f : v->files) { sel += f.wanted ? '1' : '0'; all &= f.wanted; }
        if (d.kind == 1 && d.index >= 0 && d.index < (int)sel.size()) sel[d.index] = sel[d.index] == '1' ? '0' : '1';
        if (d.kind == 2) for (auto& ch : sel) ch = all ? '0' : '1';
        if (sel.find('1') == std::string::npos) { MessageBeep(MB_ICONWARNING); return 0; }
        E.setFileSel(focusId, sel);
        refreshModel();
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

bool splitDrag = false;
LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (WM_TASKBARBUTTON && m == WM_TASKBARBUTTON) {
        if (!taskbar && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, (void**)&taskbar))) taskbar->HrInit();
        updateTaskbar();
        return 0;
    }
    if (WM_TASKBARCREATED && m == WM_TASKBARCREATED) { if (trayAdded) { trayAdded = false; setTray(true); } return 0; }
    switch (m) {
    case WM_CREATE: {
        gMain = h;
        gTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, h, nullptr, gInst, nullptr);
        SendMessageW(gTip, TTM_SETMAXTIPWIDTH, 0, 600);
        gList = CreateWindowExW(0, ListClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, h, (HMENU)(INT_PTR)101, gInst, nullptr);
        gDetails = CreateWindowExW(0, DetailsClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)(INT_PTR)102, gInst, nullptr);
        gSearch = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)(INT_PTR)SearchEditId, gInst, nullptr);
        SendMessageW(gSearch, WM_SETFONT, (WPARAM)uiFont, TRUE);
        SendMessageW(gSearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search   (Ctrl+F)");
        SendMessageW(gSearch, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SetWindowSubclass(gSearch, SearchSubclass, 1, 0);
        DragAcceptFiles(h, TRUE);
        DragAcceptFiles(gList, TRUE);
        AddClipboardFormatListener(h);
        applyTheme();
        return 0;
    }
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) { if (S.ui.tray && trayAdded) ShowWindow(h, SW_HIDE); return 0; }
        buildChrome(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: { auto mm = (MINMAXINFO*)l; mm->ptMinTrackSize = {px(640), px(420)}; return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paintChrome(dc); EndPaint(h, &ps); return 0; }
    case WM_DPICHANGED: {
        dpi = HIWORD(w);
        createFonts();
        auto r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        refreshChrome();
        return 0;
    }
    case WM_SETFOCUS: SetFocus(gList); return 0;
    case WM_CTLCOLOREDIT: SetTextColor((HDC)w, P.text); SetBkColor((HDC)w, P.surface); return (LRESULT)surfaceBrush;
    case WM_COMMAND:
        if (LOWORD(w) == SearchEditId) {
            int code = HIWORD(w);
            if (code == EN_CHANGE && !editGuard) SetTimer(h, TimerSearch, 150, nullptr);
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) { RECT r = rcSearchBox; InflateRect(&r, 2, 2); InvalidateRect(h, &r, FALSE); }
            return 0;
        }
        execute(LOWORD(w));
        return 0;
    case WM_TIMER:
        if (w == TimerSave) saveNow();
        else if (w == TimerSearch) { KillTimer(h, TimerSearch); searchText = trimW(windowText(gSearch)); listTop = 0; rebuildRows(); selectionChanged(); }
        else if (w == TimerBanner) { KillTimer(h, TimerBanner); bannerUrl.clear(); refreshChrome(); }
        return 0;
    case WM_SETCURSOR: {
        POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
        RECT s = rcSplit; InflateRect(&s, 0, px(2));
        if ((HWND)w == h && (splitDrag || (rcSplit.bottom > rcSplit.top && PtInRect(&s, p)))) { SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return TRUE; }
        break;
    }
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (splitDrag) {
            int hgt = rcStatus.top - p.y;
            S.ui.detailsHeight = std::max(80, MulDiv(hgt, 96, dpi));
            refreshChrome();
            return 0;
        }
        int hit = hitAt(p);
        if (hit != hoverHit) {
            hoverHit = hit;
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: if (hoverHit >= 0) { hoverHit = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT s = rcSplit; InflateRect(&s, 0, px(2));
        if (rcSplit.bottom > rcSplit.top && PtInRect(&s, p)) { splitDrag = true; SetCapture(h); return 0; }
        if (PtInRect(&rcSearchBox, p)) { SetFocus(gSearch); return 0; }
        int hit = hitAt(p);
        pressHit = hit >= 0 && hits[hit].enabled ? hit : -1;
        if (pressHit >= 0) { SetCapture(h); InvalidateRect(h, &hits[hit].r, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (splitDrag) { splitDrag = false; ReleaseCapture(); scheduleSave(); return 0; }
        int hit = hitAt(p), pressed = pressHit;
        pressHit = -1;
        if (GetCapture() == h) ReleaseCapture();
        InvalidateRect(h, nullptr, FALSE);
        if (hit >= 0 && hit == pressed) execute(hits[hit].cmd);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring f(len + 1, L'\0'); DragQueryFileW(drop, i, &f[0], len + 1); f.resize(len);
            files.push_back(f);
        }
        DragFinish(drop);
        SetForegroundWindow(h);
        handleArgs(files, false);
        return 0;
    }
    case WM_COPYDATA: {
        auto cd = (COPYDATASTRUCT*)l;
        if (cd->dwData != CopyDataArgs) return FALSE;
        std::wstring all((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        size_t pos = 0;
        while (pos < all.size()) {
            size_t nl = all.find(L'\n', pos); if (nl == std::wstring::npos) nl = all.size();
            std::wstring a = all.substr(pos, nl - pos);
            if (a == L"--silent") pendingSilent = true; else if (!a.empty()) pendingArgs.push_back(a);
            pos = nl + 1;
        }
        PostMessageW(h, WM_RUNARGS, 0, 0);
        return TRUE;
    }
    case WM_RUNARGS: showMain(); runPendingArgs(); return 0;
    case WM_ENGINE: enginePosted = false; refreshModel(); return 0;
    case WM_FINISHED: {
        refreshModel();
        if (S.ui.notify) if (const View* v = viewById((uint32_t)w)) {
            bool torrent = v->kind == engine::KindTorrent;
            balloon(L"Download complete", v->name + (torrent ? L"  (" + fmtSize(v->size) + L")" : v->checkResult.empty() ? L"  (" + fmtSize(v->size) + L")" : L"\n" + v->checkResult));
        }
        return 0;
    }
    case WM_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP: case NIN_SELECT: case NIN_KEYSELECT: case NIN_BALLOONUSERCLICK: showMain(); break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: showTrayMenu(); break;
        }
        return 0;
    case WM_CLIPBOARDUPDATE: onClipboard(); return 0;
    case WM_SETTINGCHANGE:
        if (l && wcscmp((const wchar_t*)l, L"ImmersiveColorSet") == 0 && S.ui.theme == 0) applyTheme();
        return 0;
    case WM_SYSCOLORCHANGE: case WM_THEMECHANGED: applyTheme(); return 0;
    case WM_TESTQUERY:
        switch (w) {
        case 0: return (LRESULT)views.size();
        case 1: return (LRESULT)rows.size();
        case 2: return P.dark ? (S.ui.theme == 3 ? 2 : 1) : 0;
        case 3: return (LRESULT)selected.size();
        case 4: saveNow(); gSaver.save(E, true); return 1;
        case 5: return filter;
        case 6: return countFor(FCompleted);
        case 7: return activeCount;
        case 8: return dialogOpen ? 1 : 0;
        case 9: return (GetWindowLongW(gDetails, GWL_STYLE) & WS_VISIBLE) ? 1 : 0;
        case 10: return bannerUrl.empty() ? 0 : 1;
        case 11: return (LRESULT)focusId;
        case 12: return S.ui.sortColumn;
        case 13: return trayAdded ? 1 : 0;
        }
        return -1;
    case WM_QUERYENDSESSION: saveNow(); return TRUE;
    case WM_CLOSE:
        if (!reallyQuit && S.ui.tray && S.ui.closeToTray && trayAdded) { ShowWindow(h, SW_HIDE); return 0; }
        saveNow(); DestroyWindow(h); return 0;
    case WM_DESTROY:
        RemoveClipboardFormatListener(h);
        setTray(false);
        if (taskbar) { taskbar->Release(); taskbar = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- startup

std::wstring instanceKey() {
    std::wstring p = gSettingsPath;
    CharLowerBuffW(&p[0], (DWORD)p.size());
    uint32_t hsh = 2166136261u;
    for (wchar_t c : p) { hsh ^= (uint32_t)c; hsh *= 16777619u; }
    wchar_t buf[16]; swprintf(buf, 16, L"%08X", hsh);
    return buf;
}
HWND findInstance(const std::wstring& key) {
    struct Ctx { std::wstring key; HWND found; } ctx{key, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto c = (Ctx*)lp;
        wchar_t cls[64]; GetClassNameW(h, cls, 64);
        if (wcscmp(cls, MainClass) != 0) return TRUE;
        HANDLE v = GetPropW(h, L"TDM.Instance");
        wchar_t k[32]{};
        if (v) { swprintf(k, 32, L"%08X", (unsigned)(UINT_PTR)v); if (c->key == k) { c->found = h; return FALSE; } }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    gInst = inst;
    using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
    if (auto f = (SetDpiCtxFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) f((HANDLE)(INT_PTR)-4);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    bool silent = false, minimized = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--silent") { silent = true; continue; }
        if (a == L"--minimized") { minimized = true; continue; }
        if (!looksLikeLink(narrow(a))) {
            wchar_t full[32768];
            DWORD n = GetFullPathNameW(a.c_str(), 32768, full, nullptr);
            if (n && n < 32768) a = full;
        }
        args.push_back(a);
    }
    LocalFree(argv);

    gSettingsPath = settings::defaultPath();
    std::wstring key = instanceKey();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, (L"Local\\TDM-" + key).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = nullptr;
        for (int i = 0; i < 40 && !other; ++i) { other = findInstance(key); if (!other) Sleep(50); }
        if (other) {
            std::wstring payload = silent ? L"--silent\n" : L"";
            for (auto& a : args) { payload += a; payload += L'\n'; }
            DWORD pid = 0; GetWindowThreadProcessId(other, &pid); AllowSetForegroundWindow(pid);
            COPYDATASTRUCT cd{CopyDataArgs, (DWORD)(payload.size() * sizeof(wchar_t)), (void*)payload.c_str()};
            DWORD_PTR result = 0;
            SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &result);
            if (mutex) CloseHandle(mutex);
            return 0;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    settings::load(gSettingsPath, S);
    makeDirs(folderOf(gSettingsPath));
    E.cfg = S.cfg;
    E.dataDir = settings::dataDirFor(gSettingsPath);
    gSaver.path = settings::itemsPathFor(gSettingsPath);
    for (auto& it : settings::loadItems(gSaver.path)) {
        if (it->kind == engine::KindTorrent) engine::loadTorrentInfo(E, *it);
        E.restore(it);
    }
    E.notify = [] { if (gMain && !enginePosted.exchange(true)) PostMessageW(gMain, WM_ENGINE, 0, 0); };
    E.onFinished = [](uint32_t id) { if (gMain) PostMessageW(gMain, WM_FINISHED, id, 0); };
    E.save = [] { gSaver.save(E); };

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    darkmode::init();
    pickIconFace();
    { HDC screen = GetDC(nullptr); dpi = GetDeviceCaps(screen, LOGPIXELSY); ReleaseDC(nullptr, screen); }
    createFonts();
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    WM_TASKBARBUTTON = RegisterWindowMessageW(L"TaskbarButtonCreated");

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = MainClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW lc{sizeof lc};
    lc.style = CS_DBLCLKS; lc.lpfnWndProc = ListProc; lc.hInstance = inst; lc.hCursor = LoadCursor(nullptr, IDC_ARROW); lc.lpszClassName = ListClass;
    RegisterClassExW(&lc);
    WNDCLASSEXW dc2{sizeof dc2};
    dc2.lpfnWndProc = DetailsProc; dc2.hInstance = inst; dc2.hCursor = LoadCursor(nullptr, IDC_ARROW); dc2.lpszClassName = DetailsClass;
    RegisterClassExW(&dc2);
    WNDCLASSEXW dc{sizeof dc};
    dc.lpfnWndProc = DialogProc; dc.hInstance = inst; dc.hCursor = LoadCursor(nullptr, IDC_ARROW); dc.lpszClassName = DialogClass; dc.hIcon = wc.hIcon;
    RegisterClassExW(&dc);

    HWND h = CreateWindowExW(0, MainClass, AppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, px(1180), px(760), nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    SetPropW(h, L"TDM.Instance", (HANDLE)(UINT_PTR)wcstoul(key.c_str(), nullptr, 16));
    {
        UINT wdpi = 0;
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        if (auto f = (GetDpiForWindowFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")) wdpi = f(h);
        if (wdpi && (int)wdpi != dpi) { dpi = (int)wdpi; createFonts(); }
    }
    setTray(S.ui.tray);
    bool startHidden = (minimized || S.ui.startMinimized) && args.empty();
    int showCmd = show == SW_HIDE ? SW_HIDE : (show == SW_SHOWMINIMIZED || show == SW_MINIMIZE || show == SW_SHOWMINNOACTIVE) ? show : SW_SHOWNORMAL;
    if (startHidden) showCmd = trayAdded ? SW_HIDE : SW_SHOWMINNOACTIVE;
    if (S.ui.haveWindow && MonitorFromRect(&S.ui.window, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp{sizeof wp};
        wp.rcNormalPosition = S.ui.window;
        wp.showCmd = showCmd == SW_SHOWNORMAL && S.ui.maximized ? SW_SHOWMAXIMIZED : showCmd;
        SetWindowPlacement(h, &wp);
    } else ShowWindow(h, showCmd);
    UpdateWindow(h);

    E.start();
    refreshModel();
    if (!views.empty()) selectOnly(views[0].id);
    if (!args.empty()) { pendingArgs = args; pendingSilent = silent; PostMessageW(h, WM_RUNARGS, 0, 0); }

    ACCEL acc[] = {
        {FCONTROL | FVIRTKEY, 'N', CmdAddUrl}, {FCONTROL | FVIRTKEY, 'O', CmdAddTorrent}, {FCONTROL | FVIRTKEY, 'V', CmdPaste},
        {FCONTROL | FVIRTKEY, 'A', CmdSelectAll}, {FCONTROL | FVIRTKEY, 'F', CmdFind}, {FCONTROL | FVIRTKEY, VK_OEM_COMMA, CmdSettings},
        {FCONTROL | FVIRTKEY, 'Q', CmdExit}, {FCONTROL | FVIRTKEY, 'P', CmdProperties}, {FCONTROL | FVIRTKEY, 'T', CmdCreateTorrent},
        {FCONTROL | FVIRTKEY, VK_UP, CmdMoveUp}, {FCONTROL | FVIRTKEY, VK_DOWN, CmdMoveDown},
    };
    HACCEL accel = CreateAcceleratorTableW(acc, (int)(sizeof acc / sizeof acc[0]));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        bool inEdit = msg.hwnd == gSearch;
        if (!inEdit && (msg.hwnd == gMain || IsChild(gMain, msg.hwnd)) && TranslateAcceleratorW(gMain, accel, &msg)) continue;
        if (inEdit && msg.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && (msg.wParam == 'N' || msg.wParam == 'O' || msg.wParam == 'Q' || msg.wParam == VK_OEM_COMMA) && TranslateAcceleratorW(gMain, accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyAcceleratorTable(accel);
    E.shutdown();
    gSaver.save(E, true);
    settings::save(gSettingsPath, S);
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return (int)msg.wParam;
}
