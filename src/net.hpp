// 非阻塞 TCP 连接与异步 HTTP 客户端（单线程 poll 驱动）
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace wsmud {
namespace net {

// 一个非阻塞 TCP 连接。in 是已读入待消费的数据，out 是待发送的数据。
struct TcpConn {
    int fd = -1;
    std::vector<char> in;
    std::vector<char> out;
    bool connecting = false;   // 正在非阻塞 connect
    std::string error;         // 最近一次错误
    bool connected() const { return fd >= 0 && !connecting; }

    // 发起非阻塞连接（host 为域名或 IP，port 为端口字符串）
    bool begin_connect(const std::string& host, const std::string& port);
    // 关闭并释放
    void close();
    // 向输出缓冲追加数据（返回是否成功，连接无效返回 false）
    bool send(const char* data, std::size_t len);
    bool send(const std::string& s) { return send(s.data(), s.size()); }
    // 可写时把 out 尽量刷到内核（由主循环调用）
    void flush();
    // 是否需要在 poll 中关注可写事件（正在连接或有待发送数据）。
    // 已连接的空闲 socket 恒可写，若总是轮询 POLLOUT 会令 poll 永不阻塞、单核忙等跑满
    bool wants_write() const { return connecting || !out.empty(); }
    // 可读时把内核数据读入 in（由主循环调用）；返回 0=正常, -1=出错, -2=对端关闭
    int read_more();
};

// 异步 HTTP 请求（GET/POST，JSON body），一次一个请求
class HttpReq {
public:
    enum class State { Idle, Connecting, Sending, Receiving, Done, Failed };

    State state = State::Idle;
    int status = 0;            // HTTP 状态码（2xx 为成功）
    std::string body;          // 响应体
    std::string error;         // 失败原因
    int64_t deadline_ms = 0;   // 超时时刻（毫秒时间戳）

    // 发起请求。url 形如 http://host:port/path（目前仅支持 http，wsmud2 是明文 HTTP）
    // method: GET / POST；body 非空时以 content_type（默认 application/json）发送
    void start(const std::string& url, const std::string& method, const std::string& body, int64_t now_ms,
               const char* content_type = nullptr);
    void close();
    TcpConn& conn() { return tcp_; }
    bool done() const { return state == State::Done || state == State::Failed; }
    // 主循环驱动：返回 true 表示已结束（Done/Failed）
    bool tick(int64_t now_ms);

private:
    TcpConn tcp_;
    std::string request_head_;  // 未发送完的请求头
    std::size_t head_sent_ = 0;
    std::string rbuf_;          // 响应累积缓冲
    bool head_done_ = false;
    std::size_t content_len_ = 0;
    bool chunked_ = false;
    std::size_t chunk_need_ = 0;  // chunked：当前块剩余待解码字节数（0=等待下一块大小行）
    bool finished_ = false;

    void finish_ok();
    void fail(const std::string& msg);
    void decode_chunks(bool peer_closed);   // 分块响应体解码
};

}  // namespace net
}  // namespace wsmud
