// Shared helpers: Windows headers in the right order, UTF-8 conversion, small string utilities.
#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tdm {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline bool validUtf8(const std::string& s) { return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0 || s.empty(); }
// Bytes from the network or a torrent: UTF-8 when valid, otherwise the ANSI code page.
inline std::wstring widenLoose(const std::string& s) {
    if (validUtf8(s)) return widen(s);
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }
inline std::wstring lowerW(std::wstring s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}
inline std::wstring trimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] <= L' ') ++a;
    while (b > a && s[b - 1] <= L' ') --b;
    return s.substr(a, b - a);
}
inline bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
inline bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
inline uint64_t nowMs() { return GetTickCount64(); }
inline int64_t unixNow() { return (int64_t)time(nullptr); }

inline std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}
inline int hexVal(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
inline bool unhex(const std::string& s, std::string& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = hexVal(s[i]), b = hexVal(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out += char(a * 16 + b);
    }
    return true;
}
inline std::string percentDecode(const std::string& s, bool plusIsSpace = false) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) { o += char(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2])); i += 2; }
        else if (plusIsSpace && s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}
inline std::string percentEncode(const std::string& s, const char* keep = "-_.~") {
    static const char* d = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(keep, c)) o += (char)c;
        else { o += '%'; o += d[c >> 4]; o += d[c & 15]; }
    }
    return o;
}
inline std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == in.size()) { uint32_t v = (uint8_t)in[i] << 16; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "=="; }
    else if (i + 2 == in.size()) { uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '='; }
    return o;
}
// RFC 4648 base32 (used by magnet links with 32-character info hashes).
inline bool base32Decode(const std::string& s, std::string& out) {
    out.clear();
    uint32_t buf = 0; int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a'; else if (c >= '2' && c <= '7') v = c - '2' + 26; else if (c == '=') break; else return false;
        buf = (buf << 5) | (uint32_t)v; bits += 5;
        if (bits >= 8) { out += char((buf >> (bits - 8)) & 0xFF); bits -= 8; }
    }
    return true;
}

// Cryptographically random bytes (peer ids, SOCKS isolation tokens, DHT ids).
inline void randomBytes(void* p, size_t n) {
    using RtlGenRandomFn = BOOLEAN(WINAPI*)(PVOID, ULONG);
    static RtlGenRandomFn f = (RtlGenRandomFn)(void*)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "SystemFunction036");
    if (f && f(p, (ULONG)n)) return;
    auto* b = (uint8_t*)p;
    static std::atomic<uint64_t> x{nowMs() * 6364136223846793005ull + 1};
    for (size_t i = 0; i < n; ++i) { uint64_t v = x.fetch_add(0x9E3779B97F4A7C15ull); v ^= v >> 29; v *= 0xBF58476D1CE4E5B9ull; b[i] = (uint8_t)(v >> 56); }
}
inline std::string randomToken(size_t n) {
    std::string r(n, '\0'); randomBytes(&r[0], n);
    return hex((const uint8_t*)r.data(), r.size());
}

// Sleeps up to ms, waking early (returning false) when the cancel flag is set.
inline bool sleepFor(int ms, const std::atomic<bool>* cancel) {
    uint64_t end = nowMs() + (uint64_t)ms;
    while (nowMs() < end) {
        if (cancel && *cancel) return false;
        Sleep((DWORD)std::min<uint64_t>(50, end - nowMs()));
    }
    return !(cancel && *cancel);
}

inline std::wstring errorText(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"Error " + std::to_wstring(e);
    LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

// Removes characters Windows does not allow in file names and trims trailing dots and spaces.
inline std::wstring sanitizeFileName(std::wstring n) {
    for (auto& c : n) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    while (!n.empty() && (n.back() == L'.' || n.back() == L' ')) n.pop_back();
    while (!n.empty() && n.front() == L' ') n.erase(n.begin());
    static const wchar_t* reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    std::wstring stem = n.substr(0, n.find(L'.'));
    for (auto r : reserved) if (_wcsicmp(stem.c_str(), r) == 0) { n = L"_" + n; break; }
    if (n.size() > 200) {
        size_t dot = n.find_last_of(L'.');
        std::wstring ext = dot != std::wstring::npos && n.size() - dot <= 16 ? n.substr(dot) : L"";
        n = n.substr(0, 200 - ext.size()) + ext;
    }
    return n.empty() ? L"download" : n;
}
inline bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
inline std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}
inline std::wstring fileNameOf(const std::wstring& p) { size_t s = p.find_last_of(L"\\/"); return s == std::wstring::npos ? p : p.substr(s + 1); }
inline std::wstring folderOf(const std::wstring& p) { size_t s = p.find_last_of(L"\\/"); return s == std::wstring::npos ? std::wstring() : p.substr(0, s); }
inline std::wstring extensionOf(const std::wstring& n) { size_t d = n.find_last_of(L'.'); return d == std::wstring::npos || d == 0 ? L"" : lowerW(n.substr(d + 1)); }
// "name.ext" -> "name (2).ext" until no file (or reserved name) exists.
inline std::wstring uniquePath(const std::wstring& folder, const std::wstring& name, const std::function<bool(const std::wstring&)>& taken = nullptr) {
    std::wstring p = joinPath(folder, name);
    if (!fileExists(p) && !fileExists(p + L".tdm") && !(taken && taken(p))) return p;
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot), ext = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    for (int i = 2; i < 10000; ++i) {
        p = joinPath(folder, stem + L" (" + std::to_wstring(i) + L")" + ext);
        if (!fileExists(p) && !fileExists(p + L".tdm") && !(taken && taken(p))) return p;
    }
    return p;
}
inline bool makeDirs(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring parent = folderOf(path);
    if (!parent.empty() && parent != path && !(parent.size() == 2 && parent[1] == L':')) makeDirs(parent);
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace tdm
