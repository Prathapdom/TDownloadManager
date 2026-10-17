// Engine tests against local servers only: segmented HTTP with resume, redirects, naming, checksums,
// Tor routing through a local SOCKS5 server, bencode/magnet/torrent creation, settings round trip,
// and a BitTorrent transfer between this process and a seeding copy of itself.
#include "settings.hpp"
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace tdm;
static int checks = 0;
#define REQUIRE(c) do { ++checks; if (!(c)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #c); } while (0)

static std::wstring tempDir;
static std::wstring file(const std::wstring& name) { return tempDir + L"\\" + name; }
template <class F> static bool waitFor(F f, DWORD ms = 15000) { auto t = GetTickCount64(); while (GetTickCount64() - t < ms) { if (f()) return true; Sleep(20); } return f(); }

static std::string pattern(size_t n, int seed = 0) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = (char)((i * 31 + i / 7 + seed) & 0xFF);
    return s;
}

// ---------------------------------------------------------------- local HTTP server

struct HttpServer {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::string big = pattern(3 << 20), small = pattern(600 << 10, 5);
    std::atomic<int> rangeRequests{0}, requests{0};
    std::atomic<bool> quit{false};
    std::thread th;
    void start() {
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(ls, (sockaddr*)&a, sizeof a); listen(ls, 64);
        int len = sizeof a; getsockname(ls, (sockaddr*)&a, &len); port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (!quit) {
                SOCKET c = accept(ls, nullptr, nullptr);
                if (c == INVALID_SOCKET) break;
                std::thread([this, c] { serve(c); closesocket(c); }).detach();
            }
        });
    }
    void stop() { quit = true; closesocket(ls); th.join(); }
    static bool sendAll(SOCKET c, const char* p, size_t n) {
        while (n) { int k = send(c, p, (int)std::min<size_t>(n, 65536), 0); if (k <= 0) return false; p += k; n -= k; }
        return true;
    }
    void serve(SOCKET c) {
        std::string req; char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) { int k = recv(c, buf, sizeof buf, 0); if (k <= 0) return; req.append(buf, k); }
        ++requests;
        std::string path = req.substr(req.find(' ') + 1); path = path.substr(0, path.find(' '));
        std::string low = lower(req);
        int64_t from = -1, to = -1;
        size_t r = low.find("\r\nrange: bytes=");
        if (r != std::string::npos) { from = _atoi64(low.c_str() + r + 15); size_t d = low.find('-', r + 15); if (d != std::string::npos && isdigit((uint8_t)low[d + 1])) to = _atoi64(low.c_str() + d + 1); }
        auto reply = [&](const std::string& head, const std::string& body) { std::string h = head + "Connection: close\r\n\r\n"; sendAll(c, h.data(), h.size()); sendAll(c, body.data(), body.size()); };
        if (path == "/missing") { reply("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n", "not found"); return; }
        if (path == "/redirect") { reply("HTTP/1.1 302 Found\r\nLocation: /data/moved.bin\r\nContent-Length: 0\r\n", ""); return; }
        if (path == "/norange.bin") { reply("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(small.size()) + "\r\n", small); return; }
        const std::string& body = big;
        std::string extra = path == "/named" ? "Content-Disposition: attachment; filename=\"report 2026.pdf\"\r\n" : "";
        if (from >= 0 && from < (int64_t)body.size()) {
            ++rangeRequests;
            int64_t end = to >= 0 ? std::min<int64_t>(to, body.size() - 1) : body.size() - 1;
            reply("HTTP/1.1 206 Partial Content\r\nAccept-Ranges: bytes\r\n" + extra + "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(end) + "/" +
                  std::to_string(body.size()) + "\r\nContent-Length: " + std::to_string(end - from + 1) + "\r\n", body.substr(from, end - from + 1));
            return;
        }
        reply("HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n", body);
    }
};

// ---------------------------------------------------------------- local SOCKS5 server (stands in for Tor)

struct SocksServer {
    SOCKET ls = INVALID_SOCKET;
    int port = 0, target = 0;
    std::mutex m;
    std::string lastHost, lastUser; // lastUser: the latest non-empty isolation user
    std::atomic<int> connects{0};
    std::thread th;
    void start(int targetPort) {
        target = targetPort;
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(ls, (sockaddr*)&a, sizeof a); listen(ls, 64);
        int len = sizeof a; getsockname(ls, (sockaddr*)&a, &len); port = ntohs(a.sin_port);
        th = std::thread([this] {
            for (;;) {
                SOCKET c = accept(ls, nullptr, nullptr);
                if (c == INVALID_SOCKET) break;
                std::thread([this, c] { serve(c); closesocket(c); }).detach();
            }
        });
    }
    void stop() { closesocket(ls); th.join(); }
    static bool readN(SOCKET c, char* p, int n) { while (n) { int k = recv(c, p, n, 0); if (k <= 0) return false; p += k; n -= k; } return true; }
    void serve(SOCKET c) {
        char h[2]; if (!readN(c, h, 2) || h[0] != 5) return;
        std::string methods(h[1], '\0'); if (!readN(c, &methods[0], h[1])) return;
        bool auth = methods.find('\x02') != std::string::npos;
        char sel[2] = {5, (char)(auth ? 2 : 0)}; send(c, sel, 2, 0);
        std::string user;
        if (auth) {
            char v[2]; if (!readN(c, v, 2)) return;
            user.resize((uint8_t)v[1]); if (!user.empty() && !readN(c, &user[0], (int)user.size())) return;
            char pl; if (!readN(c, &pl, 1)) return;
            std::string pass((uint8_t)pl, '\0'); if (!pass.empty() && !readN(c, &pass[0], (int)pass.size())) return;
            char ok[2] = {1, 0}; send(c, ok, 2, 0);
        }
        char q[4]; if (!readN(c, q, 4)) return; // a probe stops here
        std::string host;
        if (q[3] == 3) { char l; readN(c, &l, 1); host.resize((uint8_t)l); readN(c, &host[0], (uint8_t)l); }
        else if (q[3] == 1) { char ip[4]; readN(c, ip, 4); host = "ipv4"; }
        else return;
        char pb[2]; readN(c, pb, 2);
        { std::lock_guard<std::mutex> lk(m); lastHost = host; if (!user.empty()) lastUser = user; }
        ++connects;
        SOCKET up = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)target);
        bool ok = connect(up, (sockaddr*)&a, sizeof a) == 0;
        char rep[10] = {5, (char)(ok ? 0 : 4), 0, 1, 127, 0, 0, 1, 0, 0}; send(c, rep, 10, 0);
        if (!ok) { closesocket(up); return; }
        std::thread back([c, up] { char b[16384]; int k; while ((k = recv(up, b, sizeof b, 0)) > 0) if (send(c, b, k, 0) <= 0) break; shutdown(c, SD_BOTH); });
        char b[16384]; int k;
        while ((k = recv(c, b, sizeof b, 0)) > 0) if (send(up, b, k, 0) <= 0) break;
        shutdown(up, SD_BOTH);
        back.join();
        closesocket(up);
    }
};

// ---------------------------------------------------------------- helpers

static engine::Engine* E = nullptr;
static uint32_t lastId = 0; // dumped when a check fails
static HttpServer web;
static SocksServer socks;
static std::string base() { return "http://127.0.0.1:" + std::to_string(web.port); }
static engine::View view(uint32_t id) { for (auto& v : E->snapshot(true, id)) if (v.id == id) return v; return {}; }
static bool finished(uint32_t id) { auto v = view(id); return v.status == engine::Completed || v.status == engine::Failed || v.status == engine::Seeding; }
static std::string readAll(const std::wstring& p) { std::string s; engine::readWholeFile(p, s, 64u << 20); return s; }
static void dump(uint32_t id) {
    auto v = view(id);
    std::string data = readAll(v.path());
    size_t diff = 0; while (diff < data.size() && diff < web.big.size() && data[diff] == web.big[diff]) ++diff;
    fprintf(stderr, "  #%u %s tor=%d done=%lld size=%lld file=%u firstDiff=%u segs=%u error=%s path=%s\n", id, narrow(engine::statusName(v.status)).c_str(), (int)v.tor,
            (long long)v.done, (long long)v.size, (unsigned)data.size(), (unsigned)diff, (unsigned)v.segs.size(), narrow(v.error).c_str(), narrow(v.path()).c_str());
}
static uint32_t add(const std::string& url, std::function<void(engine::AddRequest&)> tweak = nullptr) {
    engine::AddRequest r; r.url = url;
    if (tweak) tweak(r);
    std::wstring err; uint32_t id = E->add(r, err);
    if (!id) throw std::runtime_error("add failed: " + narrow(err));
    lastId = id;
    return id;
}

// ---------------------------------------------------------------- tests

static void testParsing() {
    net::Url u;
    REQUIRE(net::parseUrl("https://user:pw@Example.com:8443/a/b?q=1#frag", u) && u.scheme == "https" && u.host == "example.com" && u.port == 8443 && u.path == "/a/b?q=1" && u.user == "user");
    REQUIRE(net::parseUrl("http://abcdefghijklmnop.onion/x", u) && u.onion() && u.port == 80);
    REQUIRE(!net::parseUrl("ftp://example.com/x", u) && !net::parseUrl("http://", u) && !net::parseUrl("http://host:99999/", u));
    REQUIRE(net::parseUrl("example.com/file.zip", u) && u.scheme == "http" && u.path == "/file.zip");
    int64_t a, b, t;
    REQUIRE(http::parseContentRange("bytes 100-199/1000", a, b, t) && a == 100 && b == 199 && t == 1000);
    REQUIRE(http::dispositionFilename("attachment; filename=\"a b.zip\"") == "a b.zip");
    REQUIRE(http::dispositionFilename("attachment; filename*=UTF-8''caf%C3%A9.txt") == "caf\xC3\xA9.txt");
    std::string algo, digest;
    REQUIRE(engine::parseChecksum("SHA-256 " + std::string(64, 'A'), algo, digest) && algo == "sha256" && digest == std::string(64, 'a'));
    REQUIRE(engine::parseChecksum(std::string(32, 'f'), algo, digest) && algo == "md5");
    REQUIRE(!engine::parseChecksum("xyz", algo, digest));
    REQUIRE(engine::categoryFor(L"movie.mkv", engine::KindHttp) == L"Video");
    REQUIRE(engine::categoryFor(L"setup.exe", engine::KindHttp) == L"Programs");
    REQUIRE(engine::categoryFor(L"x.7z", engine::KindHttp) == L"Compressed");
    REQUIRE(sanitizeFileName(L"a<b>:c?.txt").find_first_of(L"<>:?") == std::wstring::npos);
}

static void testBencode() {
    bt::Value v;
    REQUIRE(bt::decode("d3:cow3:moo4:spaml1:a1:bee", v) && v.type == bt::Value::Dict && v.str("cow") == "moo" && v.get("spam")->list.size() == 2);
    REQUIRE(bt::decode("i-42e", v) && v.i == -42);
    REQUIRE(!bt::decode("d3:cow", v));
    REQUIRE(!bt::decode("i42", v));
    std::string deep(5000, 'l'); deep += std::string(5000, 'e');
    REQUIRE(!bt::decode(deep, v)); // depth limited, no stack overflow
    bt::Magnet m;
    std::string hash(20, '\x11');
    std::string link = bt::makeMagnet(hash, L"My file", {"udp://tracker.example:80/announce"}, {"127.0.0.1:6881"});
    REQUIRE(bt::parseMagnet(link, m) && m.hash == hash && m.name == L"My file" && m.trackers.size() == 1 && m.peers.size() == 1 && m.peers[0] == "127.0.0.1:6881");
    REQUIRE(bt::parseMagnet("magnet:?xt=urn:btih:CEIRCEIRCEIRCEIRCEIRCEIRCEIRCEIR", m) && m.hash == hash); // base32
    REQUIRE(!bt::parseMagnet("magnet:?dn=x", m));
}

static void testCreateTorrent() {
    std::wstring dir = file(L"pack");
    makeDirs(dir + L"\\sub");
    engine::writeWholeFile(dir + L"\\one.bin", pattern(100000, 1));
    engine::writeWholeFile(dir + L"\\sub\\two.bin", pattern(250001, 2));
    engine::writeWholeFile(dir + L"\\empty.txt", "");
    std::string err;
    std::string t = bt::createTorrent(dir, 32768, {"http://tracker.local/announce"}, true, err);
    REQUIRE(!t.empty());
    bt::Meta meta;
    REQUIRE(bt::parseTorrent(t, meta, err));
    REQUIRE(meta.name == L"pack" && meta.multi && meta.priv && meta.pieceLen == 32768 && meta.total == 350001);
    REQUIRE(meta.pieceCount() == (int)((350001 + 32767) / 32768));
    REQUIRE(meta.trackers.size() == 1);
    REQUIRE(engine::visibleFiles(meta).size() == 3);
    REQUIRE(bt::sha1(meta.info) == meta.hash);
    std::string single = bt::createTorrent(dir + L"\\one.bin", 0, {}, false, err);
    REQUIRE(bt::parseTorrent(single, meta, err) && !meta.multi && meta.total == 100000 && meta.name == L"one.bin");
    REQUIRE(!bt::parseTorrent("d4:infod4:name1:xee", meta, err));
}

static void testSegmented() {
    std::wstring hashErr;
    engine::writeWholeFile(file(L"ref.bin"), web.big);
    std::string sha = engine::hashFile(file(L"ref.bin"), "sha256", nullptr, hashErr);
    REQUIRE(sha.size() == 64);
    int before = web.rangeRequests;
    uint32_t id = add(base() + "/files/big.bin", [&](engine::AddRequest& r) { r.connections = 6; r.limit = 3 << 20; r.checksum = "sha256:" + sha; });
    REQUIRE(waitFor([&] { return finished(id); }, 30000));
    auto v = view(id);
    REQUIRE(v.status == engine::Completed);
    REQUIRE(v.name == L"big.bin" && v.size == (int64_t)web.big.size() && v.done == v.size && v.resumable);
    REQUIRE(web.rangeRequests - before >= 2); // the file was split across connections
    REQUIRE(readAll(v.path()) == web.big);
    REQUIRE(v.checkResult.find(L"verified") != std::wstring::npos);
    REQUIRE(!fileExists(v.path() + L".tdm"));

    // A wrong checksum is reported, not silently accepted.
    uint32_t bad = add(base() + "/files/big.bin", [&](engine::AddRequest& r) { r.name = L"bad.bin"; r.checksum = "md5:" + std::string(32, '0'); });
    REQUIRE(waitFor([&] { return finished(bad); }, 30000));
    REQUIRE(view(bad).checkResult.find(L"MISMATCH") != std::wstring::npos);
}

static void testNamingAndRedirects() {
    uint32_t a = add(base() + "/redirect");
    uint32_t b = add(base() + "/named");
    REQUIRE(waitFor([&] { return finished(a) && finished(b); }, 30000));
    auto va = view(a), vb = view(b);
    REQUIRE(va.status == engine::Completed && va.name == L"moved.bin" && readAll(va.path()) == web.big);
    REQUIRE(vb.status == engine::Completed && vb.name == L"report 2026.pdf" && vb.category == L"Documents");
    // Same name again gets a numbered copy instead of overwriting.
    uint32_t c = add(base() + "/named");
    REQUIRE(waitFor([&] { return finished(c); }, 30000));
    REQUIRE(view(c).path() != vb.path() && fileExists(vb.path()) && fileExists(view(c).path()));
}

static void testNoRanges() {
    uint32_t id = add(base() + "/norange.bin", [](engine::AddRequest& r) { r.connections = 8; });
    REQUIRE(waitFor([&] { return finished(id); }, 30000));
    auto v = view(id);
    REQUIRE(v.status == engine::Completed && !v.resumable && readAll(v.path()) == web.small);
}

static void testPauseResume() {
    uint32_t id = add(base() + "/files/slow.bin", [](engine::AddRequest& r) { r.limit = 600 << 10; r.connections = 4; });
    REQUIRE(waitFor([&] { auto v = view(id); return v.done > (300 << 10); }, 15000));
    E->pause(id);
    REQUIRE(waitFor([&] { return view(id).status == engine::Paused; }, 10000));
    auto p = view(id);
    REQUIRE(p.done > 0 && p.done < p.size && fileExists(p.path() + L".tdm"));
    Sleep(300);
    REQUIRE(view(id).done == p.done); // nothing moves while paused
    // Lift the limit and resume: it continues from the saved segments.
    { auto it = E->find(id); std::lock_guard<std::mutex> lk(it->m); it->limit = 0; it->limiter.rate = 0; }
    int before = web.rangeRequests;
    E->resume(id);
    REQUIRE(waitFor([&] { return finished(id); }, 30000));
    auto v = view(id);
    REQUIRE(v.status == engine::Completed && readAll(v.path()) == web.big);
    REQUIRE(web.rangeRequests > before);
}

static void testFailures() {
    uint32_t id = add(base() + "/missing");
    REQUIRE(waitFor([&] { return finished(id); }, 30000));
    auto v = view(id);
    REQUIRE(v.status == engine::Failed && v.error.find(L"404") != std::wstring::npos);
    std::wstring err;
    engine::AddRequest r; r.url = "ftp://example.com/x";
    REQUIRE(E->add(r, err) == 0 && !err.empty());
}

static void testTor() {
    engine::Config c = E->config();
    c.torPort = socks.port; c.torHost = "127.0.0.1"; c.torIsolate = true;
    E->setConfig(c);
    E->forgetTor();
    // .onion goes through Tor automatically; the proxy resolves the name, never the local resolver.
    uint32_t id = add("http://tdmtestexampleonionaddr.onion:" + std::to_string(web.port) + "/files/hidden.bin", [](engine::AddRequest& r) { r.connections = 3; });
    REQUIRE(waitFor([&] { return finished(id); }, 30000));
    auto v = view(id);
    REQUIRE(v.status == engine::Completed && v.tor == 0 && readAll(v.path()) == web.big);
    { std::lock_guard<std::mutex> lk(socks.m); REQUIRE(socks.lastHost == "tdmtestexampleonionaddr.onion"); REQUIRE(!socks.lastUser.empty()); }
    // A clearnet link forced through Tor.
    int before = socks.connects;
    uint32_t f = add(base() + "/files/forced.bin", [](engine::AddRequest& r) { r.tor = 1; });
    REQUIRE(waitFor([&] { return finished(f); }, 30000));
    REQUIRE(view(f).status == engine::Completed && view(f).tor == 1 && socks.connects > before);
    // Direct links stay direct.
    before = socks.connects;
    uint32_t d = add(base() + "/files/direct.bin");
    REQUIRE(waitFor([&] { return finished(d); }, 30000));
    REQUIRE(view(d).status == engine::Completed && !view(d).tor && socks.connects == before);
    // Tor unreachable: a clear error instead of a silent direct connection.
    c.torPort = 1; E->setConfig(c); E->forgetTor();
    uint32_t x = add("http://unreachableonionaddress.onion/x.bin");
    REQUIRE(waitFor([&] { return finished(x); }, 30000));
    REQUIRE(view(x).status == engine::Failed && view(x).error.find(L"Tor") != std::wstring::npos);
    c.torPort = socks.port; E->setConfig(c); E->forgetTor();
}

static void testQueue() {
    engine::Config c = E->config(); c.maxActive = 1; E->setConfig(c);
    std::vector<uint32_t> ids;
    for (int i = 0; i < 3; ++i) ids.push_back(add(base() + "/files/q" + std::to_string(i) + ".bin", [](engine::AddRequest& r) { r.limit = 1 << 20; }));
    Sleep(400);
    int running = 0;
    for (auto id : ids) { if (engine::isRunningStatus(view(id).status)) ++running; }
    if (running > 1) for (auto id : ids) dump(id);
    REQUIRE(running <= 1);
    E->move(ids[2], -1000);
    auto order = E->snapshot();
    size_t p2 = 0, p1 = 0;
    for (size_t i = 0; i < order.size(); ++i) { if (order[i].id == ids[2]) p2 = i; if (order[i].id == ids[1]) p1 = i; }
    REQUIRE(p2 < p1);
    REQUIRE(waitFor([&] { for (auto id : ids) if (!finished(id)) return false; return true; }, 60000));
    c.maxActive = 4; E->setConfig(c);
    // Remove with files deletes them.
    std::wstring p = view(ids[0]).path();
    REQUIRE(fileExists(p));
    E->remove(ids[0], true);
    REQUIRE(waitFor([&] { return !fileExists(p); }, 5000));
    REQUIRE(!E->find(ids[0]));
}

static void testSettings() {
    std::wstring ini = file(L"settings.ini");
    settings::Settings s;
    s.cfg.folder = L"D:\\Downloads \u00E9t\u00E9";
    s.cfg.maxActive = 7; s.cfg.connections = 12; s.cfg.speedLimit = 5 << 20; s.cfg.verifyTls = false;
    s.cfg.proxy.kind = net::Proxy::Socks5; s.cfg.proxy.host = "proxy.local"; s.cfg.proxy.port = 1080; s.cfg.proxy.user = "me"; s.cfg.proxy.pass = "s3cret=;#";
    s.cfg.torPort = 9150; s.cfg.torAll = true; s.cfg.torPath = L"C:\\Tor\\tor.exe";
    s.cfg.btPort = 51413; s.cfg.btDht = false; s.cfg.seedRatio = 2.5; s.cfg.btViaTor = true;
    s.ui.theme = 3; s.ui.sidebar = false; s.ui.columns = {300, 0, 120}; s.ui.sortColumn = 2; s.ui.sortAscending = false;
    s.ui.recentFolders = {L"C:\\a", L"C:\\b|c"}; s.ui.closeToTray = true;
    REQUIRE(settings::save(ini, s));
    std::string raw = readAll(ini);
    REQUIRE(raw.find("s3cret") == std::string::npos); // the proxy password is not stored in clear text
    settings::Settings t;
    REQUIRE(settings::load(ini, t));
    REQUIRE(t.cfg.folder == s.cfg.folder && t.cfg.maxActive == 7 && t.cfg.connections == 12 && t.cfg.speedLimit == (5 << 20) && !t.cfg.verifyTls);
    REQUIRE(t.cfg.proxy.kind == net::Proxy::Socks5 && t.cfg.proxy.host == "proxy.local" && t.cfg.proxy.port == 1080 && t.cfg.proxy.pass == "s3cret=;#");
    REQUIRE(t.cfg.torPort == 9150 && t.cfg.torAll && t.cfg.torPath == s.cfg.torPath);
    REQUIRE(t.cfg.btPort == 51413 && !t.cfg.btDht && t.cfg.seedRatio == 2.5 && t.cfg.btViaTor);
    REQUIRE(t.ui.theme == 3 && !t.ui.sidebar && t.ui.columns == s.ui.columns && t.ui.sortColumn == 2 && !t.ui.sortAscending && t.ui.closeToTray);
    REQUIRE(t.ui.recentFolders == s.ui.recentFolders);
    // Missing or damaged files fall back to defaults.
    engine::writeWholeFile(ini, "garbage\n[x\nmaxActive=abc\n");
    settings::Settings d;
    settings::load(ini, d);
    REQUIRE(d.cfg.maxActive == engine::Config().maxActive);

    // The download list survives a restart.
    settings::ItemSaver saver; saver.path = file(L"downloads.ini");
    REQUIRE(saver.save(*E, true));
    auto items = settings::loadItems(saver.path);
    auto snap = E->snapshot();
    REQUIRE(items.size() == snap.size());
    size_t completed = 0;
    for (auto& it : items) if (it->status == engine::Completed) ++completed;
    size_t want = 0; for (auto& v : snap) if (v.status == engine::Completed) ++want;
    REQUIRE(completed == want);
}

// ---------------------------------------------------------------- BitTorrent between two processes

static PROCESS_INFORMATION seeder{};
static int runSeeder(const std::wstring& torrent, const std::wstring& folder, int port) {
    engine::Engine e;
    e.cfg.folder = folder; e.dataDir = folder + L"\\data";
    e.cfg.btPort = port; e.cfg.btDht = false; e.cfg.btUpnp = false; e.cfg.seedRatio = 0 + 1000;
    e.start();
    engine::AddRequest r; std::wstring err;
    if (!engine::readWholeFile(torrent, r.torrentData)) return 2;
    uint32_t id = e.add(r, err);
    if (!id) return 3;
    for (int i = 0; i < 1200; ++i) Sleep(100); // parent terminates us
    e.shutdown();
    return 0;
}

static void testTorrentTransfer(const std::wstring& exe) {
    std::wstring seedDir = file(L"seed"), leechDir = file(L"leech");
    makeDirs(seedDir + L"\\album\\disc 2"); makeDirs(leechDir);
    std::string f1 = pattern(1500000, 9), f2 = pattern(777777, 10), f3 = pattern(12345, 11);
    engine::writeWholeFile(seedDir + L"\\album\\track1.bin", f1);
    engine::writeWholeFile(seedDir + L"\\album\\disc 2\\track2.bin", f2);
    engine::writeWholeFile(seedDir + L"\\album\\cover.jpg", f3);
    std::string err;
    std::string t = bt::createTorrent(seedDir + L"\\album", 65536, {}, false, err);
    REQUIRE(!t.empty());
    engine::writeWholeFile(file(L"album.torrent"), t);
    bt::Meta meta; REQUIRE(bt::parseTorrent(t, meta, err));

    int seedPort = 47000 + (GetCurrentProcessId() % 1000), myPort = engine::Config().btPort;
    (void)myPort;
    std::wstring cmd = L"\"" + exe + L"\" --seed \"" + file(L"album.torrent") + L"\" \"" + seedDir + L"\" " + std::to_wstring(seedPort);
    STARTUPINFOW si{sizeof si};
    REQUIRE(CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &seeder));
    CloseHandle(seeder.hThread);

    // Magnet with a direct peer: metadata comes from the seeder, then the pieces.
    std::string magnet = bt::makeMagnet(meta.hash, L"album", {}, {"127.0.0.1:" + std::to_string(seedPort)});
    uint32_t id = add(magnet, [&](engine::AddRequest& r) { r.folder = leechDir; });
    bool done = waitFor([&] { auto v = view(id); return v.status == engine::Seeding || v.status == engine::Completed || v.status == engine::Failed; }, 90000);
    auto v = view(id);
    if (!done || v.status == engine::Failed) std::wcerr << L"torrent: " << engine::statusName(v.status) << L" " << v.done << L"/" << v.size << L" " << v.error << L"\n";
    REQUIRE(done && v.status != engine::Failed);
    REQUIRE(v.name == L"album" && v.size == meta.total && v.files.size() == 3 && v.infoHash == hex((const uint8_t*)meta.hash.data(), 20));
    REQUIRE(readAll(leechDir + L"\\album\\track1.bin") == f1);
    REQUIRE(readAll(leechDir + L"\\album\\disc 2\\track2.bin") == f2);
    REQUIRE(readAll(leechDir + L"\\album\\cover.jpg") == f3);

    // Recheck finds every piece.
    E->pause(id);
    REQUIRE(waitFor([&] { return !engine::isRunningStatus(view(id).status) || view(id).status == engine::Paused; }, 10000));
    E->recheck(id);
    REQUIRE(waitFor([&] { auto x = view(id); return x.status != engine::Checking && x.done == meta.total; }, 30000));
    E->pause(id);
    TerminateProcess(seeder.hProcess, 0);
    WaitForSingleObject(seeder.hProcess, 5000);
    CloseHandle(seeder.hProcess); seeder.hProcess = nullptr;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"--seed") return runSeeder(argv[2], argv[3], _wtoi(argv[4]));
    net::init();
    wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH, temp);
    tempDir = std::wstring(temp) + L"tdm-tests-" + std::to_wstring(GetCurrentProcessId());
    makeDirs(tempDir);
    int rc = 0;
    try {
        web.start();
        socks.start(web.port);
        testParsing();
        testBencode();
        testCreateTorrent();
        E = new engine::Engine; // leaked on failure so a running engine is never destroyed mid-test
        engine::Engine& e = *E;

        e.cfg.folder = file(L"downloads");
        e.dataDir = file(L"data");
        e.cfg.retries = 1; e.cfg.timeoutSec = 10; e.cfg.minSplit = 64 << 10;
        e.cfg.btDht = false; e.cfg.btUpnp = false; e.cfg.btPort = 46000 + (GetCurrentProcessId() % 1000);
        e.cfg.torPort = socks.port;
        e.start();
        testSegmented();
        testNamingAndRedirects();
        testNoRanges();
        testPauseResume();
        testFailures();
        testTor();
        testQueue();
        testTorrentTransfer(argv[0]);
        testSettings();
        e.shutdown();
        delete E; E = nullptr;
        std::cout << "All " << checks << " checks passed\n";
    } catch (const std::exception& ex) {
        std::cerr << "FAILED " << ex.what() << " (after " << checks << " checks)\n";
        if (E && lastId) dump(lastId);
        if (seeder.hProcess) TerminateProcess(seeder.hProcess, 1);
        rc = 1;
        _exit(rc);
    }
    socks.stop();
    web.stop();
    SHFILEOPSTRUCTW op{}; std::wstring from = tempDir + L'\0'; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    return rc;
}
