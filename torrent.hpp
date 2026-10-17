// BitTorrent building blocks: SHA-1, bencode, metainfo, magnet links, file storage, HTTP/UDP trackers, DHT, listener.
#pragma once
#include "http.hpp"
#include <bcrypt.h>
#include <deque>
#include <set>
#include <unordered_map>

namespace bt {
using namespace tdm;

// ---------------------------------------------------------------- SHA-1

inline BCRYPT_ALG_HANDLE sha1Alg() {
    static BCRYPT_ALG_HANDLE a = [] { BCRYPT_ALG_HANDLE h = nullptr; BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA1_ALGORITHM, nullptr, 0); return h; }();
    return a;
}
inline std::string sha1(const void* p, size_t n) {
    std::string out(20, '\0');
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(sha1Alg(), &h, nullptr, 0, nullptr, 0, 0) != 0) return out;
    const uint8_t* b = (const uint8_t*)p;
    while (n) { ULONG k = (ULONG)std::min<size_t>(n, 1u << 30); BCryptHashData(h, (PUCHAR)b, k, 0); b += k; n -= k; }
    BCryptFinishHash(h, (PUCHAR)&out[0], 20, 0);
    BCryptDestroyHash(h);
    return out;
}
inline std::string sha1(const std::string& s) { return sha1(s.data(), s.size()); }

// ---------------------------------------------------------------- bencode

struct Value {
    enum Type { None, Int, Str, List, Dict } type = None;
    int64_t i = 0;
    std::string s;
    std::vector<Value> list;
    std::vector<std::pair<std::string, Value>> dict;
    size_t rawBegin = 0, rawEnd = 0; // span in the source buffer
    const Value* get(const std::string& k) const { if (type != Dict) return nullptr; for (auto& p : dict) if (p.first == k) return &p.second; return nullptr; }
    int64_t num(const std::string& k, int64_t def = 0) const { auto v = get(k); return v && v->type == Int ? v->i : def; }
    std::string str(const std::string& k) const { auto v = get(k); return v && v->type == Str ? v->s : std::string(); }
    bool isStr() const { return type == Str; }
};

inline bool decode(const std::string& in, size_t& pos, Value& v, int depth = 0) {
    if (depth > 64 || pos >= in.size()) return false;
    v = Value();
    v.rawBegin = pos;
    char c = in[pos];
    if (c == 'i') {
        size_t e = in.find('e', pos);
        if (e == std::string::npos || e == pos + 1) return false;
        v.type = Value::Int;
        v.i = _strtoi64(in.c_str() + pos + 1, nullptr, 10);
        pos = e + 1;
    } else if (c >= '0' && c <= '9') {
        size_t colon = in.find(':', pos);
        if (colon == std::string::npos || colon - pos > 12) return false;
        int64_t len = _strtoi64(in.c_str() + pos, nullptr, 10);
        if (len < 0 || (uint64_t)len > in.size() - colon - 1) return false;
        v.type = Value::Str;
        v.s = in.substr(colon + 1, (size_t)len);
        pos = colon + 1 + (size_t)len;
    } else if (c == 'l') {
        v.type = Value::List;
        ++pos;
        while (pos < in.size() && in[pos] != 'e') {
            Value x;
            if (!decode(in, pos, x, depth + 1)) return false;
            v.list.push_back(std::move(x));
        }
        if (pos >= in.size()) return false;
        ++pos;
    } else if (c == 'd') {
        v.type = Value::Dict;
        ++pos;
        while (pos < in.size() && in[pos] != 'e') {
            Value k, x;
            if (!decode(in, pos, k, depth + 1) || k.type != Value::Str) return false;
            if (!decode(in, pos, x, depth + 1)) return false;
            v.dict.push_back({k.s, std::move(x)});
        }
        if (pos >= in.size()) return false;
        ++pos;
    } else return false;
    v.rawEnd = pos;
    return true;
}
inline bool decode(const std::string& in, Value& v) { size_t p = 0; return decode(in, p, v) && p == in.size(); }
inline bool decodePrefix(const std::string& in, Value& v, size_t& used) { used = 0; return decode(in, used, v); }

inline std::string bstr(const std::string& s) { return std::to_string(s.size()) + ":" + s; }
inline std::string bint(int64_t i) { return "i" + std::to_string(i) + "e"; }

// ---------------------------------------------------------------- metainfo

struct File { std::wstring path; int64_t length = 0, offset = 0; bool pad = false; };
struct Meta {
    std::string hash;            // 20 raw bytes (SHA-1 of the info dictionary)
    std::string info;            // raw bencoded info dictionary
    std::wstring name;
    int64_t pieceLen = 0, total = 0;
    std::string pieces;          // 20 bytes per piece
    std::vector<File> files;
    std::vector<std::string> trackers, webSeeds;
    bool priv = false, multi = false;
    std::wstring comment;
    int pieceCount() const { return (int)(pieces.size() / 20); }
    int64_t pieceSize(int i) const { return i == pieceCount() - 1 ? total - pieceLen * (int64_t)i : pieceLen; }
};

inline std::wstring cleanComponent(const std::string& raw) {
    std::wstring w = sanitizeFileName(widenLoose(raw));
    if (w == L"." || w == L"..") w = L"_";
    return w;
}

inline bool parseInfo(const std::string& info, Meta& m, std::string& err) {
    Value v;
    if (!decode(info, v) || v.type != Value::Dict) { err = "The torrent's info section is damaged"; return false; }
    m.info = info;
    m.hash = sha1(info);
    std::string nm = v.str("name.utf-8"); if (nm.empty()) nm = v.str("name");
    m.name = nm.empty() ? L"torrent" : cleanComponent(nm);
    m.pieceLen = v.num("piece length");
    m.pieces = v.str("pieces");
    m.priv = v.num("private") == 1;
    if (m.pieceLen <= 0 || m.pieces.empty() || m.pieces.size() % 20) {
        err = v.get("file tree") ? "BitTorrent v2-only torrents are not supported yet" : "The torrent has no piece hashes";
        return false;
    }
    m.files.clear(); m.total = 0; m.multi = false;
    if (auto fl = v.get("files")) {
        if (fl->type != Value::List) { err = "Bad file list"; return false; }
        m.multi = true;
        for (auto& f : fl->list) {
            File x;
            x.length = f.num("length");
            const Value* p = f.get("path.utf-8"); if (!p) p = f.get("path");
            if (!p || p->type != Value::List || x.length < 0) { err = "Bad file entry"; return false; }
            std::wstring path = m.name;
            for (auto& c : p->list) if (c.type == Value::Str && !c.s.empty()) path += L"\\" + cleanComponent(c.s);
            x.path = path;
            x.pad = f.str("attr").find('p') != std::string::npos;
            x.offset = m.total;
            m.total += x.length;
            m.files.push_back(x);
        }
    } else {
        File x; x.length = v.num("length"); x.path = m.name; x.offset = 0;
        if (x.length < 0) { err = "Bad length"; return false; }
        m.total = x.length;
        m.files.push_back(x);
    }
    int64_t need = (m.total + m.pieceLen - 1) / m.pieceLen;
    if (need != m.pieceCount()) { err = "Piece count does not match the file sizes"; return false; }
    return true;
}

inline bool parseTorrent(const std::string& data, Meta& m, std::string& err) {
    m = Meta();
    Value v;
    size_t used = 0;
    if (!decodePrefix(data, v, used) || v.type != Value::Dict) { err = "Not a valid .torrent file"; return false; }
    const Value* info = v.get("info");
    if (!info || info->type != Value::Dict) { err = "The .torrent file has no info section"; return false; }
    if (!parseInfo(data.substr(info->rawBegin, info->rawEnd - info->rawBegin), m, err)) return false;
    std::set<std::string> seen;
    auto addT = [&](const std::string& t) { if (!t.empty() && seen.insert(t).second) m.trackers.push_back(t); };
    if (auto al = v.get("announce-list")) for (auto& tier : al->list) for (auto& t : tier.list) if (t.type == Value::Str) addT(t.s);
    addT(v.str("announce"));
    if (auto ul = v.get("url-list")) { if (ul->type == Value::Str) m.webSeeds.push_back(ul->s); else for (auto& u : ul->list) if (u.type == Value::Str) m.webSeeds.push_back(u.s); }
    m.comment = widenLoose(v.str("comment"));
    return true;
}

struct Magnet { std::string hash; std::wstring name; std::vector<std::string> trackers, peers; };
inline bool parseMagnet(const std::string& uri, Magnet& m) {
    m = Magnet();
    if (!startsWith(lower(uri), "magnet:?")) return false;
    std::string q = uri.substr(8);
    size_t i = 0;
    while (i <= q.size()) {
        size_t amp = q.find('&', i);
        std::string kv = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        i = amp == std::string::npos ? q.size() + 1 : amp + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = lower(kv.substr(0, eq)), val = percentDecode(kv.substr(eq + 1), true);
        size_t dot = k.find('.');
        if (dot != std::string::npos) k = k.substr(0, dot); // tr.1, tr.2 …
        if (k == "xt" && startsWith(lower(val), "urn:btih:")) {
            std::string h = val.substr(9), raw;
            if (h.size() == 40 && unhex(h, raw)) m.hash = raw;
            else if (h.size() == 32 && base32Decode(h, raw) && raw.size() == 20) m.hash = raw;
        } else if (k == "dn") m.name = widenLoose(val);
        else if (k == "tr") m.trackers.push_back(val);
        else if (k == "x" && kv.compare(0, 5, "x.pe=") == 0) m.peers.push_back(val);
    }
    return m.hash.size() == 20;
}
inline std::string makeMagnet(const std::string& hash, const std::wstring& name, const std::vector<std::string>& trackers, const std::vector<std::string>& peers = {}) {
    std::string s = "magnet:?xt=urn:btih:" + hex((const uint8_t*)hash.data(), hash.size());
    if (!name.empty()) s += "&dn=" + percentEncode(narrow(name));
    for (auto& t : trackers) s += "&tr=" + percentEncode(t);
    for (auto& p : peers) s += "&x.pe=" + percentEncode(p, "-_.~:[]");
    return s;
}

// Builds a .torrent for a file or a folder (files sorted by path). pieceLen 0 = pick from the total size.
inline std::string createTorrent(const std::wstring& path, int64_t pieceLen, const std::vector<std::string>& trackers, bool isPrivate, std::string& err,
                                 const std::atomic<bool>* cancel = nullptr) {
    struct F { std::wstring full; std::vector<std::string> parts; int64_t len; };
    std::vector<F> files;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { err = "Path not found"; return {}; }
    bool dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::function<void(const std::wstring&, std::vector<std::string>)> walk = [&](const std::wstring& d, std::vector<std::string> rel) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(joinPath(d, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<std::pair<std::wstring, bool>> ents;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            ents.push_back({n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(ents.begin(), ents.end());
        for (auto& e : ents) {
            auto r = rel; r.push_back(narrow(e.first));
            if (e.second) walk(joinPath(d, e.first), r);
            else {
                WIN32_FILE_ATTRIBUTE_DATA a;
                if (!GetFileAttributesExW(joinPath(d, e.first).c_str(), GetFileExInfoStandard, &a)) continue;
                files.push_back({joinPath(d, e.first), r, (int64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow});
            }
        }
    };
    if (dir) walk(path, {});
    else {
        WIN32_FILE_ATTRIBUTE_DATA a;
        GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
        files.push_back({path, {}, (int64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow});
    }
    int64_t total = 0;
    for (auto& f : files) total += f.len;
    if (files.empty() || total == 0) { err = "Nothing to share (no files or all empty)"; return {}; }
    if (pieceLen <= 0) { pieceLen = 16384; while (pieceLen < (16 << 20) && total / pieceLen > 1500) pieceLen *= 2; }
    std::string pieces, buf((size_t)pieceLen, char(0));
    size_t fill = 0;
    for (auto& f : files) {
        HANDLE h = CreateFileW(f.full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) { err = "Cannot read " + narrow(f.full); return {}; }
        int64_t left = f.len;
        while (left > 0) {
            if (cancel && *cancel) { CloseHandle(h); err = "Cancelled"; return {}; }
            DWORD want = (DWORD)std::min<int64_t>(left, (int64_t)buf.size() - (int64_t)fill), got = 0;
            if (!ReadFile(h, &buf[fill], want, &got, nullptr) || got == 0) { CloseHandle(h); err = "Read error in " + narrow(f.full); return {}; }
            fill += got; left -= got;
            if (fill == buf.size()) { pieces += sha1(buf.data(), fill); fill = 0; }
        }
        CloseHandle(h);
    }
    if (fill) pieces += sha1(buf.data(), fill);
    std::wstring nameW = fileNameOf(path);
    std::string info = "d";
    if (dir) {
        info += "5:filesl";
        for (auto& f : files) {
            info += "d6:lengthi" + std::to_string(f.len) + "e4:pathl";
            for (auto& p : f.parts) info += bstr(p);
            info += "ee";
        }
        info += "e";
    } else info += "6:lengthi" + std::to_string(total) + "e";
    info += "4:name" + bstr(narrow(nameW)) + "12:piece lengthi" + std::to_string(pieceLen) + "e6:pieces" + bstr(pieces);
    if (isPrivate) info += "7:privatei1e";
    info += "e";
    std::string t = "d";
    if (!trackers.empty()) {
        t += "8:announce" + bstr(trackers[0]);
        if (trackers.size() > 1) { t += "13:announce-listl"; for (auto& x : trackers) t += "l" + bstr(x) + "e"; t += "e"; }
    }
    t += "10:created by" + bstr("TDM 1.0") + "13:creation datei" + std::to_string((long long)time(nullptr)) + "e";
    t += "4:info" + info + "e";
    return t;
}

// ---------------------------------------------------------------- endpoints

struct Endpoint {
    std::string ip; int port = 0;
    std::string key() const { return (ip.find(':') != std::string::npos ? "[" + ip + "]" : ip) + ":" + std::to_string(port); }
    bool operator<(const Endpoint& o) const { return key() < o.key(); }
};
inline void parseCompact(const std::string& s, std::vector<Endpoint>& out, bool v6) {
    size_t step = v6 ? 18 : 6;
    for (size_t i = 0; i + step <= s.size(); i += step) {
        char buf[64];
        Endpoint e;
        if (v6) { inet_ntop(AF_INET6, (void*)(s.data() + i), buf, sizeof buf); }
        else { inet_ntop(AF_INET, (void*)(s.data() + i), buf, sizeof buf); }
        e.ip = buf;
        e.port = ((uint8_t)s[i + step - 2] << 8) | (uint8_t)s[i + step - 1];
        if (e.port > 0 && e.ip != "0.0.0.0") out.push_back(e);
    }
}
inline bool parseHostPort(const std::string& s, Endpoint& e) {
    std::string t = trim(s);
    if (t.empty()) return false;
    if (t[0] == '[') { size_t rb = t.find(']'); if (rb == std::string::npos || rb + 2 > t.size()) return false; e.ip = t.substr(1, rb - 1); e.port = atoi(t.c_str() + rb + 2); }
    else { size_t c = t.rfind(':'); if (c == std::string::npos) return false; e.ip = t.substr(0, c); e.port = atoi(t.c_str() + c + 1); }
    return e.port > 0 && e.port < 65536;
}

// ---------------------------------------------------------------- storage

// Maps the torrent's byte space onto its files. Pad files (BEP 47) read as zeros and are never written.
class Storage {
    std::mutex m;
    std::vector<HANDLE> handles;
public:
    std::wstring root;
    std::vector<File> files;
    std::vector<bool> wanted;
    std::wstring lastError;
    ~Storage() { closeAll(); }
    void init(const std::wstring& folder, const std::vector<File>& f, const std::vector<bool>& want) {
        closeAll();
        root = folder; files = f; wanted = want;
        std::lock_guard<std::mutex> lk(m);
        handles.assign(files.size(), INVALID_HANDLE_VALUE);
    }
    void closeAll() {
        std::lock_guard<std::mutex> lk(m);
        for (auto& h : handles) if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    }
    std::wstring pathOf(size_t i) const { return joinPath(root, files[i].path); }
    HANDLE handle(size_t i, bool create) {
        std::lock_guard<std::mutex> lk(m);
        if (handles[i] != INVALID_HANDLE_VALUE) return handles[i];
        std::wstring p = pathOf(i);
        if (create) makeDirs(folderOf(p));
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { if (create) lastError = errorText(GetLastError()) + L": " + p; return h; }
        if (create) { DWORD br = 0; DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr); }
        handles[i] = h;
        return h;
    }
    template <class F> bool each(int64_t off, size_t n, F f) {
        size_t lo = 0, hi = files.size();
        while (lo + 1 < hi) { size_t mid = (lo + hi) / 2; if (files[mid].offset <= off) lo = mid; else hi = mid; }
        for (size_t i = lo; i < files.size() && n; ++i) {
            const File& fl = files[i];
            if (off >= fl.offset + fl.length) continue;
            int64_t inFile = off - fl.offset;
            size_t k = (size_t)std::min<int64_t>((int64_t)n, fl.length - inFile);
            if (!f(i, inFile, k)) return false;
            off += (int64_t)k; n -= k;
        }
        return n == 0;
    }
    bool write(int64_t off, const char* p, size_t n) {
        return each(off, n, [&](size_t i, int64_t at, size_t k) {
            if (files[i].pad) { p += k; return true; }
            HANDLE h = handle(i, true);
            if (h == INVALID_HANDLE_VALUE) return false;
            OVERLAPPED ov{}; ov.Offset = (DWORD)(at & 0xFFFFFFFF); ov.OffsetHigh = (DWORD)(at >> 32);
            DWORD w = 0;
            if (!WriteFile(h, p, (DWORD)k, &w, &ov) || w != k) { lastError = errorText(GetLastError()); return false; }
            p += k;
            return true;
        });
    }
    bool read(int64_t off, char* p, size_t n) {
        return each(off, n, [&](size_t i, int64_t at, size_t k) {
            if (files[i].pad) { memset(p, 0, k); p += k; return true; }
            HANDLE h = handle(i, false);
            if (h == INVALID_HANDLE_VALUE) return false;
            OVERLAPPED ov{}; ov.Offset = (DWORD)(at & 0xFFFFFFFF); ov.OffsetHigh = (DWORD)(at >> 32);
            DWORD r = 0;
            if (!ReadFile(h, p, (DWORD)k, &r, &ov) || r != k) return false;
            p += k;
            return true;
        });
    }
    // Sets each finished wanted file to its exact length (sparse writes may leave it shorter).
    void finalizeSizes() {
        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i].pad || !wanted[i]) continue;
            HANDLE h = handle(i, true);
            if (h == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER cur{}; GetFileSizeEx(h, &cur);
            if (cur.QuadPart != files[i].length) { LARGE_INTEGER sz; sz.QuadPart = files[i].length; SetFilePointerEx(h, sz, nullptr, FILE_BEGIN); SetEndOfFile(h); }
        }
    }
};

// ---------------------------------------------------------------- trackers

struct Announce {
    std::string hash, peerId;
    int port = 6881;
    int64_t up = 0, down = 0, left = 0;
    int event = 0; // 0 none, 1 completed, 2 started, 3 stopped
    int numwant = 80;
    uint32_t key = 0;
};
struct AnnounceResult { int interval = 1800; std::vector<Endpoint> peers; int seeders = -1, leechers = -1; std::string err; };

inline bool announceHttp(const std::string& url, const Announce& a, const http::Options& opt, AnnounceResult& r) {
    static const char* events[] = {"", "completed", "started", "stopped"};
    std::string u = url + (url.find('?') == std::string::npos ? "?" : "&");
    u += "info_hash=" + percentEncode(a.hash) + "&peer_id=" + percentEncode(a.peerId) + "&port=" + std::to_string(a.port) +
         "&uploaded=" + std::to_string(a.up) + "&downloaded=" + std::to_string(a.down) + "&left=" + std::to_string(a.left) +
         "&compact=1&no_peer_id=1&numwant=" + std::to_string(a.numwant) + "&key=" + std::to_string(a.key);
    if (a.event) u += std::string("&event=") + events[a.event];
    net::Url pu;
    if (!net::parseUrl(u, pu)) { r.err = "Bad tracker URL"; return false; }
    std::string body, err;
    if (!http::fetch(pu, opt, body, err, 4u << 20)) { r.err = err; return false; }
    Value v;
    size_t used = 0;
    if (!decodePrefix(body, v, used) || v.type != Value::Dict) { r.err = "Tracker sent an invalid response"; return false; }
    if (v.get("failure reason")) { r.err = "Tracker: " + v.str("failure reason"); return false; }
    r.interval = (int)std::max<int64_t>(60, v.num("interval", 1800));
    r.seeders = (int)v.num("complete", -1); r.leechers = (int)v.num("incomplete", -1);
    if (auto p = v.get("peers")) {
        if (p->type == Value::Str) parseCompact(p->s, r.peers, false);
        else for (auto& e : p->list) { Endpoint x; x.ip = e.str("ip"); x.port = (int)e.num("port"); if (!x.ip.empty() && x.port > 0) r.peers.push_back(x); }
    }
    if (auto p6 = v.get("peers6")) if (p6->type == Value::Str) parseCompact(p6->s, r.peers, true);
    return true;
}

inline void put32(std::string& s, uint32_t v) { s += char(v >> 24); s += char(v >> 16); s += char(v >> 8); s += char(v); }
inline void put64(std::string& s, uint64_t v) { put32(s, (uint32_t)(v >> 32)); put32(s, (uint32_t)v); }
inline uint32_t get32(const char* p) { return (uint32_t)(uint8_t)p[0] << 24 | (uint32_t)(uint8_t)p[1] << 16 | (uint32_t)(uint8_t)p[2] << 8 | (uint8_t)p[3]; }

// BEP 15 UDP tracker protocol.
inline bool announceUdp(const std::string& url, const Announce& a, const std::atomic<bool>* cancel, AnnounceResult& r) {
    net::init();
    std::string rest = url.substr(6);
    size_t slash = rest.find('/');
    Endpoint ep;
    if (!parseHostPort(rest.substr(0, slash), ep)) { r.err = "Bad tracker URL"; return false; }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(ep.ip.c_str(), std::to_string(ep.port).c_str(), &hints, &res) != 0 || !res) { r.err = "Cannot resolve tracker"; return false; }
    SOCKET s = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    bool v6 = res->ai_family == AF_INET6;
    if (s == INVALID_SOCKET || ::connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { freeaddrinfo(res); if (s != INVALID_SOCKET) closesocket(s); r.err = "UDP socket failed"; return false; }
    freeaddrinfo(res);
    auto exchange = [&](const std::string& req, std::string& resp, uint32_t tx, uint32_t action) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (cancel && *cancel) return false;
            send(s, req.data(), (int)req.size(), 0);
            uint64_t end = nowMs() + 3000u * (1u << attempt);
            while (nowMs() < end) {
                if (cancel && *cancel) return false;
                fd_set fs; FD_ZERO(&fs); FD_SET(s, &fs);
                timeval tv{0, 200000};
                if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
                char buf[4096];
                int n = recv(s, buf, sizeof buf, 0);
                if (n < 8) continue;
                if (get32(buf + 4) != tx) continue;
                if (get32(buf) == 3) { r.err = "Tracker: " + std::string(buf + 8, (size_t)n - 8); return false; }
                if (get32(buf) != action) continue;
                resp.assign(buf, (size_t)n);
                return true;
            }
        }
        if (r.err.empty()) r.err = "UDP tracker timed out";
        return false;
    };
    uint32_t tx; randomBytes(&tx, 4);
    std::string req; put64(req, 0x41727101980ull); put32(req, 0); put32(req, tx);
    std::string resp;
    if (!exchange(req, resp, tx, 0) || resp.size() < 16) { closesocket(s); return false; }
    std::string conn = resp.substr(8, 8);
    randomBytes(&tx, 4);
    req = conn; put32(req, 1); put32(req, tx);
    req += a.hash; req += a.peerId;
    put64(req, (uint64_t)a.down); put64(req, (uint64_t)a.left); put64(req, (uint64_t)a.up);
    static const uint32_t ev[] = {0, 1, 2, 3};
    put32(req, ev[a.event]); put32(req, 0); put32(req, a.key); put32(req, (uint32_t)a.numwant);
    req += char(a.port >> 8); req += char(a.port & 0xFF);
    bool ok = exchange(req, resp, tx, 1) && resp.size() >= 20;
    closesocket(s);
    if (!ok) return false;
    r.interval = (int)std::max<uint32_t>(60, get32(resp.data() + 8));
    r.leechers = (int)get32(resp.data() + 12); r.seeders = (int)get32(resp.data() + 16);
    parseCompact(resp.substr(20), r.peers, v6);
    return true;
}

// ---------------------------------------------------------------- DHT (BEP 5, IPv4)

class Dht {
public:
    struct Node { std::string id; sockaddr_in addr{}; uint64_t seen = 0; int fails = 0; };
    std::atomic<bool> stopFlag{false};
    std::atomic<int> nodeCount{0};

    ~Dht() { stop(); }
    bool start(int port) {
        if (sock != INVALID_SOCKET) return true;
        net::init();
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) return false;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (sockaddr*)&a, sizeof a) != 0) {
            a.sin_port = 0; // the port is taken: use any port
            if (bind(sock, (sockaddr*)&a, sizeof a) != 0) { closesocket(sock); sock = INVALID_SOCKET; return false; }
        }
        id.resize(20); randomBytes(&id[0], 20);
        secret = randomToken(8);
        stopFlag = false;
        th = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        stopFlag = true;
        if (th.joinable()) th.join();
        if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
    }
    // Starts (or refreshes) a peer lookup; announce = also announce our port to the closest nodes.
    void lookup(const std::string& hash, int port, bool announce) {
        std::lock_guard<std::mutex> lk(m);
        Lookup& L = lookups[hash];
        if (L.active) return;
        L = Lookup();
        L.active = true; L.target = hash; L.port = port; L.announce = announce; L.started = nowMs();
        for (auto& n : table) addCandidate(L, n);
    }
    std::vector<Endpoint> takePeers(const std::string& hash) {
        std::lock_guard<std::mutex> lk(m);
        auto it = lookups.find(hash);
        if (it == lookups.end()) return {};
        std::vector<Endpoint> r;
        r.swap(it->second.found);
        return r;
    }
    bool lookupActive(const std::string& hash) { std::lock_guard<std::mutex> lk(m); auto it = lookups.find(hash); return it != lookups.end() && it->second.active; }
    void forget(const std::string& hash) { std::lock_guard<std::mutex> lk(m); lookups.erase(hash); }
    void addNode(const std::string& ip, int port) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
        if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return;
        std::lock_guard<std::mutex> lk(m);
        pendingPings.push_back(a);
    }

private:
    struct Cand { std::string id; sockaddr_in addr; bool queried = false; bool replied = false; std::string token; };
    struct Lookup {
        bool active = false, announce = false, announced = false;
        std::string target; int port = 0; uint64_t started = 0;
        std::map<std::string, Cand> cands; // keyed by XOR distance
        int inflight = 0;
        std::vector<Endpoint> found;
        std::set<std::string> foundKeys;
    };
    struct Tx { int kind; std::string hash; std::string dist; uint64_t sent; };
    SOCKET sock = INVALID_SOCKET;
    std::string id, secret;
    std::thread th;
    std::mutex m;
    std::vector<Node> table;
    std::map<std::string, Lookup> lookups;
    std::map<std::string, Tx> txs;
    std::vector<sockaddr_in> pendingPings;
    std::map<std::string, std::vector<std::string>> stored; // hash -> compact peers announced to us
    uint16_t txCounter = 1;

    static std::string xorDist(const std::string& a, const std::string& b) { std::string d(20, '\0'); for (int i = 0; i < 20; ++i) d[i] = a[i] ^ b[i]; return d; }
    static std::string compactAddr(const sockaddr_in& a) { std::string s((const char*)&a.sin_addr, 4); s.append((const char*)&a.sin_port, 2); return s; }
    void addCandidate(Lookup& L, const Node& n) {
        if (n.id.size() != 20) return;
        std::string d = xorDist(n.id, L.target);
        if (L.cands.count(d)) return;
        Cand c; c.id = n.id; c.addr = n.addr;
        L.cands[d] = c;
        if (L.cands.size() > 200) L.cands.erase(std::prev(L.cands.end()));
    }
    void sendTo(const sockaddr_in& a, const std::string& msg) { sendto(sock, msg.data(), (int)msg.size(), 0, (const sockaddr*)&a, sizeof a); }
    std::string newTx(int kind, const std::string& hash, const std::string& dist) {
        std::string t(2, '\0'); t[0] = char(txCounter >> 8); t[1] = char(txCounter & 0xFF); ++txCounter;
        txs[t] = Tx{kind, hash, dist, nowMs()};
        return t;
    }
    void query(const sockaddr_in& a, const std::string& q, const std::string& args, int kind, const std::string& hash = "", const std::string& dist = "") {
        std::string t = newTx(kind, hash, dist);
        // Keys in sorted order: a, q, t, v, y.
        std::string msg = "d1:ad2:id20:" + id + args + "e1:q" + bstr(q) + "1:t" + bstr(t) + "1:v4:TD10" + "1:y1:qe";
        sendTo(a, msg);
    }
    std::string closestNodes(const std::string& target) {
        std::vector<std::pair<std::string, const Node*>> v;
        for (auto& n : table) v.push_back({xorDist(n.id, target), &n});
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, const Node*>& a, const std::pair<std::string, const Node*>& b) { return a.first < b.first; });
        std::string out;
        for (size_t i = 0; i < v.size() && i < 8; ++i) out += v[i].second->id + compactAddr(v[i].second->addr);
        return out;
    }
    std::string tokenFor(const sockaddr_in& a) { return sha1(compactAddr(a) + secret).substr(0, 8); }
    void learn(const std::string& nid, const sockaddr_in& a) {
        if (nid.size() != 20 || nid == id) return;
        for (auto& n : table) if (n.id == nid) { n.seen = nowMs(); n.fails = 0; n.addr = a; return; }
        if (table.size() >= 400) {
            auto worst = std::max_element(table.begin(), table.end(), [](const Node& x, const Node& y) { return x.fails < y.fails || (x.fails == y.fails && x.seen > y.seen); });
            if (worst->fails == 0) return;
            table.erase(worst);
        }
        Node n; n.id = nid; n.addr = a; n.seen = nowMs();
        table.push_back(n);
        nodeCount = (int)table.size();
    }
    void handle(const char* buf, int len, const sockaddr_in& from) {
        std::string data(buf, (size_t)len);
        Value v;
        if (!decode(data, v) || v.type != Value::Dict) return;
        std::string y = v.str("y"), t = v.str("t");
        std::lock_guard<std::mutex> lk(m);
        if (y == "q") {
            std::string q = v.str("q");
            const Value* a = v.get("a");
            if (!a) return;
            std::string nid = a->str("id");
            learn(nid, from);
            std::string r;
            if (q == "ping" || q == "announce_peer") {
                if (q == "announce_peer") {
                    std::string ih = a->str("info_hash");
                    if (ih.size() == 20 && a->str("token") == tokenFor(from)) {
                        sockaddr_in p = from;
                        if (a->num("implied_port") == 0) p.sin_port = htons((u_short)a->num("port"));
                        auto& lst = stored[ih];
                        std::string c = compactAddr(p);
                        if (std::find(lst.begin(), lst.end(), c) == lst.end()) { lst.push_back(c); if (lst.size() > 100) lst.erase(lst.begin()); }
                    }
                }
                r = "d2:id20:" + id + "e";
            } else if (q == "find_node") {
                r = "d2:id20:" + id + "5:nodes" + bstr(closestNodes(a->str("target"))) + "e";
            } else if (q == "get_peers") {
                std::string ih = a->str("info_hash");
                r = "d2:id20:" + id + "5:nodes" + bstr(closestNodes(ih)) + "5:token" + bstr(tokenFor(from));
                auto it = stored.find(ih);
                if (it != stored.end() && !it->second.empty()) { r += "6:valuesl"; for (auto& c : it->second) r += bstr(c); r += "e"; }
                r += "e";
            } else return;
            sendTo(from, "d1:r" + r + "1:t" + bstr(t) + "1:y1:re");
            return;
        }
        if (y != "r") return;
        auto txi = txs.find(t);
        if (txi == txs.end()) return;
        Tx tx = txi->second;
        txs.erase(txi);
        const Value* r = v.get("r");
        if (!r) return;
        std::string nid = r->str("id");
        learn(nid, from);
        std::vector<Node> nodes;
        std::string ns = r->str("nodes");
        for (size_t i = 0; i + 26 <= ns.size(); i += 26) {
            Node n; n.id = ns.substr(i, 20);
            n.addr.sin_family = AF_INET;
            memcpy(&n.addr.sin_addr, ns.data() + i + 20, 4);
            memcpy(&n.addr.sin_port, ns.data() + i + 24, 2);
            if (n.addr.sin_port && n.id != id) nodes.push_back(n);
        }
        if (tx.kind == 1) { // find_node during bootstrap
            for (auto& n : nodes) { if (table.size() < 60) { std::string q = "6:target20:" + id; query(n.addr, "find_node", q, 1); } learn(n.id, n.addr); }
            return;
        }
        if (tx.kind == 2) {
            auto li = lookups.find(tx.hash);
            if (li == lookups.end()) return;
            Lookup& L = li->second;
            if (L.inflight > 0) --L.inflight;
            auto ci = L.cands.find(tx.dist);
            if (ci != L.cands.end()) { ci->second.replied = true; ci->second.token = r->str("token"); }
            for (auto& n : nodes) addCandidate(L, n);
            if (auto vals = r->get("values")) {
                for (auto& x : vals->list) {
                    if (x.type != Value::Str || x.s.size() != 6) continue;
                    std::vector<Endpoint> eps; parseCompact(x.s, eps, false);
                    for (auto& e : eps) if (L.foundKeys.insert(e.key()).second) L.found.push_back(e);
                }
            }
        }
    }
    void drive() {
        std::lock_guard<std::mutex> lk(m);
        uint64_t now = nowMs();
        for (auto it = txs.begin(); it != txs.end();) {
            if (now - it->second.sent > 5000) {
                if (it->second.kind == 2) { auto li = lookups.find(it->second.hash); if (li != lookups.end() && li->second.inflight > 0) --li->second.inflight; }
                it = txs.erase(it);
            } else ++it;
        }
        for (auto& a : pendingPings) query(a, "find_node", "6:target20:" + id, 1);
        pendingPings.clear();
        for (auto& kv : lookups) {
            Lookup& L = kv.second;
            if (!L.active) continue;
            if (L.cands.empty()) { for (auto& n : table) addCandidate(L, n); }
            int closestDone = 0, considered = 0;
            for (auto& c : L.cands) {
                if (considered++ >= 8) break;
                if (c.second.queried && (c.second.replied || now - L.started > 4000)) ++closestDone;
            }
            for (auto& c : L.cands) {
                if (L.inflight >= 8) break;
                if (c.second.queried) continue;
                c.second.queried = true;
                ++L.inflight;
                query(c.second.addr, "get_peers", "9:info_hash20:" + L.target, 2, L.target, c.first);
            }
            bool finished = (closestDone >= 8 && L.inflight == 0) || now - L.started > 45000;
            if (finished) {
                if (L.announce && !L.announced) {
                    int n = 0;
                    for (auto& c : L.cands) {
                        if (n >= 8) break;
                        if (!c.second.replied || c.second.token.empty()) continue;
                        ++n;
                        query(c.second.addr, "announce_peer", "12:implied_porti0e9:info_hash20:" + L.target + "4:porti" + std::to_string(L.port) + "e5:token" + bstr(c.second.token), 3);
                    }
                    L.announced = true;
                }
                L.active = false;
            }
        }
    }
    void bootstrap() {
        static const std::pair<const char*, int> routers[] = {{"router.bittorrent.com", 6881}, {"dht.transmissionbt.com", 6881}, {"router.utorrent.com", 6881}, {"dht.libtorrent.org", 25401}, {"dht.aelitis.com", 6881}};
        for (auto& r : routers) {
            if (stopFlag) return;
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(r.first, std::to_string(r.second).c_str(), &hints, &res) != 0 || !res) continue;
            sockaddr_in a = *(sockaddr_in*)res->ai_addr;
            freeaddrinfo(res);
            std::lock_guard<std::mutex> lk(m);
            query(a, "find_node", "6:target20:" + id, 1);
        }
    }
    void loop() {
        uint64_t lastBoot = 0;
        while (!stopFlag) {
            uint64_t now = nowMs();
            if ((nodeCount < 20 && now - lastBoot > 20000) || lastBoot == 0) { lastBoot = now; bootstrap(); }
            fd_set fs; FD_ZERO(&fs); FD_SET(sock, &fs);
            timeval tv{0, 100000};
            if (select(0, &fs, nullptr, nullptr, &tv) > 0) {
                char buf[4096]; sockaddr_in from{}; int fl = sizeof from;
                int n = recvfrom(sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
                if (n > 0) handle(buf, n, from);
            }
            drive();
        }
    }
};

// ---------------------------------------------------------------- incoming connections

class Listener {
public:
    SOCKET s = INVALID_SOCKET;
    int port = 0;
    std::thread th;
    std::atomic<bool> stopFlag{false};
    std::function<void(SOCKET, const std::string&)> onAccept;
    ~Listener() { stop(); }
    bool start(int wantPort) {
        if (s != INVALID_SOCKET) return true;
        net::init();
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
        bool ok = false;
        for (int p = wantPort; p < wantPort + 10 && !ok; ++p) { a.sin_port = htons((u_short)p); if (bind(s, (sockaddr*)&a, sizeof a) == 0) { ok = true; port = p; } }
        if (!ok || listen(s, 32) != 0) { closesocket(s); s = INVALID_SOCKET; return false; }
        stopFlag = false;
        th = std::thread([this] {
            while (!stopFlag) {
                fd_set fs; FD_ZERO(&fs); FD_SET(s, &fs);
                timeval tv{0, 200000};
                if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
                sockaddr_storage from{}; int fl = sizeof from;
                SOCKET c = accept(s, (sockaddr*)&from, &fl);
                if (c == INVALID_SOCKET) continue;
                char buf[64] = {0};
                if (from.ss_family == AF_INET) inet_ntop(AF_INET, &((sockaddr_in*)&from)->sin_addr, buf, sizeof buf);
                std::string addr = std::string(buf) + ":" + std::to_string(ntohs(((sockaddr_in*)&from)->sin_port));
                if (onAccept) onAccept(c, addr); else closesocket(c);
            }
        });
        return true;
    }
    void stop() {
        stopFlag = true;
        if (th.joinable()) th.join();
        if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
    }
};

// Peer client names from Azureus-style peer ids ("-qB4630-…").
inline std::string clientName(const std::string& pid) {
    if (pid.size() < 8 || pid[0] != '-' || pid[7] != '-') return "";
    std::string code = pid.substr(1, 2), ver = pid.substr(3, 4);
    static const std::pair<const char*, const char*> names[] = {{"qB", "qBittorrent"}, {"TR", "Transmission"}, {"UT", "\xC2\xB5Torrent"}, {"UM", "\xC2\xB5Torrent Mac"}, {"LT", "libtorrent"}, {"lt", "rTorrent"}, {"DE", "Deluge"}, {"AZ", "Vuze"}, {"BT", "BitTorrent"}, {"BI", "BiglyBT"}, {"TD", "TDM"}, {"KT", "KTorrent"}, {"FD", "Free Download Manager"}, {"WW", "WebTorrent"}, {"TX", "Tixati"}};
    std::string n = code;
    for (auto& p : names) if (code == p.first) n = p.second;
    std::string v;
    for (char c : ver) { if (!v.empty()) v += '.'; v += c; }
    return n + " " + v;
}

} // namespace bt
