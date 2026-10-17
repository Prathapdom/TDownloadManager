// Network layer: URLs, cancellable TCP, SOCKS5 (remote DNS, for Tor), HTTP CONNECT proxies and Schannel TLS.
#pragma once
#include "common.hpp"
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#include <sspi.h>
#ifndef SCH_USE_STRONG_CRYPTO
#define SCH_USE_STRONG_CRYPTO 0x00400000
#endif

namespace net {
using namespace tdm;

inline void init() {
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
}

// ---------------------------------------------------------------- URLs

struct Url {
    std::string scheme, user, pass, host, path = "/"; // path includes the query string
    int port = 0;
    bool valid = false;
    bool tls() const { return scheme == "https"; }
    int defaultPort() const { return scheme == "https" ? 443 : scheme == "ftp" ? 21 : 80; }
    bool onion() const { return host.size() > 6 && host.compare(host.size() - 6, 6, ".onion") == 0; }
    std::string hostHeader() const {
        std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
        return port == defaultPort() ? h : h + ":" + std::to_string(port);
    }
    std::string str() const { return scheme + "://" + hostHeader() + path; }
    std::string pathOnly() const { size_t q = path.find('?'); return q == std::string::npos ? path : path.substr(0, q); }
};

// Encodes characters that browsers send percent-encoded but users paste raw (spaces, non-ASCII).
inline std::string normalizeTarget(const std::string& s) {
    static const char* d = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}') { o += '%'; o += d[c >> 4]; o += d[c & 15]; }
        else o += (char)c;
    }
    return o;
}

inline bool parseUrl(std::string s, Url& u) {
    u = Url();
    s = trim(s);
    size_t hash = s.find('#');
    if (hash != std::string::npos) s.resize(hash);
    size_t sep = s.find("://");
    if (sep == std::string::npos) { u.scheme = "http"; sep = 0; }
    else { u.scheme = lower(s.substr(0, sep)); s = s.substr(sep + 3); }
    if (u.scheme != "http" && u.scheme != "https") return false;
    size_t end = s.find_first_of("/?");
    std::string auth = s.substr(0, end);
    std::string rest = end == std::string::npos ? "/" : s.substr(end);
    if (rest[0] == '?') rest = "/" + rest;
    size_t at = auth.rfind('@');
    if (at != std::string::npos) {
        std::string ui = auth.substr(0, at);
        auth = auth.substr(at + 1);
        size_t c = ui.find(':');
        u.user = percentDecode(ui.substr(0, c));
        if (c != std::string::npos) u.pass = percentDecode(ui.substr(c + 1));
    }
    std::string portStr;
    if (!auth.empty() && auth[0] == '[') {
        size_t rb = auth.find(']');
        if (rb == std::string::npos) return false;
        u.host = auth.substr(1, rb - 1);
        if (rb + 1 < auth.size() && auth[rb + 1] == ':') portStr = auth.substr(rb + 2);
    } else {
        size_t c = auth.rfind(':');
        u.host = auth.substr(0, c);
        if (c != std::string::npos) portStr = auth.substr(c + 1);
    }
    u.host = lower(u.host);
    while (!u.host.empty() && u.host.back() == '.') u.host.pop_back();
    if (u.host.empty()) return false;
    for (char ch : u.host) if ((unsigned char)ch <= ' ' || ch == '/' || ch == '\\') return false;
    u.port = u.defaultPort();
    if (!portStr.empty()) {
        for (char ch : portStr) if (ch < '0' || ch > '9') return false;
        u.port = atoi(portStr.c_str());
        if (u.port <= 0 || u.port > 65535) return false;
    }
    u.path = normalizeTarget(rest);
    u.valid = true;
    return true;
}

// Resolves a redirect Location (absolute, scheme-relative, host-relative or path-relative).
inline Url resolveUrl(const Url& base, const std::string& ref) {
    Url out;
    std::string r = trim(ref);
    std::string lr = lower(r.substr(0, 8));
    if (startsWith(lr, "http://") || startsWith(lr, "https://")) { parseUrl(r, out); return out; }
    if (startsWith(r, "//")) { parseUrl(base.scheme + ":" + r, out); return out; }
    out = base;
    if (r.empty()) return out;
    if (r[0] == '/') out.path = r;
    else if (r[0] == '?') out.path = base.pathOnly() + r;
    else {
        std::string dir = base.pathOnly();
        dir = dir.substr(0, dir.rfind('/') + 1);
        out.path = dir + r;
    }
    // Remove dot segments.
    std::string p = out.path, q;
    size_t qm = p.find('?');
    if (qm != std::string::npos) { q = p.substr(qm); p = p.substr(0, qm); }
    std::vector<std::string> parts;
    size_t i = 1;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        std::string seg = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (seg != ".") parts.push_back(seg);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    std::string np;
    for (auto& s : parts) np += "/" + s;
    if (np.empty() || (!p.empty() && p.back() == '/' && np.back() != '/')) np += "/";
    out.path = normalizeTarget(np + q);
    return out;
}

// ---------------------------------------------------------------- streams

struct Stream {
    std::string err;
    bool absoluteForm = false; // plain HTTP through an HTTP proxy: the request line carries the full URL
    virtual ~Stream() {}
    virtual int read(char* p, int n) = 0; // >0 bytes read, 0 end of stream, <0 error (see err)
    virtual bool write(const char* p, size_t n) = 0;
    bool write(const std::string& s) { return write(s.data(), s.size()); }
};

class Tcp : public Stream {
public:
    using Stream::write;
    SOCKET s = INVALID_SOCKET;
    const std::atomic<bool>* cancel = nullptr;
    int timeoutMs = 30000;
    ~Tcp() override { close(); }
    void close() { if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; } }
    bool cancelled() const { return cancel && *cancel; }
    // Waits until the socket is readable/writable, in short slices so cancellation is prompt.
    bool wait(bool writable, int ms) {
        uint64_t end = nowMs() + (uint64_t)ms;
        for (;;) {
            if (cancelled()) { err = "Cancelled"; return false; }
            fd_set fs, ex; FD_ZERO(&fs); FD_ZERO(&ex); FD_SET(s, &fs); FD_SET(s, &ex);
            timeval tv{0, 200000};
            int r = select(0, writable ? nullptr : &fs, writable ? &fs : nullptr, &ex, &tv);
            if (r < 0) { err = "Socket error " + std::to_string(WSAGetLastError()); return false; }
            if (r > 0) {
                if (FD_ISSET(s, &ex)) { int e = 0; int len = sizeof e; getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &len); err = "Connection failed (" + std::to_string(e) + ")"; return false; }
                return true;
            }
            if (nowMs() >= end) { err = "Timed out"; return false; }
        }
    }
    bool connectTo(const std::string& host, int port) {
        init();
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
        int gr = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        if (gr != 0 || !res) { err = "Cannot resolve " + host; return false; }
        if (cancelled()) { freeaddrinfo(res); err = "Cancelled"; return false; }
        std::string last = "Cannot connect to " + host;
        for (addrinfo* a = res; a; a = a->ai_next) {
            close();
            s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
            int r = ::connect(s, a->ai_addr, (int)a->ai_addrlen);
            if (r == 0 || WSAGetLastError() == WSAEWOULDBLOCK) {
                if (r == 0 || wait(true, std::min(timeoutMs, a->ai_next ? 8000 : timeoutMs))) {
                    int e = 0; int len = sizeof e; getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &len);
                    if (e == 0) { freeaddrinfo(res); err.clear(); return true; }
                    last = "Connection refused by " + host;
                } else last = err == "Timed out" ? "Connection to " + host + " timed out" : err;
                if (cancelled()) break;
            }
        }
        freeaddrinfo(res);
        close();
        err = cancelled() ? "Cancelled" : last;
        return false;
    }
    int read(char* p, int n) override {
        for (;;) {
            int r = recv(s, p, n, 0);
            if (r >= 0) return r;
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) { err = e == WSAECONNRESET ? "Connection reset" : "Receive failed (" + std::to_string(e) + ")"; return -1; }
            if (!wait(false, timeoutMs)) { if (err == "Timed out") err = "No data received (timed out)"; return -1; }
        }
    }
    bool write(const char* p, size_t n) override {
        size_t off = 0;
        while (off < n) {
            int r = send(s, p + off, (int)std::min<size_t>(n - off, 1 << 20), 0);
            if (r > 0) { off += (size_t)r; continue; }
            int e = WSAGetLastError();
            if (r < 0 && e != WSAEWOULDBLOCK) { err = "Send failed (" + std::to_string(e) + ")"; return false; }
            if (!wait(true, timeoutMs)) return false;
        }
        return true;
    }
    bool readExact(char* p, int n) {
        int got = 0;
        while (got < n) {
            int r = read(p + got, n - got);
            if (r <= 0) { if (r == 0) err = "Connection closed"; return false; }
            got += r;
        }
        return true;
    }
};

// ---------------------------------------------------------------- proxies

struct Proxy {
    enum Kind { None = 0, Socks5 = 1, Http = 2 };
    int kind = None;
    std::string host; int port = 0;
    std::string user, pass;
};

inline std::string socksReplyText(int code) {
    switch (code) {
    case 1: return "SOCKS server failure";
    case 2: return "Connection not allowed by proxy rules";
    case 3: return "Network unreachable";
    case 4: return "Host unreachable";
    case 5: return "Connection refused";
    case 6: return "TTL expired (the circuit timed out)";
    case 7: return "Command not supported";
    case 8: return "Address type not supported";
    case 0xF0: return "Onion service descriptor cannot be found (the site is offline or the address is wrong)";
    case 0xF1: return "Onion service descriptor is invalid";
    case 0xF2: return "Onion service introduction failed";
    case 0xF3: return "Onion service rendezvous failed";
    case 0xF4: return "Onion service needs client authorization";
    case 0xF5: return "Onion service client authorization was rejected";
    case 0xF6: return "Invalid onion address";
    case 0xF7: return "Onion service introduction timed out";
    }
    return "SOCKS error " + std::to_string(code);
}

// SOCKS5 CONNECT with the host name sent to the proxy, so DNS never leaks outside Tor.
inline bool socks5Connect(Tcp& t, const std::string& host, int port, const std::string& user, const std::string& pass) {
    bool auth = !user.empty();
    std::string hello = auth ? std::string("\x05\x02\x00\x02", 4) : std::string("\x05\x01\x00", 3);
    if (!t.write(hello)) return false;
    char rep[2];
    if (!t.readExact(rep, 2)) { t.err = "Proxy closed the connection (is it a SOCKS5 proxy?)"; return false; }
    if (rep[0] != 5) { t.err = "Not a SOCKS5 proxy"; return false; }
    if ((uint8_t)rep[1] == 0x02) {
        std::string a; a += '\x01'; a += (char)std::min<size_t>(user.size(), 255); a += user.substr(0, 255); a += (char)std::min<size_t>(pass.size(), 255); a += pass.substr(0, 255);
        if (!t.write(a) || !t.readExact(rep, 2)) return false;
        if (rep[1] != 0) { t.err = "SOCKS5 proxy rejected the user name or password"; return false; }
    } else if ((uint8_t)rep[1] != 0) { t.err = "SOCKS5 proxy requires an authentication method we do not support"; return false; }
    std::string req("\x05\x01\x00", 3);
    in_addr a4; in6_addr a6;
    if (inet_pton(AF_INET, host.c_str(), &a4) == 1) { req += '\x01'; req.append((const char*)&a4, 4); }
    else if (inet_pton(AF_INET6, host.c_str(), &a6) == 1) { req += '\x04'; req.append((const char*)&a6, 16); }
    else { if (host.size() > 255) { t.err = "Host name too long"; return false; } req += '\x03'; req += (char)host.size(); req += host; }
    req += (char)(port >> 8); req += (char)(port & 0xFF);
    if (!t.write(req)) return false;
    char head[4];
    if (!t.readExact(head, 4)) { if (t.err == "Connection closed") t.err = "Proxy closed the connection"; return false; }
    if (head[1] != 0) { t.err = socksReplyText((uint8_t)head[1]); return false; }
    int skip = head[3] == 1 ? 4 : head[3] == 4 ? 16 : 0;
    if (head[3] == 3) { char l; if (!t.readExact(&l, 1)) return false; skip = (uint8_t)l; }
    char tail[260];
    return t.readExact(tail, skip + 2);
}

inline bool httpConnect(Tcp& t, const std::string& host, int port, const Proxy& p) {
    std::string hp = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    std::string req = "CONNECT " + hp + " HTTP/1.1\r\nHost: " + hp + "\r\n";
    if (!p.user.empty()) req += "Proxy-Authorization: Basic " + base64(p.user + ":" + p.pass) + "\r\n";
    req += "\r\n";
    if (!t.write(req)) return false;
    std::string resp;
    char c;
    while (resp.size() < 16384) {
        if (!t.readExact(&c, 1)) return false;
        resp += c;
        if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    int code = resp.size() > 12 ? atoi(resp.c_str() + 9) : 0;
    if (code != 200) { t.err = "Proxy refused the tunnel (" + trim(resp.substr(0, resp.find('\r'))) + ")"; return false; }
    return true;
}

struct ConnectOptions {
    Proxy proxy;
    int timeoutMs = 30000;
    bool verifyTls = true;
    const std::atomic<bool>* cancel = nullptr;
};

// A TCP connection to host:port, through the proxy when one is set.
inline std::unique_ptr<Tcp> connectTcp(const std::string& host, int port, const ConnectOptions& o, std::string& err) {
    auto t = std::make_unique<Tcp>();
    t->cancel = o.cancel; t->timeoutMs = o.timeoutMs;
    if (o.proxy.kind == Proxy::None) {
        if (!t->connectTo(host, port)) { err = t->err; return nullptr; }
        return t;
    }
    if (!t->connectTo(o.proxy.host, o.proxy.port)) {
        err = "Cannot reach the proxy at " + o.proxy.host + ":" + std::to_string(o.proxy.port) + " (" + t->err + ")";
        return nullptr;
    }
    t->timeoutMs = std::max(o.timeoutMs, 60000); // Tor circuits and onion rendezvous can take a while
    bool ok = o.proxy.kind == Proxy::Socks5 ? socks5Connect(*t, host, port, o.proxy.user, o.proxy.pass) : httpConnect(*t, host, port, o.proxy);
    t->timeoutMs = o.timeoutMs;
    if (!ok) { err = t->err; return nullptr; }
    return t;
}

// ---------------------------------------------------------------- TLS (Schannel)

inline std::string secError(SECURITY_STATUS st) {
    switch ((unsigned long)st) {
    case (unsigned long)SEC_E_UNTRUSTED_ROOT: return "TLS certificate is not trusted";
    case (unsigned long)SEC_E_CERT_EXPIRED: return "TLS certificate has expired";
    case (unsigned long)SEC_E_WRONG_PRINCIPAL: return "TLS certificate does not match the host name";
    case (unsigned long)SEC_E_ILLEGAL_MESSAGE: return "TLS handshake failed (the server sent an alert)";
    case (unsigned long)SEC_E_ALGORITHM_MISMATCH: return "TLS handshake failed (no common cipher)";
    case (unsigned long)SEC_E_INTERNAL_ERROR: return "TLS internal error";
    case (unsigned long)CERT_E_REVOKED: return "TLS certificate has been revoked";
    }
    char b[64]; snprintf(b, sizeof b, "TLS error 0x%08lX", (unsigned long)st);
    return b;
}

class TlsCreds {
public:
    static CredHandle* get(bool verify) {
        static std::mutex m;
        static CredHandle h[2];
        static bool have[2] = {false, false};
        std::lock_guard<std::mutex> lk(m);
        int i = verify ? 1 : 0;
        if (!have[i]) {
            SCHANNEL_CRED sc{};
            sc.dwVersion = SCHANNEL_CRED_VERSION;
            sc.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_DEFAULT_CREDS | (verify ? SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | SCH_CRED_IGNORE_NO_REVOCATION_CHECK | SCH_CRED_IGNORE_REVOCATION_OFFLINE : SCH_CRED_MANUAL_CRED_VALIDATION);
            TimeStamp ts;
            if (AcquireCredentialsHandleW(nullptr, (SEC_WCHAR*)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &h[i], &ts) != SEC_E_OK) return nullptr;
            have[i] = true;
        }
        return &h[i];
    }
};

class Tls : public Stream {
    std::unique_ptr<Tcp> tcp;
    CtxtHandle ctx{};
    bool haveCtx = false;
    SecPkgContext_StreamSizes sizes{};
    std::vector<char> in;
    size_t inLen = 0;
    std::string plain; size_t plainPos = 0;
    bool closed = false;
    std::wstring target;
    CredHandle* cred = nullptr;
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    bool recvMore() {
        if (in.size() - inLen < 8192) in.resize(in.size() * 2);
        int r = tcp->read(in.data() + inLen, (int)(in.size() - inLen));
        if (r <= 0) { err = r == 0 ? "Connection closed during TLS" : tcp->err; return false; }
        inLen += (size_t)r;
        return true;
    }
    bool sendToken(SecBuffer& b) {
        bool ok = true;
        if (b.cbBuffer && b.pvBuffer) ok = tcp->write((const char*)b.pvBuffer, b.cbBuffer);
        if (b.pvBuffer) FreeContextBuffer(b.pvBuffer);
        b.pvBuffer = nullptr; b.cbBuffer = 0;
        if (!ok) err = tcp->err;
        return ok;
    }
    // Runs the handshake loop until SEC_E_OK. Used for the initial handshake and renegotiation.
    bool handshakeLoop(bool needRead) {
        for (int guard = 0; guard < 200; ++guard) {
            if (needRead && !recvMore()) return false;
            needRead = true;
            SecBuffer inB[2] = {{(unsigned long)inLen, SECBUFFER_TOKEN, in.data()}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBuffer outB[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc inD{SECBUFFER_VERSION, 2, inB}, outD{SECBUFFER_VERSION, 1, outB};
            DWORD outFlags = 0;
            SECURITY_STATUS st = InitializeSecurityContextW(cred, &ctx, (SEC_WCHAR*)target.c_str(), flags, 0, 0, &inD, 0, nullptr, &outD, &outFlags, nullptr);
            if (st == SEC_E_INCOMPLETE_MESSAGE) continue;
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || (FAILED(st) && (outFlags & ISC_RET_EXTENDED_ERROR))) {
                if (!sendToken(outB[0])) return false;
            }
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS) {
                if (inB[1].BufferType == SECBUFFER_EXTRA && inB[1].cbBuffer) {
                    memmove(in.data(), in.data() + (inLen - inB[1].cbBuffer), inB[1].cbBuffer);
                    inLen = inB[1].cbBuffer;
                    needRead = false;
                } else inLen = 0;
                if (st == SEC_E_OK) return true;
                if (st == SEC_I_INCOMPLETE_CREDENTIALS) needRead = false; // server asked for a client cert: continue without one
                continue;
            }
            err = secError(st);
            return false;
        }
        err = "TLS handshake did not finish";
        return false;
    }

public:
    using Stream::write;
    ~Tls() override {
        if (haveCtx) DeleteSecurityContext(&ctx);
    }
    bool handshake(std::unique_ptr<Tcp> t, const std::string& host, bool verify) {
        tcp = std::move(t);
        absoluteForm = false;
        cred = TlsCreds::get(verify);
        if (!cred) { err = "TLS is not available on this system"; return false; }
        if (!verify) flags |= ISC_REQ_MANUAL_CRED_VALIDATION;
        target = widen(host);
        in.resize(1 << 16);
        SecBuffer outB[1] = {{0, SECBUFFER_TOKEN, nullptr}};
        SecBufferDesc outD{SECBUFFER_VERSION, 1, outB};
        DWORD outFlags = 0;
        SECURITY_STATUS st = InitializeSecurityContextW(cred, nullptr, (SEC_WCHAR*)target.c_str(), flags, 0, 0, nullptr, 0, &ctx, &outD, &outFlags, nullptr);
        if (st != SEC_I_CONTINUE_NEEDED) { err = secError(st); return false; }
        haveCtx = true;
        if (!sendToken(outB[0])) return false;
        if (!handshakeLoop(true)) return false;
        if (QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) { err = "TLS setup failed"; return false; }
        size_t need = sizes.cbHeader + sizes.cbMaximumMessage + sizes.cbTrailer;
        if (in.size() < need * 2) in.resize(need * 2);
        return true;
    }
    int read(char* p, int n) override {
        for (;;) {
            if (plainPos < plain.size()) {
                int k = (int)std::min<size_t>((size_t)n, plain.size() - plainPos);
                memcpy(p, plain.data() + plainPos, (size_t)k);
                plainPos += (size_t)k;
                if (plainPos == plain.size()) { plain.clear(); plainPos = 0; }
                return k;
            }
            if (closed) return 0;
            if (inLen) {
                SecBuffer b[4] = {{(unsigned long)inLen, SECBUFFER_DATA, in.data()}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
                SecBufferDesc d{SECBUFFER_VERSION, 4, b};
                SECURITY_STATUS st = DecryptMessage(&ctx, &d, 0, nullptr);
                if (st == SEC_E_OK || st == SEC_I_RENEGOTIATE || st == SEC_I_CONTEXT_EXPIRED) {
                    SecBuffer *data = nullptr, *extra = nullptr;
                    for (auto& x : b) { if (x.BufferType == SECBUFFER_DATA) data = &x; else if (x.BufferType == SECBUFFER_EXTRA) extra = &x; }
                    if (data && data->cbBuffer) plain.append((const char*)data->pvBuffer, data->cbBuffer);
                    if (extra && extra->cbBuffer) { memmove(in.data(), extra->pvBuffer, extra->cbBuffer); inLen = extra->cbBuffer; }
                    else inLen = 0;
                    if (st == SEC_I_CONTEXT_EXPIRED) closed = true;
                    if (st == SEC_I_RENEGOTIATE && !handshakeLoop(inLen == 0)) return -1;
                    continue;
                }
                if (st != SEC_E_INCOMPLETE_MESSAGE) { err = secError(st); return -1; }
            }
            if (in.size() - inLen < 4096) in.resize(in.size() * 2);
            int r = tcp->read(in.data() + inLen, (int)(in.size() - inLen));
            if (r == 0) { closed = true; return 0; } // many servers close without close_notify
            if (r < 0) { err = tcp->err; return -1; }
            inLen += (size_t)r;
        }
    }
    bool write(const char* p, size_t n) override {
        std::vector<char> buf(sizes.cbHeader + sizes.cbMaximumMessage + sizes.cbTrailer);
        size_t off = 0;
        while (off < n) {
            size_t k = std::min<size_t>(n - off, sizes.cbMaximumMessage);
            memcpy(buf.data() + sizes.cbHeader, p + off, k);
            SecBuffer b[4] = {{sizes.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()}, {(unsigned long)k, SECBUFFER_DATA, buf.data() + sizes.cbHeader},
                              {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + sizes.cbHeader + k}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            SECURITY_STATUS st = EncryptMessage(&ctx, 0, &d, 0);
            if (st != SEC_E_OK) { err = secError(st); return false; }
            if (!tcp->write(buf.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) { err = tcp->err; return false; }
            off += k;
        }
        return true;
    }
};

// Opens a byte stream to the URL's origin: TCP (through the proxy) plus TLS for https.
inline std::unique_ptr<Stream> connect(const Url& u, const ConnectOptions& o, std::string& err) {
    if (o.proxy.kind == Proxy::Http && !u.tls()) {
        // Plain HTTP via an HTTP proxy: talk to the proxy directly with absolute-form request targets.
        auto t = std::make_unique<Tcp>();
        t->cancel = o.cancel; t->timeoutMs = o.timeoutMs;
        if (!t->connectTo(o.proxy.host, o.proxy.port)) { err = "Cannot reach the proxy (" + t->err + ")"; return nullptr; }
        t->absoluteForm = true;
        return t;
    }
    auto t = connectTcp(u.host, u.port, o, err);
    if (!t) return nullptr;
    if (!u.tls()) return t;
    auto s = std::make_unique<Tls>();
    if (!s->handshake(std::move(t), u.host, o.verifyTls)) { err = s->err; return nullptr; }
    return s;
}

} // namespace net
