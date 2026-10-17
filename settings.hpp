// TDM settings and download list, stored as UTF-8 text files (not the registry):
//   %APPDATA%\TDM\settings.ini   (or the path in the TDM_SETTINGS environment variable)
//   downloads.ini and torrents\  next to it.
// Passwords and cookies are encrypted for the current Windows user with DPAPI.
#pragma once
#include "torrent_job.hpp"
#include <shlobj.h>
#include <wincrypt.h>
#include <cstdio>

namespace settings {
using namespace tdm;

struct Ui {
    int theme = 0;                    // 0 system, 1 light, 2 dark, 3 AMOLED black
    bool haveWindow = false, maximized = false;
    RECT window{};
    bool sidebar = true, details = true;
    int detailsHeight = 230;          // DIPs
    std::vector<int> columns;         // widths in DIPs (0 = hidden), in column order
    int sortColumn = -1;              // -1 = queue order
    bool sortAscending = true;
    bool clipboard = true;            // offer to download links copied to the clipboard
    bool tray = true;                 // keep running in the notification area when minimised
    bool closeToTray = false;
    bool notify = true;               // balloon when a download completes
    bool confirmDelete = true;
    bool startMinimized = false;
    std::wstring lastTorrentFolder;
    std::vector<std::wstring> recentFolders;
};
struct Settings {
    engine::Config cfg;
    Ui ui;
};

inline std::wstring defaultPath() {
    wchar_t env[32768];
    DWORD n = GetEnvironmentVariableW(L"TDM_SETTINGS", env, 32768);
    if (n && n < 32768) return env;
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = appData;
    CoTaskMemFree(appData);
    if (dir.empty()) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); dir = tmp; }
    return dir + L"\\TDM\\settings.ini";
}
inline std::wstring defaultDownloadFolder() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    if (dir.empty()) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); dir = tmp; }
    return dir;
}
inline std::wstring dataDirFor(const std::wstring& settingsPath) { return folderOf(settingsPath); }
inline std::wstring itemsPathFor(const std::wstring& settingsPath) { return joinPath(folderOf(settingsPath), L"downloads.ini"); }

// ---------------------------------------------------------------- encoding

// '%', '|', control characters are written as %XX so values stay on one line and paths stay readable.
inline std::string escape(const std::string& s) {
    std::string o;
    static const char* hx = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (c == '%' || c == '|' || c < 0x20) { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
        else o += (char)c;
    }
    return o;
}
inline std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) { o += (char)(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2])); i += 2; }
        else o += s[i];
    }
    return o;
}
inline std::string escW(const std::wstring& w) { return escape(narrow(w)); }
inline std::wstring unescW(const std::string& s) { return widenLoose(unescape(s)); }
inline std::vector<std::string> splitBar(const std::string& s) {
    std::vector<std::string> parts(1);
    for (char c : s) { if (c == '|') parts.emplace_back(); else parts.back() += c; }
    for (auto& p : parts) p = unescape(p);
    if (parts.size() == 1 && parts[0].empty()) parts.clear();
    return parts;
}
inline std::string joinBar(const std::vector<std::string>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += '|'; o += escape(v[i]); }
    return o;
}
inline int64_t toI64(const std::string& s, int64_t def = 0) { if (s.empty()) return def; char* e; long long v = strtoll(s.c_str(), &e, 10); return e == s.c_str() ? def : (int64_t)v; }
inline int toInt(const std::string& s, int def = 0) { return (int)toI64(s, def); }
inline int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// DPAPI, bound to the Windows user: "dp:" + hex.
inline std::string protect(const std::string& s) {
    if (s.empty()) return s;
    DATA_BLOB in{(DWORD)s.size(), (BYTE*)s.data()}, out{};
    if (!CryptProtectData(&in, L"TDM", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::string();
    std::string r = "dp:" + hex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}
inline std::string unprotect(const std::string& s) {
    if (!startsWith(s, "dp:")) return s;
    std::string raw;
    if (!unhex(s.substr(3), raw)) return std::string();
    DATA_BLOB in{(DWORD)raw.size(), (BYTE*)raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::string();
    std::string r((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}

// ---------------------------------------------------------------- INI reading

struct IniLine { std::string section, key, value; };
inline bool readIni(const std::wstring& path, std::vector<IniLine>& out) {
    std::string raw;
    if (!engine::readWholeFile(path, raw, 256u << 20)) return false;
    if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
    std::string section;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? raw.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t rb = line.find(']');
            section = line.substr(1, rb == std::string::npos ? std::string::npos : rb - 1);
            out.push_back({section, "", ""}); // section marker (lets [item] blocks start new records)
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.push_back({section, line.substr(0, eq), line.substr(eq + 1)});
    }
    return true;
}
inline bool writeText(const std::wstring& path, const std::string& data) { return engine::writeWholeFile(path, data); }

// ---------------------------------------------------------------- settings.ini

inline void defaults(Settings& s) {
    s = Settings();
    s.cfg.folder = defaultDownloadFolder();
}

inline bool load(const std::wstring& path, Settings& s) {
    defaults(s);
    std::vector<IniLine> lines;
    if (!readIni(path, lines)) return false;
    engine::Config& c = s.cfg;
    Ui& u = s.ui;
    for (auto& l : lines) {
        const std::string& k = l.key;
        const std::string& v = l.value;
        if (k.empty()) continue;
        if (l.section == "downloads") {
            if (k == "folder") { std::wstring f = unescW(v); if (!f.empty()) c.folder = f; }
            else if (k == "maxActive") c.maxActive = clampInt(toInt(v, 4), 1, 50);
            else if (k == "connections") c.connections = clampInt(toInt(v, 8), 1, 32);
            else if (k == "speedLimit") c.speedLimit = std::max<int64_t>(0, toI64(v));
            else if (k == "retries") c.retries = clampInt(toInt(v, 12), 0, 1000);
            else if (k == "timeout") c.timeoutSec = clampInt(toInt(v, 30), 5, 600);
            else if (k == "minSplitKB") c.minSplit = (int64_t)clampInt(toInt(v, 256), 16, 65536) * 1024;
            else if (k == "userAgent") { std::string ua = unescape(v); if (!ua.empty()) c.userAgent = ua; }
            else if (k == "verifyTls") c.verifyTls = v != "0";
            else if (k == "preallocate") c.preallocate = v != "0";
            else if (k == "categoryFolders") c.categoryFolders = v == "1";
            else if (k == "proxy") {
                auto p = splitBar(v);
                if (p.size() >= 3) {
                    c.proxy.kind = clampInt(toInt(p[0]), 0, 2);
                    c.proxy.host = p[1]; c.proxy.port = clampInt(toInt(p[2]), 0, 65535);
                    if (p.size() >= 5) { c.proxy.user = p[3]; c.proxy.pass = unprotect(p[4]); }
                }
            }
        } else if (l.section == "tor") {
            if (k == "host") { std::string h = unescape(v); if (!h.empty()) c.torHost = h; }
            else if (k == "port") c.torPort = clampInt(toInt(v), 0, 65535);
            else if (k == "launch") c.torLaunch = v == "1";
            else if (k == "path") c.torPath = unescW(v);
            else if (k == "connections") c.torConnections = clampInt(toInt(v, 4), 1, 16);
            else if (k == "routeAll") c.torAll = v == "1";
            else if (k == "isolate") c.torIsolate = v != "0";
        } else if (l.section == "bittorrent") {
            if (k == "port") c.btPort = clampInt(toInt(v, 6881), 1024, 65535);
            else if (k == "maxPeers") c.btMaxPeers = clampInt(toInt(v, 50), 5, 500);
            else if (k == "dht") c.btDht = v != "0";
            else if (k == "seedRatio") { double r = atof(v.c_str()); c.seedRatio = r < 0 ? 0 : r > 1000 ? 1000 : r; }
            else if (k == "uploadLimit") c.uploadLimit = std::max<int64_t>(0, toI64(v));
            else if (k == "viaTor") c.btViaTor = v == "1";
        } else if (l.section == "ui") {
            if (k == "theme") u.theme = clampInt(toInt(v), 0, 3);
            else if (k == "window") {
                auto p = splitBar(v);
                if (p.size() == 5) {
                    u.window = {toInt(p[0]), toInt(p[1]), toInt(p[2]), toInt(p[3])};
                    u.maximized = p[4] == "1";
                    u.haveWindow = u.window.right > u.window.left && u.window.bottom > u.window.top;
                }
            }
            else if (k == "sidebar") u.sidebar = v != "0";
            else if (k == "details") u.details = v != "0";
            else if (k == "detailsHeight") u.detailsHeight = clampInt(toInt(v, 230), 80, 2000);
            else if (k == "columns") { u.columns.clear(); for (auto& x : splitBar(v)) u.columns.push_back(clampInt(toInt(x), 0, 2000)); }
            else if (k == "sort") { auto p = splitBar(v); if (p.size() == 2) { u.sortColumn = clampInt(toInt(p[0], -1), -1, 50); u.sortAscending = p[1] != "0"; } }
            else if (k == "clipboard") u.clipboard = v != "0";
            else if (k == "tray") u.tray = v != "0";
            else if (k == "closeToTray") u.closeToTray = v == "1";
            else if (k == "notify") u.notify = v != "0";
            else if (k == "confirmDelete") u.confirmDelete = v != "0";
            else if (k == "startMinimized") u.startMinimized = v == "1";
            else if (k == "lastTorrentFolder") u.lastTorrentFolder = unescW(v);
            else if (k == "recentFolder") { if (u.recentFolders.size() < 10) u.recentFolders.push_back(unescW(v)); }
        }
    }
    return true;
}

inline bool save(const std::wstring& path, const Settings& s) {
    const engine::Config& c = s.cfg;
    const Ui& u = s.ui;
    auto b = [](bool v) { return v ? std::string("1") : std::string("0"); };
    auto n = [](int64_t v) { return std::to_string((long long)v); };
    std::string o = "; TDM settings. Safe to edit while TDM is closed.\r\n";
    o += "[downloads]\r\n";
    o += "folder=" + escW(c.folder) + "\r\n";
    o += "maxActive=" + n(c.maxActive) + "\r\n";
    o += "connections=" + n(c.connections) + "\r\n";
    o += "speedLimit=" + n(c.speedLimit) + "\r\n";
    o += "retries=" + n(c.retries) + "\r\n";
    o += "timeout=" + n(c.timeoutSec) + "\r\n";
    o += "minSplitKB=" + n(c.minSplit / 1024) + "\r\n";
    o += "userAgent=" + escape(c.userAgent) + "\r\n";
    o += "verifyTls=" + b(c.verifyTls) + "\r\n";
    o += "preallocate=" + b(c.preallocate) + "\r\n";
    o += "categoryFolders=" + b(c.categoryFolders) + "\r\n";
    o += "proxy=" + joinBar({n(c.proxy.kind), c.proxy.host, n(c.proxy.port), c.proxy.user, protect(c.proxy.pass)}) + "\r\n";
    o += "[tor]\r\n";
    o += "host=" + escape(c.torHost) + "\r\n";
    o += "port=" + n(c.torPort) + "\r\n";
    o += "launch=" + b(c.torLaunch) + "\r\n";
    o += "path=" + escW(c.torPath) + "\r\n";
    o += "connections=" + n(c.torConnections) + "\r\n";
    o += "routeAll=" + b(c.torAll) + "\r\n";
    o += "isolate=" + b(c.torIsolate) + "\r\n";
    o += "[bittorrent]\r\n";
    o += "port=" + n(c.btPort) + "\r\n";
    o += "maxPeers=" + n(c.btMaxPeers) + "\r\n";
    o += "dht=" + b(c.btDht) + "\r\n";
    char ratio[32]; snprintf(ratio, sizeof ratio, "%.2f", c.seedRatio);
    o += std::string("seedRatio=") + ratio + "\r\n";
    o += "uploadLimit=" + n(c.uploadLimit) + "\r\n";
    o += "viaTor=" + b(c.btViaTor) + "\r\n";
    o += "[ui]\r\n";
    o += "theme=" + n(u.theme) + "\r\n";
    if (u.haveWindow) o += "window=" + joinBar({n(u.window.left), n(u.window.top), n(u.window.right), n(u.window.bottom), b(u.maximized)}) + "\r\n";
    o += "sidebar=" + b(u.sidebar) + "\r\n";
    o += "details=" + b(u.details) + "\r\n";
    o += "detailsHeight=" + n(u.detailsHeight) + "\r\n";
    if (!u.columns.empty()) { std::vector<std::string> w; for (int x : u.columns) w.push_back(n(x)); o += "columns=" + joinBar(w) + "\r\n"; }
    o += "sort=" + n(u.sortColumn) + "|" + b(u.sortAscending) + "\r\n";
    o += "clipboard=" + b(u.clipboard) + "\r\n";
    o += "tray=" + b(u.tray) + "\r\n";
    o += "closeToTray=" + b(u.closeToTray) + "\r\n";
    o += "notify=" + b(u.notify) + "\r\n";
    o += "confirmDelete=" + b(u.confirmDelete) + "\r\n";
    o += "startMinimized=" + b(u.startMinimized) + "\r\n";
    o += "lastTorrentFolder=" + escW(u.lastTorrentFolder) + "\r\n";
    for (auto& f : u.recentFolders) o += "recentFolder=" + escW(f) + "\r\n";
    return writeText(path, o);
}

// ---------------------------------------------------------------- downloads.ini

inline std::string itemText(engine::Item& it) {
    auto n = [](int64_t v) { return std::to_string((long long)v); };
    std::lock_guard<std::mutex> lk(it.m);
    it.dirty = false;
    std::string o = "[item]\r\n";
    auto kv = [&](const char* k, const std::string& v) { if (!v.empty()) o += std::string(k) + "=" + v + "\r\n"; };
    kv("id", n(it.id));
    kv("kind", n(it.kind));
    kv("url", escape(it.url));
    kv("name", escW(it.name));
    kv("folder", escW(it.folder));
    if (it.userName) kv("userName", "1");
    kv("referer", escape(it.referer));
    kv("cookie", escape(protect(it.cookie)));
    kv("user", escape(it.user));
    kv("pass", escape(protect(it.pass)));
    kv("size", n(it.size));
    kv("done", n(it.done));
    if (it.resumable) kv("resumable", "1");
    kv("etag", escape(it.etag));
    kv("lastModified", escape(it.lastModified));
    kv("status", n(it.status));
    kv("error", escW(it.error));
    if (it.connections) kv("connections", n(it.connections));
    if (it.tor) kv("tor", n(it.tor));
    kv("added", n(it.added));
    if (it.finished) kv("finished", n(it.finished));
    if (it.limit) kv("limit", n(it.limit));
    kv("category", escW(it.category));
    kv("checksum", escape(it.checksum));
    kv("checkResult", escW(it.checkResult));
    kv("finalUrl", escape(it.finalUrl));
    if (!it.segs.empty()) {
        std::string s;
        for (auto& g : it.segs) { if (!s.empty()) s += ';'; s += n(g.start) + "," + n(g.end) + "," + n(g.pos); }
        kv("segs", s);
    }
    kv("infoHash", it.infoHash);
    kv("have", it.have);
    kv("fileSel", it.fileSel);
    if (it.uploaded) kv("uploaded", n(it.uploaded));
    kv("trackers", joinBar(it.trackers));
    return o;
}

inline std::shared_ptr<engine::Item> itemFrom(const std::vector<std::pair<std::string, std::string>>& f) {
    auto it = std::make_shared<engine::Item>();
    for (auto& p : f) {
        const std::string& k = p.first;
        const std::string& v = p.second;
        if (k == "id") it->id = (uint32_t)toI64(v);
        else if (k == "kind") it->kind = clampInt(toInt(v), 0, 1);
        else if (k == "url") it->url = unescape(v);
        else if (k == "name") it->name = unescW(v);
        else if (k == "folder") it->folder = unescW(v);
        else if (k == "userName") it->userName = v == "1";
        else if (k == "referer") it->referer = unescape(v);
        else if (k == "cookie") it->cookie = unprotect(unescape(v));
        else if (k == "user") it->user = unescape(v);
        else if (k == "pass") it->pass = unprotect(unescape(v));
        else if (k == "size") it->size = toI64(v, -1);
        else if (k == "done") it->done = std::max<int64_t>(0, toI64(v));
        else if (k == "resumable") it->resumable = v == "1";
        else if (k == "etag") it->etag = unescape(v);
        else if (k == "lastModified") it->lastModified = unescape(v);
        else if (k == "status") it->status = clampInt(toInt(v), 0, engine::StatusCount - 1);
        else if (k == "error") it->error = unescW(v);
        else if (k == "connections") it->connections = clampInt(toInt(v), 0, 32);
        else if (k == "tor") it->tor = clampInt(toInt(v), 0, 2);
        else if (k == "added") it->added = toI64(v);
        else if (k == "finished") it->finished = toI64(v);
        else if (k == "limit") it->limit = std::max<int64_t>(0, toI64(v));
        else if (k == "category") it->category = unescW(v);
        else if (k == "checksum") it->checksum = unescape(v);
        else if (k == "checkResult") it->checkResult = unescW(v);
        else if (k == "finalUrl") it->finalUrl = unescape(v);
        else if (k == "segs") {
            size_t i = 0;
            while (i < v.size()) {
                size_t semi = v.find(';', i);
                std::string g = v.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
                i = semi == std::string::npos ? v.size() : semi + 1;
                engine::Segment s;
                long long a = 0, b2 = 0, c = 0;
                if (sscanf(g.c_str(), "%lld,%lld,%lld", &a, &b2, &c) == 3 && a >= 0 && b2 >= a && c >= a && c <= b2) { s.start = a; s.end = b2; s.pos = c; it->segs.push_back(s); }
            }
        }
        else if (k == "infoHash") it->infoHash = lower(v);
        else if (k == "have") it->have = v;
        else if (k == "fileSel") it->fileSel = v;
        else if (k == "uploaded") it->uploaded = std::max<int64_t>(0, toI64(v));
        else if (k == "trackers") it->trackers = splitBar(v);
    }
    if (it->kind == engine::KindTorrent && it->infoHash.size() != 40) return nullptr;
    if (it->kind == engine::KindHttp && it->url.empty()) return nullptr;
    // Keep the segment list consistent with the byte count.
    if (it->kind == engine::KindHttp && !it->segs.empty()) { int64_t d = 0; for (auto& s : it->segs) d += s.pos - s.start; it->done = d; }
    it->dirty = false;
    return it;
}

inline std::vector<std::shared_ptr<engine::Item>> loadItems(const std::wstring& path) {
    std::vector<std::shared_ptr<engine::Item>> out;
    std::vector<IniLine> lines;
    if (!readIni(path, lines)) return out;
    std::vector<std::pair<std::string, std::string>> cur;
    bool in = false;
    auto flush = [&] { if (in) { auto it = itemFrom(cur); if (it) out.push_back(it); } cur.clear(); };
    for (auto& l : lines) {
        if (l.key.empty()) { flush(); in = l.section == "item"; continue; }
        if (in) cur.push_back({l.key, l.value});
    }
    flush();
    // Ids must be unique.
    std::set<uint32_t> seen;
    uint32_t next = 1;
    for (auto& it : out) if (it->id >= next) next = it->id + 1;
    for (auto& it : out) { if (it->id == 0 || !seen.insert(it->id).second) it->id = next++; seen.insert(it->id); }
    return out;
}

// Saves the list when something changed (an item is dirty, or items were added/removed).
class ItemSaver {
    std::mutex m;
    std::string lastIds;
public:
    std::wstring path;
    bool save(engine::Engine& e, bool force = false) {
        std::lock_guard<std::mutex> lk(m);
        std::vector<std::shared_ptr<engine::Item>> all;
        { std::lock_guard<std::mutex> l2(e.m); all = e.items; }
        std::string ids;
        bool dirty = force;
        for (auto& it : all) {
            if (it->removed) continue;
            ids += std::to_string(it->id) + ",";
            std::lock_guard<std::mutex> l3(it->m);
            if (it->dirty) dirty = true;
        }
        if (ids != lastIds) dirty = true;
        if (!dirty) return true;
        std::string o = "; TDM download list. Safe to edit while TDM is closed.\r\n";
        for (auto& it : all) if (!it->removed) o += itemText(*it);
        lastIds = ids;
        return writeText(path, o);
    }
};

} // namespace settings
