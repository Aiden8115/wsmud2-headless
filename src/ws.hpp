// RFC6455 WebSocket 客户端：握手、帧编解码、ping/pong、分片
// 零依赖（自实现 SHA1 与 Base64），仅处理 http(s) 之外的 ws:// 明文场景
#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "net.hpp"

namespace wsmud {
namespace ws {

// 生成 16 字节随机 key 并做 Base64（用于 Sec-WebSocket-Key）
std::string random_key();
// 计算 Sec-WebSocket-Accept = base64(sha1(key + GUID))
std::string accept_key(const std::string& key);

class WsClient {
public:
    enum class State { Idle, Connecting, Handshake, Open, Closing, Closed };

    State state = State::Idle;
    std::string error;
    int64_t deadline_ms = 0;

    // 发起连接（host:port，origin 为游戏网站的 Origin 头）
    void connect(const std::string& host, const std::string& port,
                 const std::string& origin, const std::string& path, int64_t now_ms);
    void close();
    bool open() const { return state == State::Open; }
    bool finished() const { return state == State::Closed; }

    net::TcpConn& conn() { return tcp_; }

    bool send_text(const std::string& text);
    bool send_ping();
    void send_close();

    // 驱动连接/握手阶段；返回 true 表示已经结束（Closed/Failed）
    bool tick(int64_t now_ms);
    // 解析 tcp_.in 中的帧，产出应用文本消息（追加到 out）；返回是否出错
    bool drain(std::vector<std::string>& out);

private:
    net::TcpConn tcp_;
    std::string request_head_;
    std::string host_port_;
    std::string origin_;
    std::size_t head_sent_ = 0;
    std::string hs_buf_;
    std::vector<char> payload_accum_;
    bool frag_started_ = false;
    int frag_opcode_ = 0;
    bool received_close_ = false;

    void fail(const std::string& msg);
    // 从 buf 解析帧；append 到 out；pong 回调自动回复
    bool parse_frames(const char* data, std::size_t len, std::vector<std::string>& out);
    bool send_frame(int opcode, const char* payload, std::size_t len);
};

}  // namespace ws
}  // namespace wsmud
