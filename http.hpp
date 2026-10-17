// HTTP/1.1 client: request building, response parsing, chunked and length-delimited bodies, redirects, ranges.
#pragma once
#include "net.hpp"

namespace http {
using namespace tdm;

struct Headers {
    std::vector<std::pair<std::string, std::string>> list; // names lower-cased
    std::string get(const std::string& name) const { for (auto& h : list) if (h.first == name) return h.second; return {}; }
    bool has(const std::string& name) const { for (auto& h : list) if (h.first == name) return true; return false; }
};

struct Response {
    int status = 0;
    std::string reason;
    Headers headers;
    int64_t length = -1;                         // body length, -1 unknown
    int64_t rangeStart = -1, rangeEnd = -1, total = -1; // from Content-Range or Content-Length
    bool chunked = false;
    net::Url url;                                // after redirects
};

struct Options {
    net::ConnectOptions conn;
    std::string userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";
    std::string referer, cookie, user, pass;
    std::vector<std::string> extraHeaders; // "Name: value"
    int maxRedirects = 12;
};

inline bool parseContentRange(const std::string& v, int64_t& a, int64_t& b, int64_t& total) {
    // "bytes 0-499/1234", "bytes 0-499/*", "bytes */1234"
    std::string s = trim(v);
    if (!startsWith(lower(s), "bytes")) return false;
    s = trim(s.substr(5));
    if (!s.empty() && s[0] == '=') s = s.substr(1);
    size_t slash = s.find('/');
    if (slash == std::string::npos) return false;
    std::string range = s.substr(0, slash), tot = s.substr(slash + 1);
    total = tot == "*" ? -1 : _strtoi64(tot.c_str(), nullptr, 10);
    if (range == "*") { a = b = -1; return true; }
    size_t dash = range.find('-');
    if (dash == std::string::npos) return false;
    a = _strtoi64(range.c_str(), nullptr, 10);
    b = _strtoi64(range.c_str() + dash + 1, nullptr, 10);
    return a >= 0 && b >= a;
}

// File name from Content-Disposition (RFC 6266 / 5987), UTF-8 encoded.
inline std::string dispositionFilename(const std::string& cd) {
    std::string best, plain;
    size_t i = 0;
    while (i < cd.size()) {
        size_t semi = cd.find(';', i);
        // Quoted strings can contain ';'.
        size_t q = cd.find('"', i);
        if (q != std::string::npos && semi != std::string::npos && q < semi) {
            size_t q2 = cd.find('"', q + 1);
            while (q2 != std::string::npos && cd[q2 - 1] == '\\') q2 = cd.find('"', q2 + 1);
            if (q2 != std::string::npos) semi = cd.find(';', q2);
        }
        std::string part = trim(cd.substr(i, semi == std::string::npos ? std::string::npos : semi - i));
        i = semi == std::string::npos ? cd.size() : semi + 1;
        size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(part.substr(0, eq))), val = trim(part.substr(eq + 1));
        if (key == "filename*") {
            size_t t1 = val.find('\'');
            size_t t2 = t1 == std::string::npos ? std::string::npos : val.find('\'', t1 + 1);
            std::string charset = t1 == std::string::npos ? "" : lower(val.substr(0, t1));
            std::string enc = t2 == std::string::npos ? val : val.substr(t2 + 1);
            if (!enc.empty() && enc[0] == '"') enc = enc.substr(1, enc.size() - 2);
            std::string dec = percentDecode(enc);
            if (charset == "iso-8859-1") { std::wstring w; for (unsigned char c : dec) w += (wchar_t)c; dec = narrow(w); }
            best = dec;
        } else if (key == "filename") {
            if (!val.empty() && val[0] == '"') {
                std::string o;
                for (size_t k = 1; k < val.size() && val[k] != '"'; ++k) { if (val[k] == '\\' && k + 1 < val.size()) ++k; o += val[k]; }
                val = o;
            }
            // Some servers percent-encode the plain parameter too.
            if (val.find('%') != std::string::npos) { std::string d = percentDecode(val); if (validUtf8(d)) val = d; }
            plain = val;
        }
    }
    std::string r = best.empty() ? plain : best;
    size_t slash = r.find_last_of("/\\");
    if (slash != std::string::npos) r = r.substr(slash + 1);
    return r;
}

// Last path segment of a URL, decoded; empty when the path ends in '/'.
inline std::string urlFilename(const net::Url& u) {
    std::string p = u.pathOnly();
    size_t s = p.rfind('/');
    std::string seg = s == std::string::npos ? p : p.substr(s + 1);
    return percentDecode(seg);
}

class Body {
public:
    std::unique_ptr<net::Stream> s;
    Response r;
    std::string err;
    bool headOnly = false;

    // Reads body bytes: >0 count, 0 end of body, -1 error.
    int read(char* p, int n) {
        if (finished) return 0;
        if (r.chunked) {
            if (chunkLeft == 0) {
                std::string line;
                if (!startedChunks) startedChunks = true;
                else { if (!readLine(line)) return -1; } // CRLF after the previous chunk
                if (!readLine(line)) return -1;
                chunkLeft = (int64_t)strtoull(line.c_str(), nullptr, 16);
                if (chunkLeft == 0) {
                    while (readLine(line) && !line.empty()) {} // trailers
                    finished = true;
                    return 0;
                }
            }
            int k = rawRead(p, (int)std::min<int64_t>(n, chunkLeft));
            if (k <= 0) { if (k == 0) err = "Connection closed in the middle of a chunk"; return -1; }
            chunkLeft -= k;
            return k;
        }
        if (r.length >= 0) {
            if (left == 0) { finished = true; return 0; }
            int k = rawRead(p, (int)std::min<int64_t>(n, left));
            if (k <= 0) { if (k == 0) err = "Connection closed early"; return -1; }
            left -= k;
            return k;
        }
        int k = rawRead(p, n);
        if (k == 0) finished = true;
        return k;
    }

    bool readHead() {
        std::string line;
        if (!readLine(line)) { if (err.empty()) err = "No response from server"; return false; }
        if (!startsWith(line, "HTTP/")) { err = "Not an HTTP response"; return false; }
        size_t sp = line.find(' ');
        r.status = sp == std::string::npos ? 0 : atoi(line.c_str() + sp + 1);
        size_t sp2 = sp == std::string::npos ? std::string::npos : line.find(' ', sp + 1);
        r.reason = sp2 == std::string::npos ? "" : line.substr(sp2 + 1);
        for (int n = 0; n < 500; ++n) {
            if (!readLine(line)) return false;
            if (line.empty()) break;
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            r.headers.list.push_back({lower(trim(line.substr(0, c))), trim(line.substr(c + 1))});
        }
        if (r.status == 100) { r = Response(); return readHead(); }
        r.chunked = lower(r.headers.get("transfer-encoding")).find("chunked") != std::string::npos;
        std::string cl = r.headers.get("content-length");
        if (!r.chunked && !cl.empty()) r.length = _strtoi64(cl.c_str(), nullptr, 10);
        if (headOnly || r.status == 204 || r.status == 304 || (r.status >= 100 && r.status < 200)) { r.length = 0; r.chunked = false; }
        left = r.length;
        std::string cr = r.headers.get("content-range");
        if (r.status == 206 && !cr.empty()) parseContentRange(cr, r.rangeStart, r.rangeEnd, r.total);
        else if (r.status == 200 && r.length >= 0) r.total = r.length;
        return true;
    }

private:
    std::vector<char> buf = std::vector<char>(1 << 16);
    size_t bpos = 0, bend = 0;
    int64_t left = -1, chunkLeft = 0;
    bool finished = false, startedChunks = false;

    int rawRead(char* p, int n) {
        if (bpos < bend) {
            int k = (int)std::min<size_t>((size_t)n, bend - bpos);
            memcpy(p, buf.data() + bpos, (size_t)k);
            bpos += (size_t)k;
            return k;
        }
        int k = s->read(p, n);
        if (k < 0) err = s->err;
        return k;
    }
    bool fill() {
        if (bpos == bend) bpos = bend = 0;
        if (bend == buf.size()) { if (bpos) { memmove(buf.data(), buf.data() + bpos, bend - bpos); bend -= bpos; bpos = 0; } else return false; }
        int k = s->read(buf.data() + bend, (int)(buf.size() - bend));
        if (k <= 0) { err = k == 0 ? "Connection closed" : s->err; return false; }
        bend += (size_t)k;
        return true;
    }
    bool readLine(std::string& line) {
        line.clear();
        for (;;) {
            for (size_t i = bpos; i < bend; ++i) {
                if (buf[i] == '\n') {
                    line.append(buf.data() + bpos, i - bpos);
                    bpos = i + 1;
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
            }
            line.append(buf.data() + bpos, bend - bpos);
            bpos = bend;
            if (line.size() > 65536) { err = "Header line too long"; return false; }
            if (!fill()) return false;
        }
    }
};

inline std::string buildRequest(const std::string& method, const net::Url& u, const Options& o, int64_t from, int64_t to, const std::string& ifRange, bool absolute, bool sameOrigin) {
    std::string req = method + " " + (absolute ? u.str() : u.path) + " HTTP/1.1\r\n";
    req += "Host: " + u.hostHeader() + "\r\n";
    req += "User-Agent: " + o.userAgent + "\r\n";
    req += "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    if (from >= 0) req += "Range: bytes=" + std::to_string(from) + "-" + (to >= 0 ? std::to_string(to) : "") + "\r\n";
    if (!ifRange.empty() && from >= 0) req += "If-Range: " + ifRange + "\r\n";
    if (!o.referer.empty()) req += "Referer: " + o.referer + "\r\n";
    if (!o.cookie.empty() && sameOrigin) req += "Cookie: " + o.cookie + "\r\n";
    std::string user = !u.user.empty() ? u.user : o.user, pass = !u.user.empty() ? u.pass : o.pass;
    if (!user.empty() && sameOrigin) req += "Authorization: Basic " + base64(user + ":" + pass) + "\r\n";
    if (absolute && !o.conn.proxy.user.empty()) req += "Proxy-Authorization: Basic " + base64(o.conn.proxy.user + ":" + o.conn.proxy.pass) + "\r\n";
    for (auto& h : o.extraHeaders) if (!h.empty()) req += h + "\r\n";
    req += "\r\n";
    return req;
}

inline std::string statusText(int code, const std::string& reason) {
    std::string r = trim(reason);
    if (r.empty()) {
        switch (code) {
        case 400: r = "Bad Request"; break; case 401: r = "Unauthorized"; break; case 403: r = "Forbidden"; break;
        case 404: r = "Not Found"; break; case 410: r = "Gone"; break; case 416: r = "Range Not Satisfiable"; break;
        case 429: r = "Too Many Requests"; break; case 500: r = "Internal Server Error"; break; case 502: r = "Bad Gateway"; break;
        case 503: r = "Service Unavailable"; break; case 504: r = "Gateway Timeout"; break;
        }
    }
    return "HTTP " + std::to_string(code) + (r.empty() ? "" : " " + r);
}

// Sends a request (following redirects) and returns the response with its body ready to stream.
// from/to: byte range, -1 for none; to = -1 with from >= 0 means "from to the end".
inline std::unique_ptr<Body> request(const net::Url& start, const Options& o, int64_t from, int64_t to, std::string& err,
                                     const std::string& ifRange = "", const std::string& method = "GET") {
    net::Url u = start;
    std::string m = method;
    for (int hop = 0; hop <= o.maxRedirects; ++hop) {
        if (o.conn.cancel && *o.conn.cancel) { err = "Cancelled"; return nullptr; }
        auto s = net::connect(u, o.conn, err);
        if (!s) return nullptr;
        bool absolute = s->absoluteForm;
        bool sameOrigin = u.host == start.host;
        if (!s->write(buildRequest(m, u, o, from, to, ifRange, absolute, sameOrigin))) { err = s->err; return nullptr; }
        auto b = std::make_unique<Body>();
        b->s = std::move(s);
        b->headOnly = m == "HEAD";
        if (!b->readHead()) { err = b->err; return nullptr; }
        b->r.url = u;
        int st = b->r.status;
        if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && b->r.headers.has("location")) {
            net::Url next = net::resolveUrl(u, b->r.headers.get("location"));
            if (!next.valid) { err = "Bad redirect location"; return nullptr; }
            if (st == 303) m = "GET";
            u = next;
            continue;
        }
        return b;
    }
    err = "Too many redirects";
    return nullptr;
}

// Small GET of a whole document (tracker announces, Tor check). Returns false on transport or HTTP errors.
inline bool fetch(const net::Url& u, const Options& o, std::string& body, std::string& err, size_t limit = 8u << 20, int* statusOut = nullptr) {
    auto b = request(u, o, -1, -1, err);
    if (!b) return false;
    if (statusOut) *statusOut = b->r.status;
    std::vector<char> tmp(1 << 15);
    body.clear();
    for (;;) {
        int k = b->read(tmp.data(), (int)tmp.size());
        if (k < 0) { err = b->err; return false; }
        if (k == 0) break;
        body.append(tmp.data(), (size_t)k);
        if (body.size() > limit) { err = "Response too large"; return false; }
    }
    if (b->r.status >= 400) { err = statusText(b->r.status, b->r.reason); return false; }
    return true;
}

} // namespace http
