#include "net.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <cstdlib>
#include <cstring>

namespace wsmud {
namespace net {

namespace {

void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

// ---------- TcpConn ----------

bool TcpConn::begin_connect(const std::string& host, const std::string& port) {
    close();
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { error = "socket 创建失败: " + std::string(strerror(errno)); return false; }
    set_nonblock(s);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        ::close(s);
        error = "域名解析失败: " + host;
        return false;
    }
    // 直接拷贝 getaddrinfo 返回的 sockaddr_in（IP 与端口均已正确填充/字节序处理）
    struct sockaddr_in addr = {};
    const struct addrinfo* chosen = res;
    std::memcpy(&addr, chosen->ai_addr, sizeof(addr));

    int rc = ::connect(s, (struct sockaddr*)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        error = "连接失败: " + std::string(strerror(errno));
        freeaddrinfo(res);
        ::close(s);
        return false;
    }
    freeaddrinfo(res);
    fd = s;
    connecting = true;
    error.clear();
    return true;
}

void TcpConn::close() {
    if (fd >= 0) { ::close(fd); fd = -1; }
    connecting = false;
    in.clear();
    out.clear();
}

bool TcpConn::send(const char* data, std::size_t len) {
    if (fd < 0) return false;
    out.insert(out.end(), data, data + len);
    return true;
}

void TcpConn::flush() {
    while (!out.empty()) {
        ssize_t n = ::send(fd, out.data(), out.size(), 0);
        if (n > 0) {
            out.erase(out.begin(), out.begin() + n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        error = std::string("发送失败: ") + strerror(errno);
        close();
        return;
    }
}

int TcpConn::read_more() {
    if (fd < 0) return -1;
    char buf[16384];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            in.insert(in.end(), buf, buf + n);
            continue;
        }
        if (n == 0) return -2;  // 对端关闭
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        error = std::string("读取失败: ") + strerror(errno);
        return -1;
    }
}

// ---------- HttpReq ----------

void HttpReq::start(const std::string& url, const std::string& method, const std::string& req_body, int64_t now_ms,
                    const char* content_type) {
    close();
    state = State::Connecting;
    error.clear();
    status = 0;
    body.clear();
    deadline_ms = now_ms + 15000;

    // 解析 url：http://host[:port]/path
    std::string rest;
    if (url.rfind("http://", 0) == 0) rest = url.substr(7);
    else { fail("仅支持 http://"); return; }
    std::size_t slash = rest.find('/');
    std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string host = host_port;
    std::string port = "80";
    std::size_t colon = host_port.find(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        port = host_port.substr(colon + 1);
    }

    if (!tcp_.begin_connect(host, port)) {
        fail(tcp_.error);
        return;
    }

    std::string head = method + " " + path + " HTTP/1.1\r\n"
                       "Host: " + host_port + "\r\n"
                       "Accept: application/json\r\n"
                       "Connection: close\r\n";
    if (!req_body.empty()) {
        head += std::string("Content-Type: ") + (content_type && content_type[0] ? content_type : "application/json") + "\r\n";
        head += "Content-Length: " + std::to_string(req_body.size()) + "\r\n";
    }
    head += "User-Agent: wsmud2-headless/1.0\r\n\r\n";
    request_head_ = head + req_body;
    head_sent_ = 0;
}

void HttpReq::close() {
    tcp_.close();
    state = State::Idle;
    rbuf_.clear();
    request_head_.clear();
    head_sent_ = 0;
    head_done_ = false;
    content_len_ = 0;
    chunked_ = false;
    chunk_need_ = 0;
    finished_ = false;
}

void HttpReq::finish_ok() {
    state = State::Done;
    tcp_.close();
}

void HttpReq::fail(const std::string& msg) {
    if (error.empty()) error = msg;
    state = State::Failed;
    tcp_.close();
}

bool HttpReq::tick(int64_t now_ms) {
    if (state == State::Done || state == State::Failed) return true;
    if (state == State::Idle) return true;
    if (now_ms > deadline_ms) { fail("HTTP 请求超时"); return true; }

    if (state == State::Connecting) {
        if (!tcp_.connecting) { fail(tcp_.error.empty() ? "连接未完成" : tcp_.error); return true; }
        // 非阻塞 connect：需等待 socket 可写（连接建立或有错误）
        struct pollfd p = {tcp_.fd, POLLOUT, 0};
        if (poll(&p, 1, 0) == 0) return false;  // 连接仍在进行
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(tcp_.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
            fail("getsockopt 失败"); return true;
        }
        if (err != 0) { fail("连接失败: " + std::string(strerror(err))); return true; }
        tcp_.connecting = false;
        state = State::Sending;
        return false;
    }

    // 可写：发送请求
    if (state == State::Sending) {
        while (head_sent_ < request_head_.size()) {
            ssize_t n = ::send(tcp_.fd, request_head_.data() + head_sent_, request_head_.size() - head_sent_, 0);
            if (n > 0) { head_sent_ += static_cast<std::size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            fail("发送请求失败"); return true;
        }
        if (head_sent_ >= request_head_.size()) {
            state = State::Receiving;
            request_head_.clear();
            head_sent_ = 0;
        }
        return false;
    }

    if (state == State::Receiving) {
        if (tcp_.fd < 0) { fail("连接已关闭"); return true; }
        int rc = tcp_.read_more();
        if (rc == -1) { fail(tcp_.error.empty() ? "读取失败" : tcp_.error); return true; }
        bool peer_closed = (rc == -2);  // 对端关闭（FIN 可能与数据同批到达，需先解析已收数据）
        // 解析响应头（未完成时）
        if (!head_done_) {
            // 等待 \r\n\r\n
            std::string all(tcp_.in.begin(), tcp_.in.end());
            std::size_t hd = all.find("\r\n\r\n");
            if (hd == std::string::npos) {
                // 数据未凑齐就 EOF 才算响应不完整
                if (peer_closed) { fail("响应不完整，连接提前关闭"); return true; }
                return false;  // 还需更多数据
            }
            std::string head = all.substr(0, hd);
            tcp_.in.erase(tcp_.in.begin(), tcp_.in.begin() + static_cast<std::ptrdiff_t>(hd) + 4);
            // 状态行
            std::size_t sp1 = head.find(' ');
            std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : head.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) { fail("响应状态行非法"); return true; }
            status = std::atoi(head.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
            // 头字段
            std::size_t pos = head.find("\r\n");
            while (pos != std::string::npos) {
                std::size_t nxt = head.find("\r\n", pos + 2);
                std::string line = head.substr(pos + 2, nxt == std::string::npos ? std::string::npos : nxt - pos - 2);
                std::size_t c = line.find(':');
                if (c != std::string::npos) {
                    std::string name = line.substr(0, c);
                    std::string val = line.substr(c + 1);
                    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
                    if (name == "Content-Length") content_len_ = std::strtoull(val.c_str(), nullptr, 10);
                    else if (name == "Transfer-Encoding") chunked_ = (val.find("chunked") != std::string::npos);
                }
                if (nxt == std::string::npos) break;
                pos = nxt;
            }
            head_done_ = true;
            if (status < 200 || status >= 300) {
                // 非 2xx：把剩余数据作为 body 收集后失败（不区分传输编码，原样取即可）
                chunked_ = false;
                content_len_ = 0;
            }
        }
        // 收集 body
        if (head_done_ && !finished_) {
            if (chunked_) {
                decode_chunks(peer_closed);
            } else {
                body.append(tcp_.in.begin(), tcp_.in.end());
                tcp_.in.clear();
                if (peer_closed || (content_len_ > 0 && body.size() >= content_len_))
                    finished_ = true;
            }
        }
        if (finished_) {
            if (status < 200 || status >= 300) {
                fail("HTTP " + std::to_string(status) + ": " + (body.size() > 160 ? body.substr(0, 160) : body));
            } else {
                finish_ok();
            }
            return true;
        }
        return false;
    }
    return false;
}

// 解码分块传输（Transfer-Encoding: chunked）：
//   块 = <HEXSIZE[;扩展]\r\n> <数据> \r\n，以 <0> \r\n 结束。
// 大小行可能跨包、块数据可能不完整 → 数据不足时留下一次继续（peer_closed 作为兜底结束）。
void HttpReq::decode_chunks(bool peer_closed) {
    for (;;) {
        if (!tcp_.in.empty() && chunk_need_ == 0) {
            // 读一块大小行（含 CRLF）
            std::size_t nl = std::string::npos;
            for (std::size_t i = 0; i + 1 < tcp_.in.size(); ++i) {
                if (tcp_.in[i] == '\r' && tcp_.in[i + 1] == '\n') { nl = i; break; }
            }
            if (nl == std::string::npos) {
                if (peer_closed) { finished_ = true; return; }   // 兜底：EOF 且无完整块尾
                return;  // 等更多数据
            }
            std::size_t semi = std::string::npos;
            for (std::size_t i = 0; i < nl; ++i)
                if (tcp_.in[i] == ';') { semi = i; break; }
            std::string hs(tcp_.in.begin(), tcp_.in.begin() + (semi == std::string::npos ? nl : semi));
            tcp_.in.erase(tcp_.in.begin(), tcp_.in.begin() + static_cast<std::ptrdiff_t>(nl) + 2);
            unsigned long sz = std::strtoul(hs.c_str(), nullptr, 16);
            if (sz == 0) {  // 结束块
                finished_ = true;
                return;
            }
            chunk_need_ = static_cast<std::size_t>(sz);
        }
        if (chunk_need_ > 0) {
            if (tcp_.in.empty()) {
                if (peer_closed) { finished_ = true; return; }   // 兜底
                return;  // 等数据
            }
            std::size_t take = tcp_.in.size() > chunk_need_ ? chunk_need_ : tcp_.in.size();
            body.append(tcp_.in.begin(), tcp_.in.begin() + static_cast<std::ptrdiff_t>(take));
            tcp_.in.erase(tcp_.in.begin(), tcp_.in.begin() + static_cast<std::ptrdiff_t>(take));
            chunk_need_ -= take;
            if (chunk_need_ == 0) {
                // 消费块尾 CRLF
                if (tcp_.in.size() < 2) {
                    if (peer_closed) { finished_ = true; return; }
                    return;  // 等块尾 CRLF
                }
                tcp_.in.erase(tcp_.in.begin(), tcp_.in.begin() + 2);
            }
        }
    }
}

}  // namespace net
}  // namespace wsmud
