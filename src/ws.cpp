#include "ws.hpp"

#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <array>
#include <cstring>
#include <random>
#include <vector>

#include "net.hpp"

namespace wsmud {
namespace ws {

namespace {

// ---------- SHA1 ----------
struct Sha1 {
    uint32_t h[5];
    uint64_t len = 0;

    Sha1() { h[0] = 0x67452301; h[1] = 0xEFCDAB89; h[2] = 0x98BADCFE; h[3] = 0x10325476; h[4] = 0xC3D2E1F0; }

    // n 为 0 时 v>>32 属移位超宽（UB），单独处理
    static uint32_t rol(uint32_t v, int n) { return n ? ((v << n) | (v >> (32 - n))) : v; }

    void process_block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = rol(x, 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const uint8_t* data, std::size_t n) {
        len += n;
        std::array<uint8_t, 64> buf;
        std::size_t off = 0;
        while (n >= 64) { process_block(data + off); off += 64; n -= 64; }
        if (n) std::memcpy(buf.data(), data + off, n);
        (void)buf;
    }
};

// 一次性 sha1（输入全量在内存，简单可靠）
std::array<uint8_t, 20> sha1_once(const uint8_t* data, std::size_t n) {
    std::array<uint8_t, 20> out{};
    Sha1 ctx;
    // 填充与长度按 RFC 3174
    std::size_t total = n + 1;
    std::size_t pad = (64 - (total % 64)) % 64;
    if (pad < 8) pad += 64;
    std::vector<uint8_t> buf(data, data + n);
    buf.push_back(0x80);
    buf.resize(n + 1 + pad, 0);
    uint64_t bitlen = uint64_t(n) * 8;
    for (int i = 0; i < 8; ++i)
        buf[buf.size() - 1 - i] = uint8_t(bitlen >> (8 * i));
    for (std::size_t i = 0; i < buf.size(); i += 64)
        ctx.process_block(buf.data() + i);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = uint8_t(ctx.h[i] >> 24);
        out[i * 4 + 1] = uint8_t(ctx.h[i] >> 16);
        out[i * 4 + 2] = uint8_t(ctx.h[i] >> 8);
        out[i * 4 + 3] = uint8_t(ctx.h[i]);
    }
    return out;
}

// ---------- Base64 ----------
const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, std::size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += B64[(v >> 18) & 63]; out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63]; out += B64[v & 63];
    }
    if (i + 1 == n) {
        uint32_t v = data[i] << 16;
        out += B64[(v >> 18) & 63]; out += B64[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == n) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += B64[(v >> 18) & 63]; out += B64[(v >> 12) & 63]; out += B64[(v >> 6) & 63]; out += "=";
    }
    return out;
}

}  // namespace

std::string random_key() {
    std::array<uint8_t, 16> key;
    static thread_local std::mt19937 rng(std::random_device{}());
    for (auto& b : key) b = static_cast<uint8_t>(rng() & 0xFF);
    return base64_encode(key.data(), key.size());
}

std::string accept_key(const std::string& key) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string src = key + GUID;
    auto digest = sha1_once(reinterpret_cast<const uint8_t*>(src.data()), src.size());
    return base64_encode(digest.data(), digest.size());
}

// ---------- WsClient ----------

void WsClient::connect(const std::string& host, const std::string& port,
                       const std::string& origin, const std::string& path, int64_t now_ms) {
    close();
    state = State::Connecting;
    error.clear();
    origin_ = origin;
    host_port_ = host + ":" + port;
    deadline_ms = now_ms + 12000;
    std::string key = random_key();
    request_head_ =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host_port_ + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: " + origin + "\r\n"
        "User-Agent: Mozilla/5.0\r\n\r\n";
    head_sent_ = 0;
    if (!tcp_.begin_connect(host, port)) {
        fail(tcp_.error);
        return;
    }
}

void WsClient::close() {
    tcp_.close();
    state = State::Idle;
    request_head_.clear();
    hs_buf_.clear();
    payload_accum_.clear();
    frag_started_ = false;
    frag_opcode_ = 0;
    received_close_ = false;
}

void WsClient::fail(const std::string& msg) {
    if (error.empty()) error = msg;
    state = State::Closed;
    tcp_.close();
}

bool WsClient::tick(int64_t now_ms) {
    if (state == State::Open || state == State::Closed || state == State::Idle) return state == State::Closed;
    if (now_ms > deadline_ms) { fail("WebSocket 握手超时"); return true; }

    if (state == State::Connecting) {
        if (!tcp_.connecting) { fail(tcp_.error.empty() ? "连接未完成" : tcp_.error); return true; }
        // 非阻塞 connect：等待 socket 可写（连接建立或有错误）
        struct pollfd p = {tcp_.fd, POLLOUT, 0};
        if (poll(&p, 1, 0) == 0) return false;  // 连接仍在进行
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(tcp_.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) { fail("getsockopt 失败"); return true; }
        if (err != 0) { fail("连接失败: " + std::string(strerror(err))); return true; }
        tcp_.connecting = false;
        state = State::Handshake;
        return false;
    }

    if (state == State::Handshake) {
        // 发送握手
        while (head_sent_ < request_head_.size()) {
            ssize_t n = ::send(tcp_.fd, request_head_.data() + head_sent_, request_head_.size() - head_sent_, 0);
            if (n > 0) { head_sent_ += static_cast<std::size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            fail("握手发送失败"); return true;
        }
        // 读取响应
        bool peer_closed = false;
        if (tcp_.fd >= 0) {
            int rc = tcp_.read_more();
            if (rc == -1) { fail(tcp_.error.empty() ? "握手读取失败" : tcp_.error); return true; }
            if (rc == -2) peer_closed = true;  // FIN 可能与数据同批到达，先解析已收数据
        }
        hs_buf_.append(tcp_.in.begin(), tcp_.in.end());
        tcp_.in.clear();
        std::size_t hd = hs_buf_.find("\r\n\r\n");
        if (hd == std::string::npos) {
            if (peer_closed) { fail("握手响应不完整，连接关闭"); return true; }
            return false;  // 还需更多
        }
        std::string head = hs_buf_.substr(0, hd);
        hs_buf_.erase(0, hd + 4);
        if (head.find("101") == std::string::npos) { fail("握手响应非 101"); return true; }
        // 校验 Sec-WebSocket-Accept
        std::size_t ak = head.find("Sec-WebSocket-Accept:");
        if (ak != std::string::npos) {
            std::size_t vl = head.find("\r\n", ak);
            std::string val = head.substr(ak + 21, vl == std::string::npos ? std::string::npos : vl - ak - 21);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            // 计算期望值（从原始 key 推导）
            std::string want;
            std::string srckey;
            std::size_t kp = request_head_.find("Sec-WebSocket-Key:");
            if (kp != std::string::npos) {
                std::size_t ke = request_head_.find("\r\n", kp);
                srckey = request_head_.substr(kp + 19, ke == std::string::npos ? std::string::npos : ke - kp - 19);
                while (!srckey.empty() && (srckey.front() == ' ' || srckey.front() == '\t')) srckey.erase(srckey.begin());
            }
            want = accept_key(srckey);
            if (val != want) { fail("Sec-WebSocket-Accept 校验失败"); return true; }
        }
        // 剩余数据作为帧输入
        if (!hs_buf_.empty()) {
            std::vector<std::string> msgs;
            if (!parse_frames(hs_buf_.data(), hs_buf_.size(), msgs)) return true;
            hs_buf_.clear();
        }
        state = State::Open;
        return false;
    }
    return false;
}

bool WsClient::send_frame(int opcode, const char* payload, std::size_t len) {
    if (!open()) return false;
    std::vector<char> frame;
    frame.reserve(len + 14);
    frame.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode
    // 客户端帧必须带 mask
    uint8_t mask[4];
    static thread_local std::mt19937 rng(std::random_device{}());
    for (auto& b : mask) b = static_cast<uint8_t>(rng() & 0xFF);
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));
    } else if (len < 65536) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        uint64_t l = len;
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((l >> (8 * i)) & 0xFF));
    }
    frame.insert(frame.end(), mask, mask + 4);
    for (std::size_t i = 0; i < len; ++i)
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    return tcp_.send(frame.data(), frame.size());
}

bool WsClient::send_text(const std::string& text) {
    return send_frame(0x1, text.data(), text.size());
}

bool WsClient::send_ping() {
    return send_frame(0x9, "", 0);
}

void WsClient::send_close() {
    if (open()) {
        send_frame(0x8, "", 0);
        state = State::Closing;
    }
}

// 从一段数据解析帧（数据可能跨多个帧）
bool WsClient::parse_frames(const char* data, std::size_t len, std::vector<std::string>& out) {
    std::size_t pos = 0;
    while (pos < len) {
        if (len - pos < 2) break;  // 帧头不完整
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data + pos);
        bool fin = (p[0] & 0x80) != 0;
        int opcode = p[0] & 0x0F;
        bool masked = (p[1] & 0x80) != 0;
        uint64_t plen = p[1] & 0x7F;
        std::size_t off = 2;
        if (plen == 126) {
            if (len - pos < 4) break;
            plen = (uint64_t(p[2]) << 8) | p[3];
            off = 4;
        } else if (plen == 127) {
            if (len - pos < 10) break;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | p[2 + i];
            off = 10;
        }
        if (masked) off += 4;  // 服务端帧不应有 mask
        // 用减法比较避免 off + plen 溢出（对端可给出 2^64-1 这类长度，溢出会让检查失效并越界读取）
        if (off > len - pos) break;                              // 帧头不完整
        if (plen > static_cast<uint64_t>(len - pos - off)) break;  // payload 不完整
        const char* payload = data + pos + off;
        std::string body(payload, static_cast<std::size_t>(plen));
        if (masked) {
            // mask 键位于 payload 之前 4 字节（off 已含这 4 字节）；
            // 原实现读 payload[4+i%4] 既取错位置、又可能越过帧尾（UB）
            const uint8_t* mkey = reinterpret_cast<const uint8_t*>(data + pos + off - 4);
            for (std::size_t i = 0; i < body.size(); ++i)
                body[i] = static_cast<char>(static_cast<uint8_t>(body[i]) ^ mkey[i % 4]);
        }
        pos += off + static_cast<std::size_t>(plen);

        switch (opcode) {
            case 0x1:  // text
            case 0x2:  // binary（游戏只用 text，binary 也按原样输出）
                if (frag_started_) { fail("分片中途收到新的数据帧"); return false; }
                if (fin) { out.push_back(std::move(body)); }
                else { frag_started_ = true; frag_opcode_ = opcode; payload_accum_ = std::vector<char>(body.begin(), body.end()); }
                break;
            case 0x0:  // continuation
                if (!frag_started_) { fail("收到意外的 continuation 帧"); return false; }
                payload_accum_.insert(payload_accum_.end(), body.begin(), body.end());
                if (fin) {
                    out.emplace_back(payload_accum_.begin(), payload_accum_.end());
                    frag_started_ = false;
                }
                break;
            case 0x8:  // close
                received_close_ = true;
                send_frame(0x8, "", 0);
                state = State::Closed;
                tcp_.close();
                return true;
            case 0x9:  // ping → 自动回 pong
                send_frame(0xA, body.data(), body.size());
                break;
            case 0xA:  // pong
                break;
            default:
                break;  // 未知控制帧忽略
        }
    }
    // 消费掉已完整解析的部分。
    // 注意：本函数也用于握手阶段（数据在 hs_buf_ 中，tcp_.in 已被清空），
    // 此时 pos 可能大于 tcp_.in.size()，直接 erase 会构造越界迭代器（UB），故做上界钳制。
    std::size_t consume = pos < tcp_.in.size() ? pos : tcp_.in.size();
    tcp_.in.erase(tcp_.in.begin(), tcp_.in.begin() + static_cast<std::ptrdiff_t>(consume));
    return true;
}

bool WsClient::drain(std::vector<std::string>& out) {
    if (state != State::Open || tcp_.fd < 0) return true;
    int rc = tcp_.read_more();
    if (rc == -1) { fail(tcp_.error.empty() ? "读取失败" : tcp_.error); return false; }
    if (rc == -2) { error = "对端关闭连接"; state = State::Closed; tcp_.close(); return false; }
    if (!tcp_.in.empty()) {
        return parse_frames(tcp_.in.data(), tcp_.in.size(), out);
    }
    return true;
}

}  // namespace ws
}  // namespace wsmud
