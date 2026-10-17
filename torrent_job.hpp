// BitTorrent job: one event loop per torrent (peer wire protocol, metadata exchange, PEX, choking, seeding),
// plus the engine-wide listener/DHT and the helpers that add torrents and magnets.
#pragma once
#include "engine.hpp"
#include "torrent.hpp"

namespace engine {

// ---------------------------------------------------------------- files

inline bool readWholeFile(const std::wstring& p, std::string& out, size_t limit = 64u << 20) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
    if (sz.QuadPart < 0 || (uint64_t)sz.QuadPart > limit) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD r = 0;
    bool ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &r, nullptr) && r == out.size());
    CloseHandle(h);
    return ok;
}
// Write to a temporary file, then rename over the target, so a crash never leaves half a file.
inline bool writeWholeFile(const std::wstring& p, const std::string& data) {
    std::wstring dir = folderOf(p);
    if (!dir.empty()) makeDirs(dir);
    std::wstring tmp = p + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size());
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
inline std::wstring torrentStorePath(Engine& e, const std::string& hashHex) {
    return joinPath(joinPath(e.dataDir, L"torrents"), widen(hashHex) + L".torrent");
}
inline std::string buildTorrentFile(const std::string& info, const std::vector<std::string>& trs) {
    std::string s = "d";
    if (!trs.empty()) {
        s += "8:announce" + bt::bstr(trs[0]) + "13:announce-listl";
        for (auto& t : trs) s += "l" + bt::bstr(t) + "e";
        s += "e";
    }
    return s + "4:info" + info + "e";
}
inline std::vector<std::string> mergeTrackers(std::vector<std::string> a, const std::vector<std::string>& b) {
    for (auto& t : b) if (!t.empty() && std::find(a.begin(), a.end(), t) == a.end()) a.push_back(t);
    return a;
}
// Used only for public torrents that arrive with no trackers at all.
inline const std::vector<std::string>& fallbackTrackers() {
    static std::vector<std::string> t{"udp://tracker.opentrackr.org:1337/announce", "udp://open.stealth.si:80/announce", "udp://tracker.torrent.eu.org:451/announce",
                                      "udp://exodus.desync.com:6969/announce", "https://tracker.opentrackr.org:443/announce"};
    return t;
}

inline std::vector<size_t> visibleFiles(const bt::Meta& m) {
    std::vector<size_t> v;
    for (size_t i = 0; i < m.files.size(); ++i) if (!m.files[i].pad) v.push_back(i);
    return v;
}
inline void fileProgress(const bt::Meta& m, const std::vector<uint8_t>& have, std::vector<TorrentFile>& out) {
    auto vis = visibleFiles(m);
    for (size_t k = 0; k < vis.size() && k < out.size(); ++k) {
        const bt::File& f = m.files[vis[k]];
        int64_t d = 0;
        if (f.length > 0 && !have.empty()) {
            int a = (int)(f.offset / m.pieceLen), b = (int)((f.offset + f.length - 1) / m.pieceLen);
            for (int i = a; i <= b && i < (int)have.size(); ++i) {
                if (!have[i]) continue;
                int64_t ps = (int64_t)i * m.pieceLen, pe = ps + m.pieceSize(i);
                d += std::min(pe, f.offset + f.length) - std::max(ps, f.offset);
            }
        }
        out[k].done = d;
    }
}
inline std::vector<uint8_t> haveFromHex(const std::string& hx, int np) {
    std::vector<uint8_t> h;
    std::string raw;
    if (np <= 0 || hx.size() != (size_t)((np + 7) / 8) * 2 || !unhex(hx, raw)) return h;
    h.assign(np, 0);
    for (int i = 0; i < np; ++i) h[i] = ((uint8_t)raw[i >> 3] >> (7 - (i & 7))) & 1;
    return h;
}
// Copies the torrent's names, files and sizes into the item (caller holds the item's lock or owns it).
inline void applyMetaToItem(Item& it, const bt::Meta& m) {
    auto vis = visibleFiles(m);
    if (it.fileSel.size() != vis.size()) it.fileSel.assign(vis.size(), '1');
    it.files.clear();
    int64_t total = 0;
    for (size_t k = 0; k < vis.size(); ++k) {
        const bt::File& f = m.files[vis[k]];
        TorrentFile t; t.path = f.path; t.size = f.length; t.offset = f.offset; t.wanted = it.fileSel[k] == '1';
        if (t.wanted) total += f.length;
        it.files.push_back(t);
    }
    it.torrentName = m.name; it.name = m.name;
    it.pieceCount = m.pieceCount(); it.pieceLen = m.pieceLen;
    it.size = total;
}
// Fills file lists for a torrent restored from the downloads file (so the details pane works before it runs).
inline void loadTorrentInfo(Engine& e, Item& it) {
    std::string data, err;
    bt::Meta m;
    if (it.infoHash.empty() || !readWholeFile(torrentStorePath(e, it.infoHash), data) || !bt::parseTorrent(data, m, err)) return;
    int64_t done = it.done;
    applyMetaToItem(it, m);
    fileProgress(m, haveFromHex(it.have, m.pieceCount()), it.files);
    it.done = done;
}

// Parses a .torrent or magnet for Engine::add, rejects duplicates and stores the metainfo.
inline bool prepareTorrentItem(Engine& e, Item& it, const AddRequest& r, std::wstring& err) {
    bt::Meta m;
    bool haveMeta = false;
    std::vector<std::string> trs, hints;
    std::wstring dn;
    if (!r.torrentData.empty()) {
        std::string perr;
        if (!bt::parseTorrent(r.torrentData, m, perr)) { err = widen(perr); return false; }
        haveMeta = true;
        trs = m.trackers;
    } else {
        bt::Magnet mg;
        if (!bt::parseMagnet(trim(r.url), mg)) { err = L"That magnet link has no valid info hash."; return false; }
        m.hash = mg.hash; trs = mg.trackers; dn = mg.name; hints = mg.peers;
    }
    std::string hx = hex((const uint8_t*)m.hash.data(), 20);
    {
        std::lock_guard<std::mutex> lk(e.m);
        for (auto& x : e.items) if (!x->removed && x->infoHash == hx) {
            std::lock_guard<std::mutex> l2(x->m);
            x->trackers = mergeTrackers(x->trackers, trs);
            x->dirty = true;
            err = L"This torrent is already in the list" + (x->name.empty() ? std::wstring(L".") : L": " + x->name);
            return false;
        }
    }
    std::wstring store = torrentStorePath(e, hx);
    if (haveMeta) {
        if (!writeWholeFile(store, r.torrentData)) { err = L"Cannot save the torrent file: " + errorText(GetLastError()); return false; }
    } else {
        // A magnet for a torrent whose metainfo we kept earlier.
        std::string data, perr;
        bt::Meta old;
        if (readWholeFile(store, data) && bt::parseTorrent(data, old, perr) && old.hash == m.hash) { trs = mergeTrackers(trs, old.trackers); m = old; haveMeta = true; }
    }
    it.infoHash = hx;
    it.trackers = trs;
    if (haveMeta) applyMetaToItem(it, m);
    else { it.name = !r.name.empty() ? sanitizeFileName(r.name) : !dn.empty() ? sanitizeFileName(dn) : L"Magnet " + widen(hx.substr(0, 8)); it.size = -1; }
    it.url = bt::makeMagnet(m.hash, haveMeta ? m.name : dn, trs, hints);
    return true;
}

// ---------------------------------------------------------------- engine-wide services

// Connections handed to a torrent's loop: finished outgoing connects and accepted incoming peers.
struct BtInbox {
    struct Conn { SOCKET s = INVALID_SOCKET; std::string key, hs; bool incoming = false; };
    std::mutex m;
    std::atomic<bool> closed{false};
    std::vector<Conn> conns;
    void push(Conn c) {
        std::lock_guard<std::mutex> lk(m);
        if (closed) { if (c.s != INVALID_SOCKET) closesocket(c.s); return; }
        conns.push_back(std::move(c));
    }
    std::vector<Conn> take() { std::lock_guard<std::mutex> lk(m); std::vector<Conn> r; r.swap(conns); return r; }
    void close() {
        std::lock_guard<std::mutex> lk(m);
        closed = true;
        for (auto& c : conns) if (c.s != INVALID_SOCKET) closesocket(c.s);
        conns.clear();
    }
};

class BtServices {
public:
    std::mutex m;
    bt::Dht dht;
    bt::Listener listener;
    bool dhtOn = false, listenTried = false;
    std::string peerId;
    std::map<std::string, std::shared_ptr<BtInbox>> sessions; // raw info hash -> inbox

    BtServices() {
        static const char al[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        uint8_t r[12]; randomBytes(r, sizeof r);
        peerId = "-TD1000-";
        for (int i = 0; i < 12; ++i) peerId += al[r[i] % 62];
    }
    int listenPort(int want) {
        std::lock_guard<std::mutex> lk(m);
        if (!listenTried) {
            listenTried = true;
            listener.onAccept = [this](SOCKET c, const std::string& a) { std::thread([this, c, a] { handshakeIncoming(c, a); }).detach(); };
            listener.start(want);
        }
        return listener.s != INVALID_SOCKET ? listener.port : 0;
    }
    bt::Dht* dhtFor(int port) {
        std::lock_guard<std::mutex> lk(m);
        if (!dhtOn) dhtOn = dht.start(port);
        return dhtOn ? &dht : nullptr;
    }
    int dhtNodes() { return dhtOn ? dht.nodeCount.load() : -1; }
    void add(const std::string& hash, std::shared_ptr<BtInbox> box) { std::lock_guard<std::mutex> lk(m); sessions[hash] = box; }
    void remove(const std::string& hash) { std::lock_guard<std::mutex> lk(m); sessions.erase(hash); }

    // Reads the 68-byte handshake of an incoming peer, then passes it to the torrent it asks for.
    void handshakeIncoming(SOCKET c, const std::string& addr) {
        u_long nb = 1; ioctlsocket(c, FIONBIO, &nb);
        std::string hs;
        uint64_t end = nowMs() + 10000;
        while (hs.size() < 68 && nowMs() < end) {
            fd_set fs; FD_ZERO(&fs); FD_SET(c, &fs);
            timeval tv{0, 200000};
            if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
            char buf[68];
            int n = recv(c, buf, (int)(68 - hs.size()), 0);
            if (n > 0) hs.append(buf, (size_t)n);
            else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) break;
        }
        if (hs.size() < 68 || hs[0] != 19 || hs.compare(1, 19, "BitTorrent protocol") != 0) { closesocket(c); return; }
        std::shared_ptr<BtInbox> box;
        { std::lock_guard<std::mutex> lk(m); auto f = sessions.find(hs.substr(28, 20)); if (f != sessions.end()) box = f->second; }
        if (!box) { closesocket(c); return; }
        BtInbox::Conn cn; cn.s = c; cn.key = addr; cn.hs = hs; cn.incoming = true;
        box->push(std::move(cn));
    }
};
inline BtServices& btServices() { static BtServices s; return s; }

// ---------------------------------------------------------------- peers

struct BtPeer {
    struct Req { int piece, off, len; uint64_t t; };
    SOCKET s = INVALID_SOCKET;
    std::string key, ip, pid, client;
    bool incoming = false, handshook = false, closing = false, ext = false, extSeen = false, dhtSupport = false;
    std::string in, out;
    size_t inPos = 0, outPos = 0;
    std::vector<bool> has;
    int hasCount = 0;
    std::string rawBitfield;          // received before we had the metadata
    std::vector<int> pendingHaves;
    bool amChoking = true, amInterested = false, peerChoking = true, peerInterested = false;
    int utMeta = 0, utPex = 0, metaPending = 0;
    int64_t metaSize = 0;
    bool metaRejected = false;
    std::vector<Req> reqs;            // our outstanding requests
    std::deque<Req> uploads;          // the peer's requests to us
    uint64_t connected = 0, lastRecv = 0, lastSend = 0;
    int64_t downWin = 0, upWin = 0, downTotal = 0, upTotal = 0;
    double downRate = 0, upRate = 0;
    bool snubbed = false;
    size_t pending() const { return out.size() - outPos; }
};

class TorrentJob {
    static const int BLOCK = 16384;
    struct Partial { std::vector<uint8_t> st; int got = 0, freeCount = 0; size_t cursor = 0; }; // st: 0 free, 1 requested, 2 written
    struct Cand { bt::Endpoint ep; uint64_t nextTry = 0; int fails = 0; bool busy = false; };

    Engine& e;
    std::shared_ptr<Item> itp;
    Item& it;
    Config c;
    BtServices& svc;
    std::shared_ptr<BtInbox> inbox = std::make_shared<BtInbox>();
    std::string hash, peerId;
    net::Proxy proxy;
    bool viaTor = false, priv = false;
    int listenPort = 0, annPort = 6881;
    bt::Dht* dht = nullptr;

    bt::Meta meta;
    bool hasMeta = false;
    bt::Storage store;
    std::vector<size_t> vis;
    std::vector<bool> wantedFiles;
    std::string sel;
    int np = 0;
    std::vector<uint8_t> have, wantP, isPartial;
    std::vector<int> avail;
    std::vector<int64_t> wantBytes;
    std::map<int, Partial> partial;
    int64_t verifiedBytes = 0, partialBytes = 0, selSize = 0, upDelta = 0, rawDown = 0;
    int haveCount = 0, missing = 0, hashFails = 0;
    bool complete = false, completeAtStart = false, seeding = false, finishedNow = false, haveDirty = true, stopDone = false;
    std::wstring fatal;
    std::string pieceBuf;

    // Metadata exchange (magnets).
    int64_t metaSize = 0;
    std::vector<std::string> metaParts, metaOwner;
    std::vector<uint8_t> metaState;
    std::vector<uint64_t> metaAt;

    std::vector<std::unique_ptr<BtPeer>> peers;
    std::map<std::string, Cand> cands;
    int connecting = 0;
    std::string optimistic;
    uint64_t lastOpt = 0, startedAt = 0;
    uint64_t rs = 0x9E3779B97F4A7C15ull;

    // Trackers (each on its own thread).
    std::atomic<bool> stopTrackers{false};
    std::vector<std::thread> trackerThreads;
    std::mutex tm;
    std::vector<bt::Endpoint> trackerPeers;
    std::set<std::string> startedTrackers;
    int64_t annUp = 0, annDown = 0, annLeft = 16384;
    bool annCompleted = false, annNeed = true;
    std::string trackerErr;
    uint32_t annKey = 0;

public:
    TorrentJob(Engine& en, std::shared_ptr<Item> p) : e(en), itp(p), it(*p), svc(btServices()) { randomBytes(&rs, sizeof rs); rs |= 1; randomBytes(&annKey, 4); }

    uint64_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
    int64_t pieceStart(int i) const { return (int64_t)i * meta.pieceLen; }
    int blocksIn(int i) const { return (int)((meta.pieceSize(i) + BLOCK - 1) / BLOCK); }
    int blockLen(int i, int b) const { return (int)std::min<int64_t>(BLOCK, meta.pieceSize(i) - (int64_t)b * BLOCK); }

    // ------------------------------------------------------------ messages

    void sendMsg(BtPeer& p, uint8_t id, const std::string& payload = std::string()) {
        bt::put32(p.out, (uint32_t)payload.size() + 1);
        p.out += (char)id;
        p.out += payload;
        p.lastSend = nowMs();
    }
    void sendHandshake(BtPeer& p) {
        std::string h(1, (char)19);
        h += "BitTorrent protocol";
        char res[8] = {0};
        res[5] = 0x10;                // extension protocol (BEP 10)
        if (dht) res[7] = 0x01;       // DHT (BEP 5)
        h.append(res, 8);
        h += hash; h += peerId;
        p.out += h;
        p.lastSend = nowMs();
    }
    std::string bitfieldBytes() const {
        std::string b((size_t)(np + 7) / 8, '\0');
        for (int i = 0; i < np; ++i) if (have[i]) b[(size_t)i >> 3] |= (char)(0x80 >> (i & 7));
        return b;
    }
    void sendExtHandshake(BtPeer& p) {
        std::string d = "d1:md11:ut_metadatai1e";
        if (!priv) d += "6:ut_pexi2e";
        d += "e";
        if (hasMeta) d += "13:metadata_sizei" + std::to_string(meta.info.size()) + "e";
        if (listenPort) d += "1:pi" + std::to_string(listenPort) + "e";
        d += "4:reqqi250e1:v" + bt::bstr("TDM 1.0") + "e";
        sendMsg(p, 20, std::string(1, '\0') + d);
    }
    void setInterest(BtPeer& p, bool v) {
        if (v == p.amInterested) return;
        p.amInterested = v;
        sendMsg(p, v ? 2 : 3);
    }
    void updateInterest(BtPeer& p) {
        if (!hasMeta || complete || p.has.empty()) { setInterest(p, false); return; }
        bool want = false;
        for (int i = 0; i < np && !want; ++i) if (p.has[i] && wantP[i] && !have[i]) want = true;
        setInterest(p, want);
    }
    int unchokedCount() const { int n = 0; for (auto& q : peers) if (q->handshook && !q->amChoking) ++n; return n; }

    // ------------------------------------------------------------ socket I/O

    void flushOut(BtPeer& p) {
        while (p.pending()) {
            int n = send(p.s, p.out.data() + p.outPos, (int)std::min<size_t>(p.pending(), 1u << 20), 0);
            if (n > 0) { p.outPos += (size_t)n; continue; }
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
            p.closing = true;
            break;
        }
        if (p.outPos == p.out.size()) { p.out.clear(); p.outPos = 0; }
        else if (p.outPos > (1u << 20)) { p.out.erase(0, p.outPos); p.outPos = 0; }
    }
    void readIn(BtPeer& p) {
        char buf[65536];
        for (int k = 0; k < 16; ++k) {
            int n = recv(p.s, buf, sizeof buf, 0);
            if (n > 0) { p.in.append(buf, (size_t)n); p.lastRecv = nowMs(); continue; }
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
            p.closing = true;
            break;
        }
        parse(p);
    }
    void parse(BtPeer& p) {
        while (fatal.empty()) {
            size_t avl = p.in.size() - p.inPos;
            const char* d = p.in.data() + p.inPos;
            if (!p.handshook) {
                if (avl < 68) break;
                if (d[0] != 19 || memcmp(d + 1, "BitTorrent protocol", 19) != 0 || memcmp(d + 28, hash.data(), 20) != 0) { p.closing = true; break; }
                p.ext = (d[25] & 0x10) != 0;
                p.dhtSupport = (d[27] & 0x01) != 0;
                p.pid.assign(d + 48, 20);
                p.inPos += 68;
                if (p.pid == peerId) { p.closing = true; break; }
                onHandshake(p);
                if (p.closing) break;
                continue;
            }
            if (avl < 4) break;
            uint32_t len = bt::get32(d);
            if (len > (2u << 20)) { p.closing = true; break; }
            if (avl < 4 + (size_t)len) break;
            p.inPos += 4 + (size_t)len;
            if (len) handleMsg(p, (uint8_t)d[4], d + 5, len - 1);
        }
        if (p.inPos == p.in.size()) { p.in.clear(); p.inPos = 0; }
        else if (p.inPos > (256u << 10)) { p.in.erase(0, p.inPos); p.inPos = 0; }
    }
    void pollOnce(int ms) {
        if (peers.empty()) { Sleep((DWORD)ms); return; }
        std::vector<WSAPOLLFD> fds(peers.size());
        for (size_t i = 0; i < peers.size(); ++i) {
            fds[i].fd = peers[i]->s;
            fds[i].events = POLLRDNORM | (peers[i]->pending() ? POLLWRNORM : 0);
            fds[i].revents = 0;
        }
        int r = WSAPoll(fds.data(), (ULONG)fds.size(), ms);
        if (r < 0) { Sleep((DWORD)ms); return; }
        if (r == 0) return;
        for (size_t i = 0; i < fds.size(); ++i) {
            short rv = fds[i].revents;
            if (!rv) continue;
            BtPeer& p = *peers[i];
            if (rv & POLLNVAL) { p.closing = true; continue; }
            if (rv & (POLLRDNORM | POLLHUP | POLLERR)) readIn(p);
            if (!p.closing && (rv & POLLWRNORM)) flushOut(p);
        }
    }

    // ------------------------------------------------------------ protocol

    void onHandshake(BtPeer& p) {
        for (auto& q : peers) if (q.get() != &p && q->handshook && !q->closing && q->pid == p.pid) { p.closing = true; return; }
        p.handshook = true;
        p.client = bt::clientName(p.pid);
        if (!hasMeta && !p.ext) { p.closing = true; return; } // cannot give us the metadata
        if (hasMeta) { p.has.assign(np, false); if (haveCount) sendMsg(p, 5, bitfieldBytes()); }
        if (p.ext) sendExtHandshake(p);
        if (dht && p.dhtSupport && listenPort) { std::string pl; pl += char(listenPort >> 8); pl += char(listenPort & 0xFF); sendMsg(p, 9, pl); }
    }
    void handleMsg(BtPeer& p, uint8_t id, const char* d, size_t n) {
        switch (id) {
        case 0: p.peerChoking = true; releaseReqs(p); break;
        case 1: p.peerChoking = false; break;
        case 2:
            p.peerInterested = true;
            if (hasMeta && p.amChoking && unchokedCount() < 4) { p.amChoking = false; sendMsg(p, 1); }
            break;
        case 3: p.peerInterested = false; break;
        case 4: if (n >= 4) gotHave(p, (int)bt::get32(d)); break;
        case 5: if (!hasMeta) p.rawBitfield.assign(d, n); else applyBitfield(p, std::string(d, n)); break;
        case 6:
            if (n >= 12) {
                BtPeer::Req r{(int)bt::get32(d), (int)bt::get32(d + 4), (int)bt::get32(d + 8), nowMs()};
                if (p.amChoking || !hasMeta || r.piece < 0 || r.piece >= np || !have[r.piece] || r.len <= 0 || r.len > 131072 || r.off < 0 ||
                    (int64_t)r.off + r.len > meta.pieceSize(r.piece)) break;
                if (p.uploads.size() < 500) p.uploads.push_back(r);
            }
            break;
        case 7: if (n >= 8) gotBlock(p, (int)bt::get32(d), (int)bt::get32(d + 4), d + 8, n - 8); break;
        case 8:
            if (n >= 12) {
                int pc = (int)bt::get32(d), off = (int)bt::get32(d + 4);
                for (auto u = p.uploads.begin(); u != p.uploads.end(); ++u) if (u->piece == pc && u->off == off) { p.uploads.erase(u); break; }
            }
            break;
        case 9: if (n >= 2 && dht && !p.ip.empty()) dht->addNode(p.ip, ((uint8_t)d[0] << 8) | (uint8_t)d[1]); break;
        case 20: if (n >= 1) gotExtended(p, (uint8_t)d[0], std::string(d + 1, n - 1)); break;
        default: break;
        }
    }
    void gotHave(BtPeer& p, int idx) {
        if (!hasMeta) { if (p.pendingHaves.size() < 200000) p.pendingHaves.push_back(idx); return; }
        if (idx < 0 || idx >= np || p.has[idx]) return;
        p.has[idx] = true; ++p.hasCount; ++avail[idx];
        if (!p.amInterested && !complete && wantP[idx] && !have[idx]) setInterest(p, true);
    }
    void applyBitfield(BtPeer& p, const std::string& b) {
        if (p.has.size() != (size_t)np) { p.has.assign(np, false); p.hasCount = 0; }
        for (int i = 0; i < np; ++i) {
            bool v = (size_t)(i >> 3) < b.size() && (((uint8_t)b[(size_t)i >> 3] >> (7 - (i & 7))) & 1);
            if (v && !p.has[i]) { p.has[i] = true; ++p.hasCount; ++avail[i]; }
        }
        updateInterest(p);
    }
    static bool hasReq(const BtPeer& p, int piece, int off) {
        for (auto& r : p.reqs) if (r.piece == piece && r.off == off) return true;
        return false;
    }
    void releaseOne(BtPeer& p, const BtPeer::Req& r) {
        auto f = partial.find(r.piece);
        if (f == partial.end()) return;
        Partial& pt = f->second;
        int b = r.off / BLOCK;
        if (b < 0 || b >= (int)pt.st.size() || pt.st[b] != 1) return;
        for (auto& q : peers) if (q.get() != &p && hasReq(*q, r.piece, r.off)) return;
        pt.st[b] = 0; ++pt.freeCount;
        pt.cursor = std::min(pt.cursor, (size_t)b);
    }
    void releaseReqs(BtPeer& p) {
        std::vector<BtPeer::Req> rs2;
        rs2.swap(p.reqs);
        for (auto& r : rs2) releaseOne(p, r);
    }
    void gotBlock(BtPeer& p, int idx, int off, const char* d, size_t n) {
        bool asked = false;
        for (size_t i = 0; i < p.reqs.size(); ++i) if (p.reqs[i].piece == idx && p.reqs[i].off == off) { p.reqs.erase(p.reqs.begin() + (long)i); asked = true; break; }
        (void)asked;
        p.downWin += (int64_t)n; p.downTotal += (int64_t)n; p.snubbed = false;
        e.totalDown += (int64_t)n; e.global.consume((int64_t)n); it.limiter.consume((int64_t)n);
        rawDown += (int64_t)n;
        if (!hasMeta || idx < 0 || idx >= np || have[idx] || !isPartial[idx]) return;
        Partial& pt = partial[idx];
        if (off < 0 || off % BLOCK) return;
        int b = off / BLOCK;
        if (b >= (int)pt.st.size() || pt.st[b] == 2 || (int64_t)n != blockLen(idx, b)) return;
        if (!store.write(pieceStart(idx) + off, d, n)) { fatal = L"Cannot write to disk: " + store.lastError; return; }
        if (pt.st[b] == 0) --pt.freeCount;
        pt.st[b] = 2; ++pt.got;
        partialBytes += (int64_t)n;
        // Endgame duplicates: cancel the same block on other peers.
        for (auto& q : peers) {
            if (q.get() == &p) continue;
            for (size_t i = 0; i < q->reqs.size(); ++i) if (q->reqs[i].piece == idx && q->reqs[i].off == off) {
                std::string pl; bt::put32(pl, (uint32_t)idx); bt::put32(pl, (uint32_t)off); bt::put32(pl, (uint32_t)q->reqs[i].len);
                sendMsg(*q, 8, pl);
                q->reqs.erase(q->reqs.begin() + (long)i);
                break;
            }
        }
        if (pt.got == (int)pt.st.size()) verifyPiece(idx);
    }
    void verifyPiece(int idx) {
        int64_t sz = meta.pieceSize(idx);
        pieceBuf.resize((size_t)sz);
        bool ok = store.read(pieceStart(idx), &pieceBuf[0], (size_t)sz) && memcmp(bt::sha1(pieceBuf).data(), meta.pieces.data() + (size_t)idx * 20, 20) == 0;
        partialBytes -= sz;
        if (partialBytes < 0) partialBytes = 0;
        partial.erase(idx);
        isPartial[idx] = 0;
        if (ok) markHave(idx);
        else ++hashFails;
    }
    void markHave(int idx) {
        have[idx] = 1; ++haveCount;
        verifiedBytes += wantBytes[idx];
        if (wantP[idx]) --missing;
        haveDirty = true;
        std::string pl; bt::put32(pl, (uint32_t)idx);
        for (auto& q : peers) if (q->handshook && !q->closing && (q->has.empty() || !q->has[idx])) sendMsg(*q, 4, pl);
    }
    void startPartial(int idx) {
        Partial pt;
        pt.st.assign((size_t)blocksIn(idx), 0);
        pt.freeCount = (int)pt.st.size();
        partial[idx] = pt;
        isPartial[idx] = 1;
    }
    bool pickBlock(BtPeer& p, int& idx, int& b) {
        // Finish pieces already started before opening new ones.
        for (auto& kv : partial) {
            Partial& pt = kv.second;
            if (!pt.freeCount || !p.has[kv.first]) continue;
            for (size_t i = pt.cursor; i < pt.st.size(); ++i) if (pt.st[i] == 0) { pt.cursor = i + 1; idx = kv.first; b = (int)i; return true; }
            pt.cursor = pt.st.size();
        }
        // Rarest first, random among equals.
        int best = -1, bestA = INT32_MAX;
        int start = (int)(rnd() % (uint64_t)np);
        for (int k = 0; k < np; ++k) {
            int i = start + k;
            if (i >= np) i -= np;
            if (!wantP[i] || have[i] || isPartial[i] || !p.has[i]) continue;
            if (avail[i] < bestA) { bestA = avail[i]; best = i; if (bestA <= 1) break; }
        }
        if (best >= 0) { startPartial(best); partial[best].cursor = 1; idx = best; b = 0; return true; }
        // Endgame: every missing piece is in flight, so ask this peer for blocks others are slow with.
        if (missing > 0 && missing == (int)partial.size()) {
            for (auto& kv : partial) {
                if (!p.has[kv.first]) continue;
                auto& st = kv.second.st;
                for (size_t i = 0; i < st.size(); ++i) if (st[i] == 1 && !hasReq(p, kv.first, (int)i * BLOCK)) { idx = kv.first; b = (int)i; return true; }
            }
        }
        return false;
    }
    void requestBlock(BtPeer& p, int idx, int b) {
        Partial& pt = partial[idx];
        if (pt.st[b] == 0) { pt.st[b] = 1; --pt.freeCount; }
        int len = blockLen(idx, b);
        p.reqs.push_back({idx, b * BLOCK, len, nowMs()});
        std::string pl; bt::put32(pl, (uint32_t)idx); bt::put32(pl, (uint32_t)(b * BLOCK)); bt::put32(pl, (uint32_t)len);
        sendMsg(p, 6, pl);
    }
    void fillRequests(BtPeer& p, bool limited) {
        if (!hasMeta || complete || !p.handshook || p.closing || p.peerChoking || !p.amInterested) return;
        size_t depth = p.snubbed ? 1 : limited ? 2 : (size_t)std::min(200.0, std::max(16.0, p.downRate * 3.0 / BLOCK));
        while (p.reqs.size() < depth) {
            int idx, b;
            if (!pickBlock(p, idx, b)) break;
            requestBlock(p, idx, b);
        }
    }
    void serveUploads(BtPeer& p) {
        while (!p.uploads.empty() && p.pending() < 256 * 1024) {
            if (!e.globalUp.ready()) break;
            BtPeer::Req r = p.uploads.front();
            p.uploads.pop_front();
            if (p.amChoking || !have[r.piece]) continue;
            std::string pl;
            bt::put32(pl, (uint32_t)r.piece); bt::put32(pl, (uint32_t)r.off);
            size_t base = pl.size();
            pl.resize(base + (size_t)r.len);
            if (!store.read(pieceStart(r.piece) + r.off, &pl[base], (size_t)r.len)) continue;
            sendMsg(p, 7, pl);
            p.upWin += r.len; p.upTotal += r.len;
            upDelta += r.len;
            e.totalUp += r.len;
            e.globalUp.consume(r.len);
        }
    }

    // ------------------------------------------------------------ extension protocol

    void gotExtended(BtPeer& p, uint8_t eid, const std::string& d) {
        if (eid == 0) {
            bt::Value v;
            size_t used = 0;
            if (!bt::decodePrefix(d, v, used) || v.type != bt::Value::Dict) return;
            p.extSeen = true;
            if (auto mm = v.get("m")) { p.utMeta = (int)mm->num("ut_metadata"); p.utPex = (int)mm->num("ut_pex"); }
            int64_t ms = v.num("metadata_size");
            if (ms > 0 && ms < (64 << 20)) { p.metaSize = ms; if (!hasMeta && metaSize == 0) startMetadata(ms); }
            std::string cl = v.str("v");
            if (!cl.empty() && cl.size() < 64 && validUtf8(cl)) p.client = cl;
            return;
        }
        if (eid == 1) gotMetadataMsg(p, d);
        else if (eid == 2 && !priv) gotPex(d);
    }
    void gotPex(const std::string& d) {
        bt::Value v;
        size_t used = 0;
        if (!bt::decodePrefix(d, v, used) || v.type != bt::Value::Dict) return;
        std::vector<bt::Endpoint> eps;
        bt::parseCompact(v.str("added"), eps, false);
        bt::parseCompact(v.str("added6"), eps, true);
        for (auto& ep : eps) addCand(ep);
    }
    void startMetadata(int64_t size) {
        metaSize = size;
        size_t n = (size_t)((size + BLOCK - 1) / BLOCK);
        metaParts.assign(n, std::string()); metaOwner.assign(n, std::string());
        metaState.assign(n, 0); metaAt.assign(n, 0);
    }
    void requestMetadata() {
        if (hasMeta || !metaSize) return;
        uint64_t now = nowMs();
        for (size_t i = 0; i < metaState.size(); ++i) {
            if (metaState[i] != 1 || now - metaAt[i] < 15000) continue;
            metaState[i] = 0;
            for (auto& q : peers) if (q->key == metaOwner[i] && q->metaPending > 0) --q->metaPending;
        }
        for (auto& q : peers) {
            BtPeer& p = *q;
            if (!p.handshook || p.closing || !p.utMeta || p.metaRejected || p.metaSize != metaSize) continue;
            while (p.metaPending < 2) {
                size_t i = 0;
                while (i < metaState.size() && metaState[i] != 0) ++i;
                if (i == metaState.size()) return;
                metaState[i] = 1; metaAt[i] = now; metaOwner[i] = p.key;
                ++p.metaPending;
                sendMsg(p, 20, std::string(1, (char)p.utMeta) + "d8:msg_typei0e5:piecei" + std::to_string(i) + "ee");
            }
        }
    }
    void gotMetadataMsg(BtPeer& p, const std::string& d) {
        bt::Value v;
        size_t used = 0;
        if (!bt::decodePrefix(d, v, used) || v.type != bt::Value::Dict) return;
        int type = (int)v.num("msg_type", -1), piece = (int)v.num("piece", -1);
        if (type == 0) {
            if (!p.utMeta) return;
            if (hasMeta && piece >= 0 && (int64_t)piece * BLOCK < (int64_t)meta.info.size())
                sendMsg(p, 20, std::string(1, (char)p.utMeta) + "d8:msg_typei1e5:piecei" + std::to_string(piece) + "e10:total_sizei" + std::to_string(meta.info.size()) + "ee" +
                                   meta.info.substr((size_t)piece * BLOCK, BLOCK));
            else sendMsg(p, 20, std::string(1, (char)p.utMeta) + "d8:msg_typei2e5:piecei" + std::to_string(piece) + "ee");
            return;
        }
        if (hasMeta || !metaSize || piece < 0 || piece >= (int)metaState.size()) return;
        if (p.metaPending > 0) --p.metaPending;
        if (type == 2) { p.metaRejected = true; if (metaState[piece] == 1) metaState[piece] = 0; return; }
        if (type != 1 || metaState[piece] == 2) return;
        std::string data = d.substr(used);
        size_t want = (size_t)std::min<int64_t>(BLOCK, metaSize - (int64_t)piece * BLOCK);
        if (data.size() != want) { metaState[piece] = 0; return; }
        metaParts[piece] = data;
        metaState[piece] = 2;
        for (auto s : metaState) if (s != 2) return;
        std::string info;
        for (auto& part : metaParts) info += part;
        if (bt::sha1(info) != hash) { for (auto& s : metaState) s = 0; return; }
        onMetadata(info);
    }
    void onMetadata(const std::string& info) {
        bt::Meta m;
        std::string err;
        if (!bt::parseInfo(info, m, err)) { fatal = widen(err); return; }
        std::vector<std::string> trs;
        { std::lock_guard<std::mutex> lk(it.m); trs = it.trackers; }
        m.trackers = trs;
        writeWholeFile(torrentStorePath(e, hex((const uint8_t*)hash.data(), 20)), buildTorrentFile(info, trs));
        meta = m;
        hasMeta = true;
        priv = meta.priv;
        metaParts.clear(); metaState.clear(); metaAt.clear(); metaOwner.clear();
        if (!setupMeta()) return;
        for (auto& q : peers) {
            q->has.assign(np, false); q->hasCount = 0;
            if (!q->rawBitfield.empty()) applyBitfield(*q, q->rawBitfield);
            for (int h : q->pendingHaves) gotHave(*q, h);
            q->rawBitfield.clear(); q->pendingHaves.clear();
            updateInterest(*q);
        }
        e.changed();
    }

    // ------------------------------------------------------------ pieces and files

    void applySelection(const std::string& s) {
        sel = s;
        wantedFiles.assign(meta.files.size(), false);
        for (size_t k = 0; k < vis.size(); ++k) wantedFiles[vis[k]] = k < sel.size() && sel[k] == '1';
        wantP.assign(np, 0); wantBytes.assign(np, 0);
        selSize = 0;
        for (size_t f = 0; f < meta.files.size(); ++f) {
            const bt::File& fl = meta.files[f];
            if (!wantedFiles[f] || fl.length <= 0) continue;
            selSize += fl.length;
            int a = (int)(fl.offset / meta.pieceLen), b = (int)((fl.offset + fl.length - 1) / meta.pieceLen);
            for (int i = a; i <= b; ++i) {
                wantP[i] = 1;
                int64_t ps = pieceStart(i), pe = ps + meta.pieceSize(i);
                wantBytes[i] += std::min(pe, fl.offset + fl.length) - std::max(ps, fl.offset);
            }
        }
        store.wanted = wantedFiles;
    }
    void recount() {
        haveCount = 0; verifiedBytes = 0; missing = 0;
        for (int i = 0; i < np; ++i) {
            if (have[i]) { ++haveCount; verifiedBytes += wantBytes[i]; }
            else if (wantP[i]) ++missing;
        }
    }
    bool recheck() {
        { std::lock_guard<std::mutex> lk(it.m); it.status = Checking; it.done = 0; it.lastDone = 0; it.dirty = true; }
        e.changed();
        int64_t okBytes = 0;
        for (int i = 0; i < np; ++i) {
            if (it.cancel) return false;
            int64_t sz = meta.pieceSize(i);
            pieceBuf.resize((size_t)sz);
            bool ok = store.read(pieceStart(i), &pieceBuf[0], (size_t)sz) && memcmp(bt::sha1(pieceBuf).data(), meta.pieces.data() + (size_t)i * 20, 20) == 0;
            have[i] = ok ? 1 : 0;
            if (ok) okBytes += wantBytes[i];
            if ((i & 15) == 15 || i == np - 1) { std::lock_guard<std::mutex> lk(it.m); it.done = okBytes; it.lastDone = okBytes; it.dirty = true; }
        }
        haveDirty = true;
        return true;
    }
    bool setupMeta() {
        np = meta.pieceCount();
        vis = visibleFiles(meta);
        std::string hx, s;
        { std::lock_guard<std::mutex> lk(it.m); hx = it.have; s = it.fileSel; applyMetaToItem(it, meta); }
        if (s.size() != vis.size()) s.assign(vis.size(), '1');
        have.assign(np, 0); avail.assign(np, 0); isPartial.assign(np, 0);
        partial.clear(); partialBytes = 0;
        store.init(it.folder, meta.files, std::vector<bool>(meta.files.size(), false));
        applySelection(s);
        auto loaded = haveFromHex(hx, np);
        bool check = false;
        if (!loaded.empty()) {
            have = loaded;
            // Files deleted behind our back: verify again.
            for (size_t f = 0; f < meta.files.size() && !check; ++f) {
                const bt::File& fl = meta.files[f];
                if (fl.pad || fl.length <= 0) continue;
                int a = (int)(fl.offset / meta.pieceLen), b = (int)((fl.offset + fl.length - 1) / meta.pieceLen);
                bool any = false;
                for (int i = a; i <= b && !any; ++i) any = have[i] != 0;
                if (any && !fileExists(store.pathOf(f))) check = true;
            }
        } else {
            for (size_t f = 0; f < meta.files.size() && !check; ++f)
                if (!meta.files[f].pad && meta.files[f].length > 0 && fileExists(store.pathOf(f))) check = true;
        }
        if (check && !recheck()) return false;
        recount();
        haveDirty = true;
        publish(true);
        return true;
    }
    // The user changed which files to download.
    void selectionChanged() {
        std::string s;
        { std::lock_guard<std::mutex> lk(it.m); s = it.fileSel; }
        if (!hasMeta || s.size() != vis.size() || s == sel) return;
        applySelection(s);
        // Drop pieces in flight that are no longer wanted.
        for (auto f = partial.begin(); f != partial.end();) {
            if (wantP[f->first]) { ++f; continue; }
            int idx = f->first;
            for (size_t b = 0; b < f->second.st.size(); ++b) if (f->second.st[b] == 2) partialBytes -= blockLen(idx, (int)b);
            isPartial[idx] = 0;
            for (auto& q : peers) q->reqs.erase(std::remove_if(q->reqs.begin(), q->reqs.end(), [&](const BtPeer::Req& r) { return r.piece == idx; }), q->reqs.end());
            f = partial.erase(f);
        }
        if (partialBytes < 0) partialBytes = 0;
        recount();
        if (missing > 0 && complete) { complete = false; seeding = false; }
        for (auto& q : peers) if (q->handshook) updateInterest(*q);
        { std::lock_guard<std::mutex> lk(it.m); applyMetaToItem(it, meta); }
        publish(true);
    }

    // ------------------------------------------------------------ peers and connections

    void addCand(const bt::Endpoint& ep) {
        if (ep.ip.empty() || ep.port <= 0 || ep.port > 65535 || cands.size() >= 3000) return;
        cands.emplace(ep.key(), Cand{ep, 0, 0, false});
    }
    void spawnConnect(Cand& cd) {
        cd.busy = true;
        ++connecting;
        auto box = inbox;
        bt::Endpoint ep = cd.ep;
        net::ConnectOptions o;
        o.proxy = proxy; o.timeoutMs = viaTor ? 60000 : 8000; o.verifyTls = false; o.cancel = &box->closed;
        if (viaTor && c.torIsolate) { o.proxy.user = "tdm-bt-" + randomToken(4); o.proxy.pass = "x"; }
        std::thread([box, ep, o] {
            std::string err;
            auto t = net::connectTcp(ep.ip, ep.port, o, err);
            BtInbox::Conn cn;
            cn.key = ep.key();
            if (t) { cn.s = t->s; t->s = INVALID_SOCKET; }
            box->push(std::move(cn));
        }).detach();
    }
    void connectMore(uint64_t now) {
        if (complete && !seeding) return;
        int limit = complete ? std::max(5, c.btMaxPeers / 2) : std::max(1, c.btMaxPeers);
        int maxConnecting = viaTor ? 6 : 16;
        if ((int)peers.size() + connecting >= limit || connecting >= maxConnecting) return;
        std::set<std::string> live;
        for (auto& q : peers) live.insert(q->key);
        std::vector<Cand*> el;
        for (auto& kv : cands) {
            Cand& cd = kv.second;
            if (cd.busy || cd.nextTry > now || live.count(kv.first)) continue;
            el.push_back(&cd);
        }
        std::sort(el.begin(), el.end(), [](const Cand* a, const Cand* b) { return a->fails < b->fails; });
        for (Cand* cd : el) {
            if ((int)peers.size() + connecting >= limit || connecting >= maxConnecting) break;
            spawnConnect(*cd);
        }
    }
    void acceptConn(BtInbox::Conn& cn, uint64_t now) {
        if (!cn.incoming) {
            if (connecting > 0) --connecting;
            auto f = cands.find(cn.key);
            if (f != cands.end()) {
                Cand& cd = f->second;
                cd.busy = false;
                if (cn.s == INVALID_SOCKET) {
                    ++cd.fails;
                    if (cd.fails >= 6) { cands.erase(f); return; }
                    cd.nextTry = now + std::min<uint64_t>(600000, 30000ull << std::min(cd.fails, 4));
                } else { cd.fails = 0; cd.nextTry = now + 300000; }
            }
            if (cn.s == INVALID_SOCKET) return;
        } else if ((int)peers.size() >= c.btMaxPeers + 10 || (complete && !seeding)) { closesocket(cn.s); return; }
        auto p = std::make_unique<BtPeer>();
        p->s = cn.s; p->key = cn.key; p->incoming = cn.incoming;
        bt::Endpoint ep;
        if (bt::parseHostPort(cn.key, ep)) p->ip = ep.ip;
        p->connected = p->lastRecv = p->lastSend = now;
        p->in = cn.hs;
        sendHandshake(*p);
        peers.push_back(std::move(p));
        BtPeer& ref = *peers.back();
        if (!ref.in.empty()) parse(ref);
        flushOut(ref);
    }
    void dropClosed() {
        uint64_t now = nowMs();
        for (size_t i = 0; i < peers.size();) {
            if (!peers[i]->closing) { ++i; continue; }
            BtPeer& p = *peers[i];
            releaseReqs(p);
            if (hasMeta && p.has.size() == (size_t)np) for (int k = 0; k < np; ++k) if (p.has[k]) --avail[k];
            for (size_t k = 0; k < metaState.size(); ++k) if (metaState[k] == 1 && metaOwner[k] == p.key) metaState[k] = 0;
            closesocket(p.s);
            if (!p.incoming) {
                auto f = cands.find(p.key);
                if (f != cands.end()) f->second.nextTry = std::min(f->second.nextTry, now + (p.handshook ? 120000ull : 600000ull));
            }
            peers.erase(peers.begin() + (long)i);
        }
    }
    void rechoke(uint64_t now) {
        if (!hasMeta) return;
        std::vector<BtPeer*> cand;
        for (auto& q : peers) if (q->handshook && !q->closing && q->peerInterested) cand.push_back(q.get());
        bool up = complete;
        std::sort(cand.begin(), cand.end(), [up](BtPeer* a, BtPeer* b) { return up ? a->upRate > b->upRate : a->downRate > b->downRate; });
        std::set<BtPeer*> un;
        for (size_t i = 0; i < cand.size() && i < 4; ++i) un.insert(cand[i]);
        bool optAlive = false;
        for (auto& q : peers) if (q->key == optimistic && !q->closing) optAlive = true;
        if (now - lastOpt >= 30000 || !optAlive) {
            std::vector<BtPeer*> rest;
            for (auto* p : cand) if (!un.count(p)) rest.push_back(p);
            optimistic = rest.empty() ? std::string() : rest[rnd() % rest.size()]->key;
            lastOpt = now;
        }
        for (auto& q : peers) {
            if (!q->handshook || q->closing) continue;
            bool u = un.count(q.get()) || (!optimistic.empty() && q->key == optimistic && q->peerInterested);
            if (u && q->amChoking) { q->amChoking = false; sendMsg(*q, 1); }
            else if (!u && !q->amChoking) { q->amChoking = true; sendMsg(*q, 0); q->uploads.clear(); }
        }
    }
    void secondTick(uint64_t now, double dt) {
        for (auto& q : peers) {
            BtPeer& p = *q;
            p.downRate = p.downWin / dt; p.upRate = p.upWin / dt;
            p.downWin = 0; p.upWin = 0;
            // Requests that never came back.
            for (size_t i = 0; i < p.reqs.size();) {
                if (now - p.reqs[i].t > 60000) { BtPeer::Req r = p.reqs[i]; p.reqs.erase(p.reqs.begin() + (long)i); releaseOne(p, r); p.snubbed = true; }
                else ++i;
            }
            if (!p.handshook && now - p.connected > 20000) p.closing = true;
            if (now - p.lastRecv > 180000) p.closing = true;
            if (hasMeta && complete && np && p.hasCount == np) p.closing = true; // two seeds have nothing to trade
            if (!hasMeta && p.handshook && p.extSeen && !p.utMeta) p.closing = true;
            if (!p.closing && now - p.lastSend > 100000) { p.out.append(4, '\0'); p.lastSend = now; }
        }
        std::vector<bt::Endpoint> fresh;
        { std::lock_guard<std::mutex> lk(tm); fresh.swap(trackerPeers); }
        if (dht) { auto d = dht->takePeers(hash); fresh.insert(fresh.end(), d.begin(), d.end()); }
        for (auto& ep : fresh) addCand(ep);
        connectMore(now);
        {
            std::lock_guard<std::mutex> lk(tm);
            annDown = rawDown;
            annLeft = hasMeta ? std::max<int64_t>(0, selSize - verifiedBytes) : 16384;
            annCompleted = complete && !completeAtStart;
            annNeed = !complete && (int)peers.size() < c.btMaxPeers / 2;
            std::lock_guard<std::mutex> l2(it.m);
            annUp = it.uploaded;
        }
        { std::lock_guard<std::mutex> lk(it.m); it.limiter.rate = it.limit; }
    }

    // ------------------------------------------------------------ trackers

    void trackerThread(std::string url) {
        std::string lu = lower(url);
        bool udp = startsWith(lu, "udp://"), web = startsWith(lu, "http://") || startsWith(lu, "https://");
        if ((!udp && !web) || (udp && viaTor)) return;
        bool started = false, sentCompleted = false;
        int fails = 0;
        uint64_t next = 0;
        while (!stopTrackers) {
            bool comp;
            { std::lock_guard<std::mutex> lk(tm); comp = annCompleted; }
            if (comp && started && !sentCompleted) next = 0;
            if (nowMs() >= next) {
                bt::Announce a;
                a.hash = hash; a.peerId = peerId; a.port = annPort; a.key = annKey;
                bool need;
                {
                    std::lock_guard<std::mutex> lk(tm);
                    a.up = annUp; a.down = annDown; a.left = annLeft;
                    need = annNeed;
                    a.event = !started ? 2 : (annCompleted && !sentCompleted ? 1 : 0);
                }
                a.numwant = need ? 80 : 30;
                bt::AnnounceResult r;
                bool ok;
                if (udp) ok = bt::announceUdp(url, a, &stopTrackers, r);
                else {
                    http::Options o;
                    o.conn.proxy = proxy; o.conn.timeoutMs = viaTor ? 60000 : 15000; o.conn.verifyTls = false; o.conn.cancel = &stopTrackers;
                    o.userAgent = "TDM/1.0";
                    o.maxRedirects = 3;
                    ok = bt::announceHttp(url, a, o, r);
                }
                if (stopTrackers) break;
                if (ok) {
                    fails = 0;
                    if (a.event == 2) started = true;
                    if (a.event == 1) sentCompleted = true;
                    if (!started) started = true;
                    std::lock_guard<std::mutex> lk(tm);
                    startedTrackers.insert(url);
                    trackerPeers.insert(trackerPeers.end(), r.peers.begin(), r.peers.end());
                    trackerErr.clear();
                    int iv = need ? std::min(r.interval, 300) : r.interval;
                    next = nowMs() + (uint64_t)std::max(60, iv) * 1000;
                } else {
                    ++fails;
                    std::lock_guard<std::mutex> lk(tm);
                    trackerErr = r.err;
                    next = nowMs() + std::min<uint64_t>(1800000, 30000ull << std::min(fails, 6));
                }
            }
            sleepFor(250, &stopTrackers);
        }
    }
    void startTrackers() {
        std::vector<std::string> trs;
        { std::lock_guard<std::mutex> lk(it.m); trs = it.trackers; }
        if (hasMeta) trs = mergeTrackers(trs, meta.trackers);
        if (trs.empty() && !priv) trs = fallbackTrackers();
        { std::lock_guard<std::mutex> lk(it.m); it.trackers = trs; }
        for (auto& t : trs) trackerThreads.emplace_back([this, t] { trackerThread(t); });
    }
    // Best-effort "stopped" announces after the job ends; they run detached so pausing is instant.
    void announceStopped() {
        std::set<std::string> urls;
        bt::Announce a;
        {
            std::lock_guard<std::mutex> lk(tm);
            urls = startedTrackers;
            a.up = annUp; a.down = annDown; a.left = annLeft;
        }
        a.hash = hash; a.peerId = peerId; a.port = annPort; a.key = annKey; a.event = 3; a.numwant = 0;
        net::Proxy px = proxy;
        bool tor = viaTor;
        for (auto& u : urls) {
            std::thread([u, a, px, tor] {
                bt::AnnounceResult r;
                if (startsWith(lower(u), "udp://")) { if (!tor) bt::announceUdp(u, a, nullptr, r); }
                else {
                    http::Options o;
                    o.conn.proxy = px; o.conn.timeoutMs = tor ? 30000 : 8000; o.conn.verifyTls = false; o.userAgent = "TDM/1.0";
                    bt::announceHttp(u, a, o, r);
                }
            }).detach();
        }
    }

    // ------------------------------------------------------------ UI state

    void publish(bool files) {
        int conns = 0, seedsN = 0;
        std::vector<PeerView> pv;
        for (auto& q : peers) {
            if (!q->handshook || q->closing) continue;
            ++conns;
            bool seed = hasMeta && np && q->hasCount == np;
            if (seed) ++seedsN;
            if (pv.size() < 200) {
                PeerView v;
                v.addr = q->key; v.client = q->client.empty() ? std::string("?") : q->client;
                v.down = q->downRate; v.up = q->upRate; v.seed = seed; v.choked = q->peerChoking; v.incoming = q->incoming;
                v.pct = hasMeta && np ? (int)((int64_t)q->hasCount * 100 / np) : 0;
                pv.push_back(v);
            }
        }
        std::string hx;
        if (hasMeta && haveDirty) { std::string b = bitfieldBytes(); hx = hex((const uint8_t*)b.data(), b.size()); }
        std::wstring note;
        uint64_t now = nowMs();
        if (!complete && conns == 0 && now - startedAt > 45000) {
            std::string te;
            { std::lock_guard<std::mutex> lk(tm); te = trackerErr; }
            note = hasMeta ? L"Looking for peers…" : L"Looking for peers that have the torrent's metadata…";
            if (!te.empty()) note += L" (tracker: " + widenLoose(te) + L")";
        }
        std::lock_guard<std::mutex> lk(it.m);
        it.conns = conns; it.peers = (int)peers.size(); it.seeds = seedsN;
        it.peerList.swap(pv);
        if (hasMeta) {
            it.size = selSize;
            it.done = std::min(selSize, verifiedBytes + partialBytes);
            if (files) fileProgress(meta, have, it.files);
        }
        it.uploaded += upDelta; upDelta = 0;
        if (!hx.empty()) { it.have = hx; haveDirty = false; }
        it.status = !hasMeta ? Metadata : complete ? Seeding : (conns > 0 ? Downloading : Starting);
        if (fatal.empty()) it.error = note;
        it.dirty = true;
    }

    void onComplete() {
        complete = true;
        store.finalizeSizes();
        for (auto& q : peers) { releaseReqs(*q); setInterest(*q, false); }
        if (!completeAtStart) {
            std::lock_guard<std::mutex> lk(it.m);
            it.finished = unixNow();
            finishedNow = true;
        }
        seeding = e.seedingWanted(it);
        if (!seeding) stopDone = true;
    }

    // ------------------------------------------------------------ main loop

    void run() {
        c = e.config();
        startedAt = nowMs();
        std::string hx;
        { std::lock_guard<std::mutex> lk(it.m); hx = it.infoHash; it.status = Starting; it.limiter.rate = it.limit; }
        if (!unhex(hx, hash) || hash.size() != 20) { e.finishWith(it, Failed, L"Bad info hash"); return; }
        std::wstring perr;
        if (!e.proxyFor(it, proxy, perr, &viaTor)) { e.finishWith(it, Failed, perr); return; }
        peerId = svc.peerId;
        if (!viaTor) listenPort = svc.listenPort(c.btPort);
        annPort = listenPort ? listenPort : c.btPort;
        std::string data, err;
        bt::Meta m;
        if (readWholeFile(torrentStorePath(e, hx), data) && bt::parseTorrent(data, m, err) && m.hash == hash) { meta = m; hasMeta = true; priv = meta.priv; }
        if (hasMeta) {
            if (!setupMeta()) { store.closeAll(); e.finishWith(it, it.cancel ? it.stopAs : Failed, it.cancel ? L"" : L"Cannot read the files"); return; }
        } else { std::lock_guard<std::mutex> lk(it.m); it.status = Metadata; it.dirty = true; }
        if (c.btDht && !viaTor && !priv) dht = svc.dhtFor(annPort);
        svc.add(hash, inbox);
        startTrackers();
        {
            // Peer hints from the magnet link (x.pe=host:port).
            std::string url;
            { std::lock_guard<std::mutex> lk(it.m); url = it.url; }
            bt::Magnet mg;
            bt::Endpoint ep;
            if (bt::parseMagnet(url, mg)) for (auto& pe : mg.peers) if (bt::parseHostPort(pe, ep)) addCand(ep);
        }
        if (hasMeta && missing == 0) { completeAtStart = true; onComplete(); }

        uint64_t lastSec = nowMs(), lastChoke = 0, lastUi = 0, lastFiles = 0, lastDht = 0, lastInterest = nowMs();
        while (!it.cancel && fatal.empty() && !stopDone) {
            uint64_t now = nowMs();
            for (auto& cn : inbox->take()) acceptConn(cn, now);
            pollOnce(40);
            if (it.selChanged.exchange(false)) selectionChanged();
            bool limited = !e.global.ready() || !it.limiter.ready();
            for (auto& q : peers) {
                if (q->closing) continue;
                fillRequests(*q, limited);
                serveUploads(*q);
                if (q->pending()) flushOut(*q);
            }
            if (!hasMeta) requestMetadata();
            dropClosed();
            if (hasMeta && !complete && missing == 0) onComplete();
            if (finishedNow) {
                finishedNow = false;
                publish(true);
                if (e.onFinished) e.onFinished(it.id);
            }
            now = nowMs();
            if (now - lastSec >= 1000) {
                secondTick(now, (now - lastSec) / 1000.0);
                lastSec = now;
                if (seeding && !e.seedingWanted(it)) { seeding = false; stopDone = true; }
            }
            if (now - lastInterest >= 5000) { lastInterest = now; for (auto& q : peers) if (q->handshook && !q->closing) updateInterest(*q); }
            if (now - lastChoke >= 10000) { lastChoke = now; rechoke(now); }
            if (dht && (!complete || seeding) && (lastDht == 0 || now - lastDht > (complete ? 900000ull : 180000ull))) {
                lastDht = now;
                dht->lookup(hash, annPort, listenPort > 0 && !priv);
            }
            if (now - lastUi >= 500) {
                bool files = now - lastFiles >= 2000;
                if (files) lastFiles = now;
                publish(files);
                lastUi = now;
            }
        }

        // Tear down.
        svc.remove(hash);
        inbox->close();
        stopTrackers = true;
        for (auto& t : trackerThreads) if (t.joinable()) t.join();
        for (auto& q : peers) closesocket(q->s);
        peers.clear();
        if (dht) dht->forget(hash);
        store.closeAll();
        { std::lock_guard<std::mutex> tl(tm); annUp = it.uploaded + upDelta; }
        announceStopped();
        publish(true);
        {
            std::lock_guard<std::mutex> lk(it.m);
            it.peerList.clear(); it.peers = 0; it.seeds = 0; it.upSpeed = 0;
            if (hasMeta) it.done = std::min(selSize, verifiedBytes);
        }
        if (!fatal.empty()) e.finishWith(it, Failed, fatal);
        else if (it.cancel) e.finishWith(it, it.stopAs == Paused && complete ? Completed : it.stopAs, L"");
        else e.finishWith(it, Completed, L"");
    }
};

inline void runTorrentItem(Engine& e, std::shared_ptr<Item> it) {
    TorrentJob job(e, it);
    job.run();
}

} // namespace engine
