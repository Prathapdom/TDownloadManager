// Hidden-window integration test: launches TDM.exe with an isolated settings file, feeds it links through a
// second instance (as a browser or the shell would), and drives it with WM_COMMAND / WM_TESTQUERY.
#ifndef UNICODE
#define UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

constexpr UINT WM_TESTQUERY = WM_APP + 100;
enum { QItems, QRows, QTheme, QSelected, QSave, QFilter, QCompleted, QActive, QDialog, QDetails, QBanner, QFocus, QSort, QTray };
enum {
    CmdDelete = 104, CmdPauseAll = 107, CmdResumeAll = 106, CmdToggleDetails = 125, CmdSelectAll = 131, CmdBannerClose = 135,
    CmdFilterBase = 1000, CmdThemeBase = 3100, CmdLimitBase = 3200,
};
enum { FAll = 0, FActive, FQueued, FPaused, FCompleted, FFailed, FTorrents, FTor };

DWORD targetPid = 0;
HWND app = nullptr;
std::wstring root;
static int checks = 0;

void require(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
BOOL CALLBACK findWindow(HWND h, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (pid == targetPid && wcscmp(cls, L"TDM.Main") == 0) { app = h; return FALSE; }
    return TRUE;
}
LRESULT q(int what) { return SendMessageW(app, WM_TESTQUERY, what, 0); }
void cmd(int id) { SendMessageW(app, WM_COMMAND, MAKEWPARAM(id, 0), 0); }
bool waitFor(const std::function<bool()>& f, DWORD timeout = 10000) {
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeout) { if (f()) return true; Sleep(25); }
    return f();
}
std::string readFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string s(4 << 20, '\0'); DWORD n = 0; ReadFile(h, &s[0], (DWORD)s.size(), &n, nullptr); CloseHandle(h);
    s.resize(n); return s;
}
void writeFile(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    require(h != INVALID_HANDLE_VALUE, "write fixture");
    DWORD n; WriteFile(h, data.data(), (DWORD)data.size(), &n, nullptr); CloseHandle(h);
}
std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
PROCESS_INFORMATION launch(const std::wstring& exe, const std::wstring& args) {
    std::wstring command = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    require(CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi), "launch TDM");
    CloseHandle(pi.hThread);
    return pi;
}
void attach(const PROCESS_INFORMATION& pi) {
    targetPid = pi.dwProcessId; app = nullptr;
    for (int i = 0; i < 400 && !app; ++i) { EnumWindows(findWindow, 0); if (!app) Sleep(25); }
    require(app != nullptr, "main window created");
}
bool closeApp(PROCESS_INFORMATION& pi) {
    PostMessageW(app, WM_CLOSE, 0, 0);
    bool ok = WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0;
    if (!ok) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess); pi.hProcess = nullptr;
    return ok;
}
// A second instance hands its arguments to the running one and exits.
void forward(const std::wstring& exe, const std::wstring& args) {
    PROCESS_INFORMATION pi = launch(exe, args);
    require(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "second instance exits");
    CloseHandle(pi.hProcess);
}

// Minimal HTTP server with ranges.
struct Server {
    SOCKET ls = INVALID_SOCKET; int port = 0; std::string body;
    std::atomic<bool> quit{false};
    std::thread th;
    void start() {
        body.resize(1 << 20);
        for (size_t i = 0; i < body.size(); ++i) body[i] = (char)(i * 7);
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(ls, (sockaddr*)&a, sizeof a); listen(ls, 16);
        int len = sizeof a; getsockname(ls, (sockaddr*)&a, &len); port = ntohs(a.sin_port);
        th = std::thread([this] {
            for (;;) {
                SOCKET c = accept(ls, nullptr, nullptr);
                if (c == INVALID_SOCKET) break;
                std::thread([this, c] {
                    std::string req; char b[2048];
                    while (req.find("\r\n\r\n") == std::string::npos) { int k = recv(c, b, sizeof b, 0); if (k <= 0) { closesocket(c); return; } req.append(b, k); }
                    long long from = 0; size_t r = req.find("Range: bytes=");
                    if (r != std::string::npos) from = _atoi64(req.c_str() + r + 13);
                    std::string part = body.substr((size_t)from);
                    std::string h = r != std::string::npos
                        ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(from) + "-" + std::to_string(body.size() - 1) + "/" + std::to_string(body.size()) + "\r\n"
                        : "HTTP/1.1 200 OK\r\n";
                    h += "Accept-Ranges: bytes\r\nContent-Length: " + std::to_string(part.size()) + "\r\nConnection: close\r\n\r\n";
                    send(c, h.data(), (int)h.size(), 0);
                    for (size_t o = 0; o < part.size();) { int k = send(c, part.data() + o, (int)std::min<size_t>(65536, part.size() - o), 0); if (k <= 0) break; o += k; }
                    closesocket(c);
                }).detach();
            }
        });
    }
    void stop() { closesocket(ls); th.join(); }
};

std::wstring clipboardText() {
    std::wstring s;
    if (!OpenClipboard(nullptr)) return s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) { if (auto p = (const wchar_t*)GlobalLock(h)) { s = p; GlobalUnlock(h); } }
    CloseClipboard();
    return s;
}
void setClipboard(const std::wstring& s) {
    // SetClipboardData fails after EmptyClipboard unless the clipboard was opened with an owner window.
    static HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    for (int i = 0; i < 20 && !OpenClipboard(owner); ++i) Sleep(50);
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g);
    SetClipboardData(CF_UNICODETEXT, g);
    CloseClipboard();
}

int wmain(int argc, wchar_t** argv) {
    PROCESS_INFORMATION pi{};
    Server web;
    std::wstring savedClipboard;
    bool clipboardTouched = false;
    try {
        require(argc == 2, "pass the TDM.exe path");
        std::wstring exe = argv[1];
        WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
        web.start();
        wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH, temp);
        root = std::wstring(temp) + L"tdm-ui-" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(root.c_str(), nullptr);
        std::wstring ini = root + L"\\settings.ini", downloads = root + L"\\Downloads";
        writeFile(ini, "[downloads]\r\nfolder=" + narrow(downloads) + "\r\n[bittorrent]\r\ndht=0\r\nport=" + std::to_string(48000 + GetCurrentProcessId() % 1000) +
                       "\r\n[ui]\r\ntheme=1\r\ntray=0\r\nconfirmDelete=0\r\nnotify=0\r\nclipboard=1\r\n");
        SetEnvironmentVariableW(L"TDM_SETTINGS", ini.c_str());

        pi = launch(exe, L"");
        attach(pi);
        require(q(QItems) == 0, "starts empty");
        require(q(QTheme) == 0, "light theme from settings");
        cmd(CmdThemeBase + 2); require(q(QTheme) == 1, "dark theme");
        cmd(CmdThemeBase + 3); require(q(QTheme) == 2, "AMOLED theme");
        cmd(CmdThemeBase + 1); require(q(QTheme) == 0, "back to light");
        require(q(QDetails) == 1, "details pane visible");
        cmd(CmdToggleDetails); require(q(QDetails) == 0, "details hidden");
        cmd(CmdToggleDetails); require(q(QDetails) == 1, "details shown again");

        // Links from another process (browser, shell, magnet handler) are forwarded to the running instance.
        std::wstring base = L"http://127.0.0.1:" + std::to_wstring(web.port);
        forward(exe, L"--silent " + base + L"/files/first.bin " + base + L"/files/second.iso");
        require(waitFor([] { return q(QItems) == 2; }), "two downloads added from the second instance");
        require(waitFor([] { return q(QCompleted) == 2; }, 30000), "both downloads complete");
        std::string got = readFile(downloads + L"\\first.bin");
        require(got == web.body, "downloaded file matches");
        require(q(QFocus) != 0, "the new download is selected");

        cmd(CmdFilterBase + FCompleted); require(q(QFilter) == FCompleted && q(QRows) == 2, "completed filter");
        cmd(CmdFilterBase + FFailed); require(q(QRows) == 0, "failed filter is empty");
        cmd(CmdFilterBase + FAll); require(q(QRows) == 2, "all filter");
        cmd(CmdSelectAll); require(q(QSelected) == 2, "select all");
        cmd(CmdLimitBase + 3);

        // Copying a download link offers it in a banner.
        savedClipboard = clipboardText(); clipboardTouched = true;
        setClipboard(base + L"/files/archive.zip");
        if (clipboardText() != base + L"/files/archive.zip") {
            std::cout << "  (clipboard not available in this session; banner checks skipped)\n";
        } else {
            require(waitFor([] { return q(QBanner) == 1; }), "clipboard banner appears");
            cmd(CmdBannerClose); require(q(QBanner) == 0, "banner closes");
            setClipboard(L"just some words");
            Sleep(200); require(q(QBanner) == 0, "plain text is not offered");
        }

        require(q(QSave) == 1, "save");
        std::string settings = readFile(ini);
        require(settings.find("speedLimit=524288") != std::string::npos, "speed limit saved");
        require(settings.find("theme=1") != std::string::npos, "theme saved");
        std::string list = readFile(root + L"\\downloads.ini");
        require(list.find("first.bin") != std::string::npos && list.find("second.iso") != std::string::npos, "download list saved");
        require(closeApp(pi), "closes cleanly");

        // The list and the settings come back after a restart.
        pi = launch(exe, L"");
        attach(pi);
        require(waitFor([] { return q(QItems) == 2; }), "downloads restored");
        require(q(QCompleted) == 2, "still completed");
        cmd(CmdSelectAll);
        cmd(CmdDelete);
        require(waitFor([] { return q(QItems) == 0; }), "removed from the list");
        require(GetFileAttributesW((downloads + L"\\first.bin").c_str()) != INVALID_FILE_ATTRIBUTES, "remove keeps the files");
        require(closeApp(pi), "closes cleanly again");
        std::cout << "UI integration: " << checks << " checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "UI test failed: " << e.what() << "\n";
        if (pi.hProcess) { TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); }
        if (clipboardTouched) setClipboard(savedClipboard);
        if (web.port) web.stop();
        return 1;
    }
    if (clipboardTouched) setClipboard(savedClipboard);
    web.stop();
    SHFILEOPSTRUCTW op{}; std::wstring from = root + L'\0'; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    return 0;
}
