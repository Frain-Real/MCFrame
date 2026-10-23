// os/net/http.hpp
// ---------------------------------------------------------------------------
// Минимальный HTTP/1.1 client: GET/POST, Content-Length и chunked body.
//
// HTTPS намеренно не притворяется поддержанным: TLS — отдельный слой, и
// молчаливое понижение до plaintext было бы хуже явного NotSupported.
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"

namespace embed::net::http {

struct Url {
    char     host[96]{};
    uint16_t port = 80u;
    char     path[160] = "/";
    bool     tls = false;
};

/// Разобрать http://host[:port][/path]. false — URL некорректен.
bool parse_url(const char* url, Url& out) noexcept;

struct Response {
    int      status = 0;
    uint64_t content_length = 0u;
    char     content_type[64]{};
    bool     chunked = false;
};

/// Получатель тела. Возвращает false — приём прервать (ошибка/переполнение).
using BodySink = bool (*)(void* ctx, const uint8_t* data, Size len) noexcept;

struct Request {
    const char* method  = "GET";
    const char* url     = nullptr;
    const char* headers = nullptr;   ///< дополнительные "Name: value\r\n"
    const void* body    = nullptr;
    Size        body_len = 0u;
};

template <Size Capacity = 512u>
class Client {
public:
    Client() noexcept = default;

    Status request(Factory& factory, const Request& req, Response& resp,
                   BodySink sink = nullptr, void* sink_ctx = nullptr,
                   uint32_t timeout_ms = 5000u) noexcept {
        if (req.url == nullptr) return Status::InvalidArg;
        resp = Response{};
        Url url{};
        if (!parse_url(req.url, url)) return Status::InvalidArg;
        if (url.tls) return Status::NotSupported;
        if (req.body_len > 0u && req.body == nullptr) return Status::InvalidArg;

        Socket* s = nullptr;
        Status st = factory.create(Protocol::Tcp, s);
        if (st != Status::Ok || s == nullptr) return (st == Status::Ok) ? Status::NoMemory : st;

        Endpoint ep{};
        ep.port = url.port;
        st = factory.resolve(url.host, ep);
        if (st != Status::Ok) { s->close(); return st; }
        st = s->connect(ep, timeout_ms);
        if (st != Status::Ok) { s->close(); return st; }
        sock_ = s;

        // Заголовки запроса.
        st = send_str(req.method);
        if (st == Status::Ok) st = send_str(" ");
        if (st == Status::Ok) st = send_str(url.path);
        if (st == Status::Ok) st = send_str(" HTTP/1.1\r\nHost: ");
        if (st == Status::Ok) st = send_str(url.host);
        if (st == Status::Ok && url.port != 80u) {
            st = send_str(":");
            if (st == Status::Ok) st = send_u32(url.port);
        }
        if (st == Status::Ok) st = send_str("\r\nConnection: close\r\n");
        if (st == Status::Ok && req.body_len > 0u) {
            st = send_str("Content-Length: ");
            if (st == Status::Ok) st = send_u32(static_cast<uint32_t>(req.body_len));
            if (st == Status::Ok) st = send_str("\r\n");
        }
        if (st == Status::Ok && req.headers != nullptr) st = send_str(req.headers);
        if (st == Status::Ok) st = send_str("\r\n");
        if (st == Status::Ok && req.body_len > 0u) {
            st = send_all(req.body, req.body_len);
        }
        if (st != Status::Ok) { s->close(); return st; }

        st = read_headers(resp, timeout_ms);
        if (st != Status::Ok) { s->close(); return st; }

        if (resp.chunked) st = read_chunked(sink, sink_ctx, timeout_ms);
        else if (resp.content_length > 0u) st = read_exact_body(resp.content_length, sink, sink_ctx, timeout_ms);
        else st = read_until_close(sink, sink_ctx, timeout_ms);

        s->close();
        sock_ = nullptr;
        return st;
    }

    Status get(Factory& factory, const char* url, Response& resp,
               BodySink sink = nullptr, void* ctx = nullptr,
               uint32_t timeout_ms = 5000u) noexcept {
        Request r{};
        r.url = url;
        return request(factory, r, resp, sink, ctx, timeout_ms);
    }

    Status post(Factory& factory, const char* url, const void* body, Size len,
                Response& resp, BodySink sink = nullptr, void* ctx = nullptr,
                uint32_t timeout_ms = 5000u) noexcept {
        Request r{};
        r.method = "POST";
        r.url = url;
        r.body = body;
        r.body_len = len;
        r.headers = "Content-Type: application/octet-stream\r\n";
        return request(factory, r, resp, sink, ctx, timeout_ms);
    }

private:
    Status send_str(const char* s) noexcept {
        if (s == nullptr) return Status::InvalidArg;
        Size n = 0u;
        while (s[n] != '\0') ++n;
        return send_all(s, n);
    }
    Status send_u32(uint32_t v) noexcept {
        char b[11]{};
        uint8_t i = 0u;
        do { b[i++] = static_cast<char>('0' + (v % 10u)); v /= 10u; } while (v != 0u);
        char out[11]{};
        for (uint8_t k = 0u; k < i; ++k) out[k] = b[i - 1u - k];
        return send_all(out, i);
    }
    Status send_all(const void* p, Size n) noexcept {
        const auto* b = static_cast<const uint8_t*>(p);
        Size done = 0u;
        while (done < n) {
            Size sent = 0u;
            const Status st = sock_->send(b + done, n - done, sent);
            if (st != Status::Ok) return st;
            if (sent == 0u) return Status::Aborted;
            done += sent;
        }
        return Status::Ok;
    }

    Status recv_some(void* p, Size cap, Size& got, uint32_t timeout_ms) noexcept {
        return sock_->recv(p, cap, got, timeout_ms);
    }

    Status read_line(char* out, Size cap, uint32_t timeout_ms) noexcept {
        Size n = 0u;
        while (n + 1u < cap) {
            uint8_t c = 0u;
            Size got = 0u;
            Status st = recv_some(&c, 1u, got, timeout_ms);
            if (st != Status::Ok) return st;
            if (got == 0u) return Status::Unavailable;
            if (c == '\n') {
                if (n > 0u && out[n - 1u] == '\r') --n;
                out[n] = '\0';
                return Status::Ok;
            }
            out[n++] = static_cast<char>(c);
        }
        out[cap - 1u] = '\0';
        return Status::NoMemory;
    }

    static bool starts_with(const char* s, const char* p) noexcept {
        while (*p != '\0') { if (*s++ != *p++) return false; }
        return true;
    }
    static uint64_t parse_u64(const char* s) noexcept {
        uint64_t v = 0u;
        while (*s >= '0' && *s <= '9') { v = v * 10u + static_cast<uint64_t>(*s++ - '0'); }
        return v;
    }

    Status read_headers(Response& resp, uint32_t timeout_ms) noexcept {
        char line[192]{};
        Status st = read_line(line, sizeof(line), timeout_ms);
        if (st != Status::Ok) return st;
        // "HTTP/1.1 200 OK"
        if (!starts_with(line, "HTTP/")) return Status::Corruption;
        const char* p = line;
        while (*p != '\0' && *p != ' ') ++p;
        while (*p == ' ') ++p;
        resp.status = 0;
        while (*p >= '0' && *p <= '9') resp.status = resp.status * 10 + (*p++ - '0');
        if (resp.status < 100 || resp.status > 599) return Status::Corruption;

        for (;;) {
            st = read_line(line, sizeof(line), timeout_ms);
            if (st != Status::Ok) return st;
            if (line[0] == '\0') return Status::Ok;
            if (starts_with(line, "Content-Length:")) {
                const char* q = line + 15;
                while (*q == ' ') ++q;
                resp.content_length = parse_u64(q);
            } else if (starts_with(line, "Transfer-Encoding:") &&
                       (starts_with(line + 18, " chunked") || starts_with(line + 18, "chunked"))) {
                resp.chunked = true;
            } else if (starts_with(line, "Content-Type:")) {
                const char* q = line + 13;
                while (*q == ' ') ++q;
                Size i = 0u;
                while (*q != '\0' && i + 1u < sizeof(resp.content_type)) {
                    resp.content_type[i++] = *q++;
                }
                resp.content_type[i] = '\0';
            }
        }
    }

    Status read_exact_body(uint64_t len, BodySink sink, void* ctx,
                           uint32_t timeout_ms) noexcept {
        uint8_t buf[Capacity]{};
        while (len > 0u) {
            const Size want = (len > sizeof(buf)) ? sizeof(buf) : static_cast<Size>(len);
            Size got = 0u;
            const Status st = recv_some(buf, want, got, timeout_ms);
            if (st != Status::Ok) return st;
            if (got == 0u) return Status::Unavailable;
            if (sink != nullptr && !sink(ctx, buf, got)) return Status::Aborted;
            len -= got;
        }
        return Status::Ok;
    }

    Status read_chunked(BodySink sink, void* ctx, uint32_t timeout_ms) noexcept {
        char line[64]{};
        for (;;) {
            Status st = read_line(line, sizeof(line), timeout_ms);
            if (st != Status::Ok) return st;
            uint64_t n = 0u;
            const char* p = line;
            bool any = false;
            while (true) {
                uint8_t d = 0u;
                if (*p >= '0' && *p <= '9') d = static_cast<uint8_t>(*p - '0');
                else if (*p >= 'a' && *p <= 'f') d = static_cast<uint8_t>(*p - 'a' + 10);
                else if (*p >= 'A' && *p <= 'F') d = static_cast<uint8_t>(*p - 'A' + 10);
                else break;
                n = n * 16u + d;
                any = true;
                ++p;
            }
            if (!any) return Status::Corruption;
            if (n == 0u) return Status::Ok;   // trailer игнорируем
            st = read_exact_body(n, sink, ctx, timeout_ms);
            if (st != Status::Ok) return st;
            (void)read_line(line, sizeof(line), timeout_ms);  // CRLF после chunk
        }
    }

    Status read_until_close(BodySink sink, void* ctx, uint32_t timeout_ms) noexcept {
        uint8_t buf[Capacity]{};
        for (;;) {
            Size got = 0u;
            const Status st = recv_some(buf, sizeof(buf), got, timeout_ms);
            if (st == Status::Timeout) return sink == nullptr ? Status::Ok : st;
            if (st == Status::Unavailable) return Status::Ok;
            if (st != Status::Ok) return st;
            if (got == 0u) return Status::Ok;
            if (sink != nullptr && !sink(ctx, buf, got)) return Status::Aborted;
        }
    }

    Socket* sock_ = nullptr;
};

} // namespace embed::net::http
