// Download engine: items, queue scheduler, segmented HTTP downloads with resume, speed limits, Tor service.
#pragma once
#include "http.hpp"
#include <bcrypt.h>
#include <list>

namespace engine {
using namespace tdm;

enum Status { Queued = 0, Starting, Downloading, Paused, Completed, Failed, Checking, Seeding, Metadata, StatusCount };
enum Kind { KindHttp = 0, KindTorrent = 1 };
inline const wchar_t* statusName(int s) {
    static const wchar_t* n[] = {L"Queued", L"Connecting", L"Downloading", L"Paused", L"Completed", L"Failed", L"Checking", L"Seeding", L"Getting metadata"};
    return s >= 0 && s < StatusCount ? n[s] : L"";
}
inline bool isRunningStatus(int s) { return s == Starting || s == Downloading || s == Checking || s == Metadata || s == Seeding; }

struct Segment { int64_t start = 0, end = 0, pos = 0; bool active = false; };

// ---------------------------------------------------------------- categories

inline std::wstring categoryFor(const std::wstring& name, int kind) {
    if (kind == KindTorrent) return L"Torrents";
    std::wstring e = extensionOf(name);
    auto in = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (e == x) return true; return false; };
    if (in({L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"tgz", L"zst", L"iso", L"cab", L"lz", L"lzma", L"img"})) return L"Compressed";
    if (in({L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"flv", L"m4v", L"mpg", L"mpeg", L"ts", L"3gp", L"m2ts"})) return L"Video";
    if (in({L"mp3", L"flac", L"wav", L"aac", L"ogg", L"m4a", L"wma", L"opus", L"alac", L"aiff"})) return L"Music";
    if (in({L"exe", L"msi", L"msix", L"appx", L"apk", L"dmg", L"deb", L"rpm", L"bat", L"ps1", L"jar", L"appimage", L"msixbundle"})) return L"Programs";
    if (in({L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt", L"rtf", L"odt", L"ods", L"epub", L"csv", L"md", L"mobi", L"json", L"xml"})) return L"Documents";
    if (in({L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"svg", L"tif", L"tiff", L"heic", L"ico", L"raw"})) return L"Images";
    return L"Other";
}
inline const std::vector<std::wstring>& categories() {
    static std::vector<std::wstring> c{L"Compressed", L"Documents", L"Music", L"Video", L"Images", L"Programs", L"Torrents", L"Other"};
    return c;
}

// ---------------------------------------------------------------- configuration

struct Config {
    std::wstring folder;              // default download folder
    int maxActive = 4;                // downloads running at once
    int connections = 8;              // per HTTP download
    int64_t speedLimit = 0;           // bytes/s for all downloads, 0 = unlimited
    int retries = 12;
    int timeoutSec = 30;
    int64_t minSplit = 256 * 1024;    // smallest segment worth a new connection
    std::string userAgent = http::Options().userAgent;
    bool verifyTls = true;
    bool preallocate = true;
    bool categoryFolders = false;     // save into <folder>\<Category>
    net::Proxy proxy;                 // for regular downloads
    // Tor
    std::string torHost = "127.0.0.1";
    int torPort = 0;                  // 0 = auto-detect 9050 (tor) then 9150 (Tor Browser)
    bool torLaunch = false;           // start tor.exe ourselves
    std::wstring torPath;
    int torConnections = 4;
    bool torAll = false;              // route every download through Tor
    bool torIsolate = true;           // separate circuit per connection (SOCKS auth isolation)
    // BitTorrent
    int btPort = 6881;
    int btMaxPeers = 50;
    bool btDht = true;
    bool btUpnp = false;
    double seedRatio = 1.0;           // stop seeding at this ratio, 0 = do not seed
    int64_t uploadLimit = 0;          // bytes/s, 0 = unlimited
    bool btViaTor = false;            // trackers and peers through the Tor SOCKS proxy (TCP only, no DHT)
};

// ---------------------------------------------------------------- rate limiting

class Limiter {
    std::mutex m;
    double tokens = 0;
    uint64_t last = 0;
    void refill(int64_t r) {
        uint64_t now = nowMs();
        if (!last) last = now;
        tokens = std::min<double>((double)r * 0.5, tokens + (double)(now - last) * (double)r / 1000.0);
        last = now;
    }
public:
    std::atomic<int64_t> rate{0}; // bytes per second, 0 unlimited
    // Blocks until n bytes may pass. Returns false when cancelled.
    bool take(int64_t n, const std::atomic<bool>* cancel) {
        int64_t r = rate;
        if (r <= 0 || n <= 0) return true;
        double wait;
        {
            std::lock_guard<std::mutex> lk(m);
            refill(r);
            tokens -= (double)n;
            wait = tokens < 0 ? -tokens * 1000.0 / (double)r : 0;
        }
        if (wait > 0) return sleepFor((int)std::min(wait, 5000.0), cancel);
        return true;
    }
    // Non-blocking use (the torrent event loop): account for bytes, and ask whether more may pass now.
    void consume(int64_t n) { int64_t r = rate; if (r <= 0) return; std::lock_guard<std::mutex> lk(m); refill(r); tokens -= (double)n; }
    bool ready() { int64_t r = rate; if (r <= 0) return true; std::lock_guard<std::mutex> lk(m); refill(r); return tokens > 0; }
};

// ---------------------------------------------------------------- items

struct TorrentFile { std::wstring path; int64_t size = 0, offset = 0; bool wanted = true; int64_t done = 0; };
struct PeerView { std::string addr, client; double down = 0, up = 0; bool seed = false, choked = true, incoming = false; int pct = 0; };

struct Item {
    // Persistent fields (settings.hpp saves them).
    uint32_t id = 0;
    int kind = KindHttp;
    std::string url;                 // HTTP URL, or the magnet link for torrents
    std::wstring name, folder;
    bool userName = false;           // name chosen by the user: do not replace from headers
    std::string referer, cookie, user, pass;
    int64_t size = -1, done = 0;
    bool resumable = false;
    std::string etag, lastModified;
    int status = Queued;
    std::wstring error;
    int connections = 0;             // 0 = use Config
    int tor = 0;                     // 0 auto (onion or "route all"), 1 always, 2 never
    int64_t added = 0, finished = 0;
    int64_t limit = 0;               // bytes/s for this item, 0 = unlimited
    std::wstring category;
    std::string checksum;            // "sha256:hex", "sha1:hex" or "md5:hex"
    std::wstring checkResult;
    std::vector<Segment> segs;
    // Torrent persistent fields.
    std::string infoHash;            // 40 hex chars
    std::string have;                // piece bitfield, hex
    std::string fileSel;             // '1'/'0' per file
    int64_t uploaded = 0;
    std::vector<std::string> trackers;

    // Runtime.
    std::mutex m;
    std::atomic<bool> cancel{false};
    std::atomic<bool> running{false};
    std::thread th;
    int stopAs = Paused;             // status to set when the job stops because of cancel
    bool force = false;              // start even when the queue is full
    bool removed = false, deleteFiles = false;
    double speed = 0, upSpeed = 0;
    int64_t lastDone = 0, lastUp = 0;
    int conns = 0, peers = 0, seeds = 0;
    std::string finalUrl;
    std::wstring torrentName;
    std::vector<TorrentFile> files;
    int pieceCount = 0; int64_t pieceLen = 0;
    std::vector<PeerView> peerList;
    Limiter limiter;
    std::atomic<bool> selChanged{false};
    bool dirty = true;

    std::wstring path() const { return joinPath(folder, name); }
    std::wstring partPath() const { return path() + L".tdm"; }
};

// A copy of the fields the UI shows.
struct View {
    uint32_t id; int kind, status, conns, peers, seeds, tor;
    std::wstring name, folder, category, error, checkResult;
    std::string url, finalUrl, infoHash;
    int64_t size, done, added, finished, uploaded, limit;
    double speed, upSpeed;
    bool resumable;
    std::vector<Segment> segs;
    std::string have;
    int pieceCount;
    std::vector<TorrentFile> files;
    std::vector<PeerView> peerList;
    std::wstring path() const { return joinPath(folder, name); }
};

struct AddRequest {
    std::string url;
    std::wstring name, folder;
    std::string referer, cookie, user, pass, checksum;
    int connections = 0, tor = 0;
    int64_t limit = 0;
    bool start = true;
    // Torrent: raw .torrent file contents (empty for magnets and HTTP).
    std::string torrentData;
    std::string fileSel;
};

// ---------------------------------------------------------------- hashing

inline std::string hashFile(const std::wstring& path, const std::string& algo, const std::atomic<bool>* cancel, std::wstring& err) {
    LPCWSTR id = algo == "sha256" ? BCRYPT_SHA256_ALGORITHM : algo == "sha1" ? BCRYPT_SHA1_ALGORITHM : algo == "md5" ? BCRYPT_MD5_ALGORITHM : algo == "sha512" ? BCRYPT_SHA512_ALGORITHM : nullptr;
    if (!id) { err = L"Unknown checksum type"; return {}; }
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, id, nullptr, 0) != 0) { err = L"Hash provider unavailable"; return {}; }
    DWORD len = 0, got = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof len, &got, 0);
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    std::string out;
    if (f == INVALID_HANDLE_VALUE) err = errorText(GetLastError());
    else {
        std::vector<uint8_t> buf(1 << 20);
        DWORD n = 0;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) {
            if (cancel && *cancel) { err = L"Cancelled"; break; }
            BCryptHashData(h, buf.data(), n, 0);
        }
        CloseHandle(f);
        if (err.empty()) { std::vector<uint8_t> d(len); BCryptFinishHash(h, d.data(), len, 0); out = hex(d.data(), d.size()); }
    }
    BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}
// Accepts "sha256:abc…", "abc…" (type from length) or "SHA-256 abc…".
inline bool parseChecksum(const std::string& in, std::string& algo, std::string& digest) {
    std::string s = lower(trim(in));
    size_t sep = s.find_first_of(": =");
    std::string a = sep == std::string::npos ? "" : s.substr(0, sep), d = sep == std::string::npos ? s : trim(s.substr(sep + 1));
    a.erase(std::remove(a.begin(), a.end(), '-'), a.end());
    for (char c : d) if (hexVal(c) < 0) return false;
    if (a.empty()) a = d.size() == 64 ? "sha256" : d.size() == 40 ? "sha1" : d.size() == 32 ? "md5" : d.size() == 128 ? "sha512" : "";
    size_t want = a == "sha256" ? 64 : a == "sha1" ? 40 : a == "md5" ? 32 : a == "sha512" ? 128 : 0;
    if (!want || d.size() != want) return false;
    algo = a; digest = d;
    return true;
}

// ---------------------------------------------------------------- Tor

class TorService {
public:
    std::mutex m;
    int state = 0;           // 0 unknown/off, 1 starting, 2 ready, 3 failed
    int progress = 0;        // bootstrap %
    int port = 0;            // SOCKS port in use
    std::wstring message;
    HANDLE proc = nullptr, job = nullptr;
    std::thread reader;
    std::function<void()> notify;

    // A SOCKS5 greeting succeeds at host:port.
    static bool probe(const std::string& host, int port) {
        net::Tcp t; t.timeoutMs = 1500;
        if (!t.connectTo(host, port)) return false;
        if (!t.write(std::string("\x05\x01\x00", 3))) return false;
        char r[2];
        return t.readExact(r, 2) && r[0] == 5 && r[1] == 0;
    }
    bool ownProcessAlive() { return proc && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT; }
    void setState(int s, int p, const std::wstring& msg) {
        { std::lock_guard<std::mutex> lk(m); state = s; progress = p; message = msg; }
        if (notify) notify();
    }
    bool launch(const std::wstring& exe, int socksPort, const std::wstring& dataDir) {
        stop();
        if (exe.empty() || !fileExists(exe)) { setState(3, 0, L"tor.exe not found. Set its path in Settings."); return false; }
        makeDirs(dataDir);
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe + L"\" --SocksPort " + std::to_wstring(socksPort) + L" --DataDirectory \"" + dataDir + L"\" --ClientOnly 1 --Log \"notice stdout\"";
        std::vector<wchar_t> cl(cmd.begin(), cmd.end()); cl.push_back(0);
        std::wstring dir = folderOf(exe);
        if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
            DWORD e = GetLastError();
            CloseHandle(rd); CloseHandle(wr);
            setState(3, 0, L"Cannot start tor.exe: " + errorText(e));
            return false;
        }
        // Kill tor with us, even if we crash.
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread); CloseHandle(wr);
        proc = pi.hProcess;
        { std::lock_guard<std::mutex> lk(m); port = socksPort; }
        setState(1, 0, L"Starting Tor…");
        reader = std::thread([this, rd] {
            std::string acc; char buf[2048]; DWORD n = 0;
            while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
                acc.append(buf, n);
                size_t nl;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    std::string line = trim(acc.substr(0, nl)); acc.erase(0, nl + 1);
                    size_t b = line.find("Bootstrapped ");
                    if (b != std::string::npos) {
                        int pct = atoi(line.c_str() + b + 13);
                        size_t colon = line.find(": ", b);
                        std::wstring what = colon == std::string::npos ? L"" : widen(line.substr(colon + 2));
                        setState(pct >= 100 ? 2 : 1, pct, pct >= 100 ? L"Tor is ready" : L"Tor " + std::to_wstring(pct) + L"%: " + what);
                    } else if (line.find("[err]") != std::string::npos) setState(3, 0, widen(line.substr(line.find("[err]") + 6)));
                }
            }
            CloseHandle(rd);
            std::lock_guard<std::mutex> lk(m);
            if (state != 3) { state = 0; message = L"Tor stopped"; }
        });
        return true;
    }
    void stop() {
        if (proc) { TerminateProcess(proc, 0); WaitForSingleObject(proc, 3000); CloseHandle(proc); proc = nullptr; }
        if (reader.joinable()) reader.join();
        if (job) { CloseHandle(job); job = nullptr; }
    }
    ~TorService() { stop(); }
};

// ---------------------------------------------------------------- engine

class Engine;
void runTorrentItem(Engine& e, std::shared_ptr<Item> it); // torrent_job.hpp
bool prepareTorrentItem(Engine& e, Item& it, const AddRequest& r, std::wstring& err); // torrent_job.hpp

class Engine {
public:
    std::mutex cfgMutex;
    Config cfg;
    std::mutex m;                                  // guards `items` (the vector, not the items)
    std::vector<std::shared_ptr<Item>> items;
    uint32_t nextId = 1;
    Limiter global, globalUp;
    TorService tor;
    std::function<void()> notify;                  // something changed (coalesced by the UI)
    std::function<void(uint32_t)> onFinished;      // a download completed
    std::function<void()> save;                    // persist now (called from the manager thread)
    std::wstring dataDir;                          // settings folder (torrent files live here)
    std::atomic<bool> quitting{false};
    std::thread manager;
    std::atomic<int64_t> totalDown{0}, totalUp{0};

    Config config() { std::lock_guard<std::mutex> lk(cfgMutex); return cfg; }
    void setConfig(const Config& c) {
        { std::lock_guard<std::mutex> lk(cfgMutex); cfg = c; }
        global.rate = c.speedLimit; globalUp.rate = c.uploadLimit;
        changed();
    }
    void changed() { if (notify) notify(); }

    void start() {
        net::init();
        Config c = config();
        global.rate = c.speedLimit; globalUp.rate = c.uploadLimit;
        tor.notify = [this] { changed(); };
        manager = std::thread([this] { loop(); });
    }
    void shutdown() {
        quitting = true;
        {
            std::lock_guard<std::mutex> lk(m);
            for (auto& it : items) if (it->running) { it->stopAs = it->status == Seeding ? Completed : (it->status == Paused ? Paused : Queued); it->cancel = true; }
        }
        if (manager.joinable()) manager.join();
        std::vector<std::shared_ptr<Item>> all;
        { std::lock_guard<std::mutex> lk(m); all = items; }
        for (auto& it : all) if (it->th.joinable()) it->th.join();
        tor.stop();
    }

    std::shared_ptr<Item> find(uint32_t id) {
        std::lock_guard<std::mutex> lk(m);
        for (auto& it : items) if (it->id == id) return it;
        return nullptr;
    }
    // Paths reserved by other items, so two downloads never pick the same file name.
    bool pathTaken(const std::wstring& p, uint32_t except) {
        std::lock_guard<std::mutex> lk(m);
        for (auto& it : items) if (it->id != except && it->kind == KindHttp && !it->removed && _wcsicmp(it->path().c_str(), p.c_str()) == 0) return true;
        return false;
    }
    std::wstring folderFor(const std::wstring& base, const std::wstring& category) {
        Config c = config();
        std::wstring f = base.empty() ? c.folder : base;
        if (c.categoryFolders && base.empty() && !category.empty()) f = joinPath(f, category);
        return f;
    }

    // Adds an item restored from the downloads file.
    void restore(std::shared_ptr<Item> it) {
        std::lock_guard<std::mutex> lk(m);
        if (it->id >= nextId) nextId = it->id + 1;
        if (isRunningStatus(it->status)) it->status = Queued; // seeding torrents resume seeding
        items.push_back(it);
    }

    uint32_t add(const AddRequest& r, std::wstring& err);
    void resume(uint32_t id, bool force = true) {
        auto it = find(id);
        if (!it || it->removed) return;
        std::lock_guard<std::mutex> lk(it->m);
        if (it->running) { if (it->cancel) { it->stopAs = Queued; it->force = force; } return; }
        if (it->status == Completed && it->kind != KindTorrent) return;
        if (it->status == Completed && it->kind == KindTorrent) { if (!seedingWanted(*it)) return; }
        it->status = Queued; it->force = force; it->error.clear(); it->dirty = true;
        changed();
    }
    void pause(uint32_t id) {
        auto it = find(id);
        if (!it) return;
        std::lock_guard<std::mutex> lk(it->m);
        if (it->running) { it->stopAs = it->status == Seeding ? Completed : Paused; it->cancel = true; }
        else if (it->status == Queued || it->status == Failed) it->status = Paused;
        it->dirty = true;
        changed();
    }
    void remove(uint32_t id, bool deleteFiles) {
        auto it = find(id);
        if (!it) return;
        { std::lock_guard<std::mutex> lk(it->m); it->removed = true; it->deleteFiles = deleteFiles; it->cancel = true; }
        changed();
    }
    // Restart from zero (keeps the URL and name).
    void restart(uint32_t id) {
        auto it = find(id);
        if (!it || it->running || it->kind != KindHttp) return;
        std::lock_guard<std::mutex> lk(it->m);
        DeleteFileW(it->partPath().c_str());
        it->segs.clear(); it->done = 0; it->size = -1; it->status = Queued; it->error.clear(); it->force = true; it->finished = 0; it->checkResult.clear(); it->dirty = true;
        changed();
    }
    void move(uint32_t id, int delta) {
        std::lock_guard<std::mutex> lk(m);
        for (size_t i = 0; i < items.size(); ++i) if (items[i]->id == id) {
            int j = (int)i + delta;
            if (delta == -1000) j = 0;
            if (delta == 1000) j = (int)items.size() - 1;
            j = std::max(0, std::min(j, (int)items.size() - 1));
            auto x = items[i]; items.erase(items.begin() + (long)i); items.insert(items.begin() + j, x);
            x->dirty = true;
            break;
        }
        changed();
    }
    // Torrent file selection: '1'/'0' per file.
    void setFileSel(uint32_t id, const std::string& sel) {
        auto it = find(id);
        if (!it || it->kind != KindTorrent) return;
        std::lock_guard<std::mutex> lk(it->m);
        if (sel.size() != it->files.size()) return;
        it->fileSel = sel;
        it->size = 0;
        for (size_t i = 0; i < it->files.size(); ++i) { it->files[i].wanted = sel[i] == '1'; if (it->files[i].wanted) it->size += it->files[i].size; }
        it->selChanged = true;
        if (!it->running && it->status == Completed) { it->status = Queued; it->force = true; }
        it->dirty = true;
        changed();
    }
    // Verify a torrent's data again on its next start.
    void recheck(uint32_t id) {
        auto it = find(id);
        if (!it || it->kind != KindTorrent) return;
        std::lock_guard<std::mutex> lk(it->m);
        if (it->running) return;
        it->have.clear(); it->done = 0;
        it->status = Queued; it->force = true; it->error.clear(); it->dirty = true;
        changed();
    }
    void setUrl(uint32_t id, const std::string& url) {
        auto it = find(id);
        if (!it || it->kind != KindHttp) return;
        std::lock_guard<std::mutex> lk(it->m);
        it->url = url; it->finalUrl.clear(); it->dirty = true;
    }

    std::vector<View> snapshot(bool details = false, uint32_t detailId = 0) {
        std::vector<std::shared_ptr<Item>> all;
        { std::lock_guard<std::mutex> lk(m); all = items; }
        std::vector<View> v;
        v.reserve(all.size());
        for (auto& it : all) {
            if (it->removed) continue;
            std::lock_guard<std::mutex> lk(it->m);
            View x{};
            x.id = it->id; x.kind = it->kind; x.status = it->status; x.conns = it->conns; x.peers = it->peers; x.seeds = it->seeds; x.tor = it->tor;
            x.name = it->name; x.folder = it->folder; x.category = it->category; x.error = it->error; x.checkResult = it->checkResult;
            x.url = it->url; x.finalUrl = it->finalUrl; x.infoHash = it->infoHash;
            x.size = it->size; x.done = it->done; x.added = it->added; x.finished = it->finished; x.uploaded = it->uploaded; x.limit = it->limit;
            x.speed = it->speed; x.upSpeed = it->upSpeed; x.resumable = it->resumable; x.pieceCount = it->pieceCount;
            if (details && (detailId == 0 || detailId == it->id)) { x.segs = it->segs; x.have = it->have; x.files = it->files; x.peerList = it->peerList; }
            v.push_back(std::move(x));
        }
        return v;
    }
    bool seedingWanted(const Item& it) { Config c = config(); return c.seedRatio > 0 && (it.size <= 0 || (double)it.uploaded < c.seedRatio * (double)it.size); }

    // Proxy for an item: Tor, the configured proxy, or none. Tor port 0 means auto-detect.
    bool proxyFor(Item& it, net::Proxy& p, std::wstring& err, bool* usedTor = nullptr) {
        Config c = config();
        if (usedTor) *usedTor = false;
        net::Url u; net::parseUrl(it.url, u);
        bool useTor = it.tor == 1 || (it.tor == 0 && (c.torAll || u.onion()));
        if (it.kind == KindTorrent) useTor = c.btViaTor || it.tor == 1;
        if (!useTor) { p = c.proxy; return true; }
        int port = torPort(c, err);
        if (!port) return false;
        p.kind = net::Proxy::Socks5; p.host = c.torHost; p.port = port;
        if (usedTor) *usedTor = true;
        return true;
    }
    int torPort(const Config& c, std::wstring& err) {
        {
            std::lock_guard<std::mutex> lk(tor.m);
            if (tor.state == 2 && tor.port) return tor.port;
        }
        if (c.torLaunch) {
            int want = c.torPort ? c.torPort : 9050;
            if (TorService::probe(c.torHost, want)) { std::lock_guard<std::mutex> lk(tor.m); tor.port = want; tor.state = 2; tor.message = L"Tor is ready"; return want; }
            static std::mutex launchMutex;
            std::lock_guard<std::mutex> ll(launchMutex);
            if (!tor.ownProcessAlive()) tor.launch(c.torPath, want, joinPath(dataDir, L"tor-data"));
            for (int i = 0; i < 240 && !quitting; ++i) {
                {
                    std::lock_guard<std::mutex> lk(tor.m);
                    if (tor.state == 2) return tor.port;
                    if (tor.state == 3) { err = tor.message; return 0; }
                }
                Sleep(500);
            }
            err = L"Tor did not finish starting";
            return 0;
        }
        for (int p : c.torPort ? std::vector<int>{c.torPort} : std::vector<int>{9050, 9150}) {
            if (TorService::probe(c.torHost, p)) {
                std::lock_guard<std::mutex> lk(tor.m);
                tor.port = p; tor.state = 2; tor.message = p == 9150 ? L"Using Tor Browser" : L"Tor is ready";
                return p;
            }
        }
        err = L"Tor is not running. Start Tor Browser, or set tor.exe in Settings so TDM can start it.";
        { std::lock_guard<std::mutex> lk(tor.m); tor.state = 3; tor.message = L"Tor not found"; }
        return 0;
    }
    // Re-checks Tor on demand (Settings › Test).
    void forgetTor() { std::lock_guard<std::mutex> lk(tor.m); if (!tor.ownProcessAlive()) { tor.state = 0; tor.port = 0; tor.message.clear(); } }

    http::Options httpOptions(Item& it, const net::Proxy& p) {
        Config c = config();
        http::Options o;
        o.userAgent = c.userAgent.empty() ? o.userAgent : c.userAgent;
        o.referer = it.referer; o.cookie = it.cookie; o.user = it.user; o.pass = it.pass;
        o.conn.proxy = p; o.conn.timeoutMs = c.timeoutSec * 1000; o.conn.verifyTls = c.verifyTls; o.conn.cancel = &it.cancel;
        return o;
    }

private:
    void loop() {
        uint64_t lastTick = nowMs(), lastSave = nowMs();
        while (!quitting) {
            Sleep(100);
            uint64_t now = nowMs();
            reap();
            schedule();
            if (now - lastTick >= 500) {
                double dt = (now - lastTick) / 1000.0;
                lastTick = now;
                speeds(dt);
                changed();
            }
            if (now - lastSave >= 3000) { lastSave = now; if (save) save(); }
        }
        if (save) save();
    }
    void speeds(double dt) {
        std::vector<std::shared_ptr<Item>> all;
        { std::lock_guard<std::mutex> lk(m); all = items; }
        for (auto& it : all) {
            std::lock_guard<std::mutex> lk(it->m);
            int64_t d = it->done - it->lastDone, u = it->uploaded - it->lastUp;
            it->lastDone = it->done; it->lastUp = it->uploaded;
            if (d < 0) d = 0;
            if (u < 0) u = 0;
            double inst = d / dt, instUp = u / dt;
            it->speed = it->running ? (it->speed == 0 ? inst : it->speed * 0.6 + inst * 0.4) : 0;
            it->upSpeed = it->running ? (it->upSpeed == 0 ? instUp : it->upSpeed * 0.6 + instUp * 0.4) : 0;
            if (it->speed < 1) it->speed = 0;
            if (it->upSpeed < 1) it->upSpeed = 0;
            if (d || u) it->dirty = true;
        }
    }
    // Joins finished job threads and deletes removed items.
    void reap() {
        std::vector<std::shared_ptr<Item>> all;
        { std::lock_guard<std::mutex> lk(m); all = items; }
        bool any = false;
        for (auto& it : all) {
            if (it->running || !it->th.joinable()) {
                if (it->removed && !it->running && !it->th.joinable()) { erase(it); any = true; }
                continue;
            }
            it->th.join();
            any = true;
            if (it->removed) erase(it);
        }
        if (any) changed();
    }
    void erase(std::shared_ptr<Item> it) {
        if (it->deleteFiles) {
            if (it->kind == KindHttp) { DeleteFileW(it->partPath().c_str()); if (it->status == Completed) DeleteFileW(it->path().c_str()); }
            else {
                for (auto& f : it->files) { std::wstring p = joinPath(it->folder, f.path); DeleteFileW(p.c_str()); DeleteFileW((p + L".tdm").c_str()); }
                // Remove now-empty folders of a multi-file torrent.
                if (!it->torrentName.empty() && it->files.size() > 1) {
                    std::vector<std::wstring> dirs;
                    for (auto& f : it->files) for (std::wstring d = folderOf(joinPath(it->folder, f.path)); d.size() > it->folder.size(); d = folderOf(d)) dirs.push_back(d);
                    std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
                    for (auto& d : dirs) RemoveDirectoryW(d.c_str());
                }
            }
        }
        if (!it->infoHash.empty()) {
            bool other = false;
            { std::lock_guard<std::mutex> lk(m); for (auto& x : items) if (x != it && x->infoHash == it->infoHash) other = true; }
            if (!other) DeleteFileW(joinPath(joinPath(dataDir, L"torrents"), widen(it->infoHash) + L".torrent").c_str());
        }
        std::lock_guard<std::mutex> lk(m);
        items.erase(std::remove(items.begin(), items.end(), it), items.end());
    }
    void schedule() {
        if (quitting) return;
        Config c = config();
        std::vector<std::shared_ptr<Item>> all;
        { std::lock_guard<std::mutex> lk(m); all = items; }
        int active = 0;
        for (auto& it : all) { if (it->running && it->status != Seeding && !it->removed) ++active; }
        for (auto& it : all) {
            if (it->removed || it->running || it->th.joinable()) continue;
            bool go;
            {
                std::lock_guard<std::mutex> lk(it->m);
                if (it->status != Queued) continue;
                go = it->force || active < c.maxActive;
                if (!go) continue;
                it->force = false;
                it->cancel = false;
                it->stopAs = Paused;
                it->running = true;
                it->status = Starting;
                it->error.clear();
            }
            ++active;
            it->th = std::thread([this, it] { runItem(it); });
        }
    }
    void runItem(std::shared_ptr<Item> it) {
        if (it->kind == KindTorrent) runTorrentItem(*this, it);
        else runHttp(it);
        { std::lock_guard<std::mutex> lk(it->m); it->conns = 0; it->dirty = true; it->running = false; }
        changed();
    }

    // ------------------------------------------------------------ HTTP job

    struct Job {
        Engine* e;
        std::shared_ptr<Item> it;
        HANDLE file = INVALID_HANDLE_VALUE;
        net::Url url;
        http::Options opt;
        bool torProxy = false;
        std::atomic<bool> failed{false};
        std::wstring failMsg;
        std::mutex failMutex;
        std::atomic<bool> restartFromZero{false};
        std::atomic<int> active{0};
    };

    void fail(Job& j, const std::wstring& msg) {
        { std::lock_guard<std::mutex> lk(j.failMutex); if (!j.failed) j.failMsg = msg; }
        j.failed = true;
        j.it->cancel = true;
    }

    int claim(Job& j) {
        Item& it = *j.it;
        Config c = config();
        std::lock_guard<std::mutex> lk(it.m);
        for (size_t i = 0; i < it.segs.size(); ++i) if (!it.segs[i].active && it.segs[i].pos < it.segs[i].end) { it.segs[i].active = true; return (int)i; }
        if (!it.resumable || it.size < 0) return -1;
        int best = -1; int64_t rem = 0;
        for (size_t i = 0; i < it.segs.size(); ++i) {
            const Segment& s = it.segs[i];
            if (s.active && s.end - s.pos > rem) { rem = s.end - s.pos; best = (int)i; }
        }
        if (best < 0 || rem < 2 * c.minSplit) return -1;
        Segment& s = it.segs[best];
        int64_t mid = s.pos + rem / 2;
        mid -= mid % 65536;
        if (mid <= s.pos + c.minSplit / 2) return -1;
        Segment n; n.start = mid; n.end = s.end; n.pos = mid; n.active = true;
        s.end = mid;
        it.segs.push_back(n);
        it.dirty = true;
        return (int)it.segs.size() - 1;
    }

    static bool writeAt(HANDLE f, int64_t off, const char* p, size_t n) {
        while (n) {
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(off & 0xFFFFFFFF); ov.OffsetHigh = (DWORD)(off >> 32);
            DWORD w = 0, k = (DWORD)std::min<size_t>(n, 1 << 24);
            if (!WriteFile(f, p, k, &w, &ov) || w == 0) return false;
            p += w; n -= w; off += w;
        }
        return true;
    }

    void worker(Job& j, int wi, int seg, std::unique_ptr<http::Body> body) {
        Item& it = *j.it;
        Config c = config();
        http::Options opt = j.opt;
        if (j.torProxy && c.torIsolate) { opt.conn.proxy.user = "tdm-" + std::to_string(it.id) + "-" + std::to_string(wi) + "-" + randomToken(4); opt.conn.proxy.pass = "x"; }
        std::vector<char> buf(512 * 1024);
        int retries = 0;
        bool counted = false;
        auto setConn = [&](bool on) {
            if (on == counted) return;
            counted = on;
            std::lock_guard<std::mutex> lk(it.m);
            it.conns += on ? 1 : -1;
            if (it.conns > 0 && it.status == Starting) it.status = Downloading;
        };
        while (!it.cancel) {
            if (seg < 0) { seg = claim(j); if (seg < 0) break; }
            int64_t pos, end;
            { std::lock_guard<std::mutex> lk(it.m); pos = it.segs[seg].pos; end = it.segs[seg].end; }
            if (pos >= end) { std::lock_guard<std::mutex> lk(it.m); it.segs[seg].active = false; seg = -1; continue; }
            std::string err;
            if (!body) {
                std::string ifRange = !it.etag.empty() ? it.etag : it.lastModified;
                body = http::request(j.url, opt, it.resumable || pos > 0 ? pos : -1, -1, err, it.resumable ? ifRange : "");
                if (body) {
                    const http::Response& r = body->r;
                    if (r.status == 206) {
                        if (r.rangeStart != pos || (it.size >= 0 && r.total >= 0 && r.total != it.size)) {
                            if (r.total >= 0 && r.total != it.size) { j.restartFromZero = true; fail(j, L"The file changed on the server"); break; }
                            err = "Server returned the wrong range"; body.reset();
                        }
                    } else if (r.status == 200) {
                        if (pos != 0) {
                            if (it.resumable && (!it.etag.empty() || !it.lastModified.empty())) { j.restartFromZero = true; fail(j, L"The file changed on the server"); break; }
                            if (wi != 0) { body.reset(); break; } // extra connections are not possible without ranges
                            j.restartFromZero = true; fail(j, L"Server no longer supports resuming"); break;
                        }
                    } else if (r.status == 416 && it.size >= 0 && pos >= it.size) {
                        std::lock_guard<std::mutex> lk(it.m); it.segs[seg].pos = it.segs[seg].end; body.reset(); continue;
                    } else {
                        err = http::statusText(r.status, r.reason);
                        bool fatal = r.status == 404 || r.status == 410 || (r.status >= 400 && r.status < 500 && r.status != 408 && r.status != 429 && retries >= 2);
                        body.reset();
                        if (fatal) { fail(j, widen(err)); break; }
                    }
                }
            }
            if (body) {
                setConn(true);
                size_t fill = 0;
                int64_t wpos = pos;
                uint64_t lastFlush = nowMs();
                bool eof = false, segDone = false;
                auto flush = [&]() -> bool {
                    if (!fill) return true;
                    int64_t e2;
                    { std::lock_guard<std::mutex> lk(it.m); e2 = it.segs[seg].end; }
                    // The segment may have been split below what is buffered; never let the length go negative.
                    int64_t room = e2 - wpos;
                    size_t n = room > 0 ? (size_t)std::min<int64_t>((int64_t)fill, room) : 0;
                    if (n && !writeAt(j.file, wpos, buf.data(), n)) { fail(j, L"Cannot write to disk: " + errorText(GetLastError())); return false; }
                    int64_t e3;
                    {
                        // claim() may have split this segment while the write was in flight; bytes past the new end
                        // belong to the new segment, which fetches them itself, so only count up to that end.
                        std::lock_guard<std::mutex> lk(it.m);
                        Segment& s = it.segs[seg];
                        e3 = s.end;
                        int64_t np = std::min<int64_t>(wpos + (int64_t)n, e3);
                        if (np > s.pos) { it.done += np - s.pos; s.pos = np; }
                        it.dirty = true;
                    }
                    totalDown += (int64_t)n;
                    wpos += (int64_t)n;
                    fill = 0;
                    lastFlush = nowMs();
                    if (wpos >= e3) segDone = true;
                    return true;
                };
                while (!it.cancel) {
                    int want = (int)std::min<size_t>(buf.size() - fill, 128 * 1024);
                    int k = body->read(buf.data() + fill, want);
                    if (k < 0) { err = body->err.empty() ? "Connection lost" : body->err; break; }
                    if (k == 0) { eof = true; break; }
                    fill += (size_t)k;
                    if (!global.take(k, &it.cancel) || !it.limiter.take(k, &it.cancel)) break;
                    if (fill + 128 * 1024 > buf.size() || nowMs() - lastFlush > 250) {
                        if (!flush()) break;
                        if (segDone) break;
                        retries = 0;
                    }
                }
                if (!j.failed) flush();
                body.reset();
                if (j.failed) break;
                if (eof && !segDone) {
                    std::lock_guard<std::mutex> lk(it.m);
                    Segment& s = it.segs[seg];
                    if (it.size < 0) { s.end = s.pos; it.size = it.done; segDone = true; } // unknown length: the stream end is the file end
                    else if (s.pos >= s.end) segDone = true;
                    else if (!it.resumable) { err = "Connection closed before the download finished"; }
                    else err = "Connection closed early";
                }
                if (segDone) { std::lock_guard<std::mutex> lk(it.m); it.segs[seg].active = false; seg = -1; continue; }
                if (it.cancel) break;
            }
            setConn(false);
            if (it.cancel) break;
            if (++retries > c.retries) { fail(j, widen(err.empty() ? "Too many failed attempts" : err)); break; }
            if (!it.resumable) {
                // Without ranges a broken transfer restarts from byte 0.
                std::lock_guard<std::mutex> lk(it.m);
                for (auto& s : it.segs) { it.done -= s.pos - s.start; s.pos = s.start; }
                if (it.done < 0) it.done = 0;
            }
            { std::lock_guard<std::mutex> lk(it.m); it.error = widen(err) + L" — retrying (" + std::to_wstring(retries) + L"/" + std::to_wstring(c.retries) + L")"; }
            if (!sleepFor(std::min(30000, 1000 * (1 << std::min(retries - 1, 5))), &it.cancel)) break;
        }
        setConn(false);
        if (seg >= 0) { std::lock_guard<std::mutex> lk(it.m); it.segs[seg].active = false; }
        --j.active;
    }

    static std::wstring extForType(const std::string& ct) {
        std::string t = lower(ct.substr(0, ct.find(';')));
        t = trim(t);
        static const std::pair<const char*, const wchar_t*> map[] = {
            {"text/html", L".html"}, {"application/zip", L".zip"}, {"application/pdf", L".pdf"}, {"application/json", L".json"}, {"text/plain", L".txt"},
            {"image/png", L".png"}, {"image/jpeg", L".jpg"}, {"image/gif", L".gif"}, {"image/webp", L".webp"}, {"video/mp4", L".mp4"}, {"audio/mpeg", L".mp3"},
            {"application/x-msdownload", L".exe"}, {"application/x-7z-compressed", L".7z"}, {"application/gzip", L".gz"}, {"application/x-bittorrent", L".torrent"},
            {"application/vnd.rar", L".rar"}, {"application/x-rar-compressed", L".rar"}, {"video/webm", L".webm"}, {"application/xml", L".xml"}, {"text/csv", L".csv"}};
        for (auto& p : map) if (t == p.first) return p.second;
        return L"";
    }

    void runHttp(std::shared_ptr<Item> itp) {
        Item& it = *itp;
        Job j; j.e = this; j.it = itp;
        net::Proxy proxy;
        std::wstring perr;
        if (!proxyFor(it, proxy, perr, &j.torProxy)) { finishWith(it, Failed, perr); return; }
        j.opt = httpOptions(it, proxy);
        std::string start = it.finalUrl.empty() ? it.url : it.finalUrl;
        if (!net::parseUrl(start, j.url)) { finishWith(it, Failed, L"Invalid URL"); return; }
        Config c = config();
        bool fresh;
        { std::lock_guard<std::mutex> lk(it.m); fresh = it.segs.empty() || (!it.resumable && it.done > 0) || (!it.resumable && it.size < 0); }
        std::unique_ptr<http::Body> first;
        if (fresh) {
            std::string err;
            int tries = 0;
            // The probe connection becomes worker 0, so it gets its own Tor circuit like the others.
            http::Options firstOpt = j.opt;
            if (j.torProxy && c.torIsolate) { firstOpt.conn.proxy.user = "tdm-" + std::to_string(it.id) + "-0-" + randomToken(4); firstOpt.conn.proxy.pass = "x"; }
            for (;;) {
                first = http::request(j.url, firstOpt, 0, -1, err);
                if (first || it.cancel) break;
                if (++tries > std::min(c.retries, 4)) break;
                { std::lock_guard<std::mutex> lk(it.m); it.error = widen(err) + L" — retrying"; }
                if (!sleepFor(1000 * tries, &it.cancel)) break;
            }
            if (it.cancel) { finishWith(it, it.stopAs, L""); return; }
            if (!first) { finishWith(it, Failed, widen(err)); return; }
            const http::Response& r = first->r;
            if (r.status >= 400) { finishWith(it, Failed, widen(http::statusText(r.status, r.reason))); return; }
            std::wstring name;
            {
                std::lock_guard<std::mutex> lk(it.m);
                it.finalUrl = r.url.str();
                it.resumable = r.status == 206 && r.rangeStart == 0;
                it.size = it.resumable ? r.total : r.length;
                std::string et = r.headers.get("etag");
                it.etag = startsWith(et, "W/") ? "" : et;
                it.lastModified = r.headers.get("last-modified");
                it.segs.clear();
                Segment s; s.start = 0; s.end = it.size >= 0 ? it.size : INT64_MAX; s.pos = 0; s.active = true;
                it.segs.push_back(s);
                it.done = 0;
                name = it.name;
            }
            j.url = r.url;
            if (!it.userName) {
                std::string n = http::dispositionFilename(r.headers.get("content-disposition"));
                if (n.empty()) n = http::urlFilename(r.url);
                if (n.empty()) n = http::urlFilename(j.url);
                net::Url orig; net::parseUrl(it.url, orig);
                if (n.empty()) n = http::urlFilename(orig);
                if (n.empty()) n = r.url.host;
                std::wstring w = sanitizeFileName(widenLoose(n));
                if (extensionOf(w).empty()) w += extForType(r.headers.get("content-type"));
                std::wstring folder = it.folder;
                std::wstring p = uniquePath(folder, w, [&](const std::wstring& x) { return pathTaken(x, it.id); });
                std::lock_guard<std::mutex> lk(it.m);
                it.name = fileNameOf(p);
                it.category = categoryFor(it.name, KindHttp);
            }
        } else {
            std::lock_guard<std::mutex> lk(it.m);
            if (!it.finalUrl.empty()) net::parseUrl(it.finalUrl, j.url);
        }
        makeDirs(it.folder);
        j.file = CreateFileW(it.partPath().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (j.file == INVALID_HANDLE_VALUE) { finishWith(it, Failed, L"Cannot create the file: " + errorText(GetLastError())); return; }
        if (fresh) {
            LARGE_INTEGER z{}; SetFilePointerEx(j.file, z, nullptr, FILE_BEGIN); SetEndOfFile(j.file);
            if (c.preallocate && it.size > 0) {
                LARGE_INTEGER sz; sz.QuadPart = it.size;
                if (SetFilePointerEx(j.file, sz, nullptr, FILE_BEGIN) && !SetEndOfFile(j.file)) {
                    DWORD e = GetLastError();
                    CloseHandle(j.file);
                    finishWith(it, Failed, e == ERROR_DISK_FULL ? L"Not enough disk space" : L"Cannot allocate the file: " + errorText(e));
                    return;
                }
            }
        } else {
            // A resumed download whose part file vanished starts over.
            LARGE_INTEGER sz{}; GetFileSizeEx(j.file, &sz);
            std::lock_guard<std::mutex> lk(it.m);
            if (sz.QuadPart == 0 && it.done > 0) { for (auto& s : it.segs) s.pos = s.start; it.done = 0; }
        }
        { std::lock_guard<std::mutex> lk(it.m); it.limiter.rate = it.limit; for (auto& s : it.segs) s.active = false; if (fresh) it.segs[0].active = true; it.error.clear(); }

        int want = it.connections ? it.connections : (j.torProxy ? c.torConnections : c.connections);
        want = std::max(1, std::min(32, want));
        if (!it.resumable || it.size < 0) want = 1;
        std::vector<std::thread> ws;
        j.active = 1;
        ws.emplace_back([this, &j, &first, fresh] { worker(j, 0, fresh ? 0 : -1, std::move(first)); });
        for (int i = 1; i < want && !it.cancel; ++i) {
            if (!sleepFor(fresh ? 150 : 50, &it.cancel)) break;
            ++j.active;
            ws.emplace_back([this, &j, i] { worker(j, i, -1, nullptr); });
        }
        // Keep the connection count topped up while there is still work to split.
        while (!it.cancel) {
            Sleep(200);
            if (j.active == 0) break;
        }
        for (auto& t : ws) t.join();

        bool complete;
        {
            std::lock_guard<std::mutex> lk(it.m);
            complete = it.size >= 0;
            for (auto& s : it.segs) if (s.pos < s.end) complete = false;
            // Merge finished neighbours so the saved segment list stays short.
            std::sort(it.segs.begin(), it.segs.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
            std::vector<Segment> merged;
            for (auto& s : it.segs) {
                if (!merged.empty() && merged.back().pos == merged.back().end && merged.back().end == s.start) { merged.back().end = s.end; merged.back().pos = s.pos; }
                else merged.push_back(s);
            }
            it.segs = merged;
            it.dirty = true;
        }
        if (j.failed) {
            CloseHandle(j.file);
            if (j.restartFromZero) {
                std::lock_guard<std::mutex> lk(it.m);
                it.segs.clear(); it.done = 0; it.size = -1; it.finalUrl.clear();
                if (j.failMsg == L"The file changed on the server" || j.failMsg == L"Server no longer supports resuming") { it.status = Queued; it.force = true; it.error = j.failMsg + L" — restarting"; it.cancel = false; return; }
            }
            finishWith(it, Failed, j.failMsg);
            return;
        }
        if (!complete) {
            CloseHandle(j.file);
            if (it.cancel) finishWith(it, it.stopAs, L"");
            else finishWith(it, Failed, L"Download stopped unexpectedly");
            return;
        }
        LARGE_INTEGER sz; sz.QuadPart = it.size;
        SetFilePointerEx(j.file, sz, nullptr, FILE_BEGIN); SetEndOfFile(j.file);
        CloseHandle(j.file);
        std::wstring final = it.path();
        if (fileExists(final)) {
            std::wstring alt = uniquePath(it.folder, it.name, [&](const std::wstring& x) { return pathTaken(x, it.id); });
            std::wstring part = it.partPath();
            { std::lock_guard<std::mutex> lk(it.m); it.name = fileNameOf(alt); }
            if (!MoveFileExW(part.c_str(), alt.c_str(), MOVEFILE_COPY_ALLOWED)) { finishWith(it, Failed, L"Cannot rename the finished file: " + errorText(GetLastError())); return; }
        } else if (!MoveFileExW(it.partPath().c_str(), final.c_str(), MOVEFILE_COPY_ALLOWED)) { finishWith(it, Failed, L"Cannot rename the finished file: " + errorText(GetLastError())); return; }
        verifyChecksum(it);
        {
            std::lock_guard<std::mutex> lk(it.m);
            it.finished = unixNow();
            it.segs.clear();
            Segment s; s.start = 0; s.end = it.size; s.pos = it.size; it.segs.push_back(s);
        }
        finishWith(it, Completed, L"");
        if (onFinished) onFinished(it.id);
    }
public:
    void verifyChecksum(Item& it) {
        std::string algo, digest;
        if (it.checksum.empty() || !parseChecksum(it.checksum, algo, digest)) return;
        { std::lock_guard<std::mutex> lk(it.m); it.status = Checking; }
        changed();
        std::wstring err;
        std::string got = hashFile(it.path(), algo, nullptr, err);
        std::wstring algoName = widen(algo); for (auto& ch : algoName) ch = towupper(ch);
        std::lock_guard<std::mutex> lk(it.m);
        if (!err.empty()) it.checkResult = L"Checksum not verified: " + err;
        else it.checkResult = got == digest ? algoName + L" verified" : algoName + L" MISMATCH (got " + widen(got) + L")";
    }
    void finishWith(Item& it, int status, const std::wstring& err) {
        std::lock_guard<std::mutex> lk(it.m);
        it.status = it.removed ? Paused : status;
        if (!err.empty() || status != Failed) it.error = err;
        it.conns = 0; it.speed = 0; it.dirty = true;
    }
};

inline uint32_t Engine::add(const AddRequest& r, std::wstring& err) {
    auto it = std::make_shared<Item>();
    it->kind = (startsWith(lower(r.url), "magnet:") || !r.torrentData.empty()) ? KindTorrent : KindHttp;
    it->url = trim(r.url);
    it->referer = r.referer; it->cookie = r.cookie; it->user = r.user; it->pass = r.pass; it->checksum = r.checksum;
    it->connections = r.connections; it->tor = r.tor; it->limit = r.limit;
    it->added = unixNow();
    it->status = r.start ? Queued : Paused;
    it->force = false;
    it->fileSel = r.fileSel;
    if (it->kind == KindHttp) {
        net::Url u;
        if (!net::parseUrl(it->url, u)) { err = L"That is not a valid http or https URL."; return 0; }
        it->url = u.str();
        it->name = sanitizeFileName(r.name.empty() ? widenLoose(http::urlFilename(u)) : r.name);
        if (r.name.empty() && http::urlFilename(u).empty()) it->name = sanitizeFileName(widen(u.host));
        it->userName = !r.name.empty();
        it->category = categoryFor(it->name, KindHttp);
        it->folder = folderFor(r.folder, it->category);
        std::wstring p = uniquePath(it->folder, it->name, [&](const std::wstring& x) { return pathTaken(x, 0); });
        it->name = fileNameOf(p);
    } else {
        it->category = L"Torrents";
        it->folder = folderFor(r.folder, it->category);
        if (!prepareTorrentItem(*this, *it, r, err)) return 0;
    }
    {
        std::lock_guard<std::mutex> lk(m);
        it->id = nextId++;
        items.push_back(it);
    }
    changed();
    return it->id;
}

} // namespace engine
