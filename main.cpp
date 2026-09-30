// main.cpp — 程序入口：全局状态定义、输出层、经典模式、终端探测、main()
// 命令处理见 commands.cpp，全屏 TUI 见 tui_ui.cpp（共享声明见 app.hpp）
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"

// ---------- 全局状态（声明见 app.hpp） ----------

int slots = 5;                    // 顶栏标签页数量（初始 5，可 F7 追加 / DEL 删除）
std::vector<std::unique_ptr<Account>> accounts;
bool quitting = false;
int sel = 0;                      // 当前选中槽位（0-based）
bool game_mode = false;           // F6：false=程序命令 true=游戏命令
std::vector<std::vector<std::string>> acc_logs;  // 每账号独立日志缓冲（文本区，全屏模式）
std::vector<std::vector<std::string>> acc_chat_logs;  // 每账号独立聊天缓冲（聊天区，全屏模式）
std::vector<std::vector<std::string>> acc_pkt_logs;   // 每账号独立网络包缓冲（右栏"网络包"，全屏模式）
std::string cmd_buf;              // 命令行当前输入
int scroll_offset = 0;            // 日志区向上滚动行数（0=显示最新）
int chat_scroll_offset = 0;       // 聊天区向上滚动行数（0=显示最新，Shift+↑/↓）
int pkt_scroll_offset = 0;        // 网络包栏向上滚动行数（0=显示最新，[上翻 / ]下翻）
bool g_tui = false;               // true=全屏 TUI，false=经典行式输出
InputStage input_stage = InputStage::None;
bool trig_view = false;           // F8 触发器列表视图（覆盖输出区，只读）
int list_cursor = 0;              // 列表视图键盘高亮行（0=首条触发器）
View view = View::Logs;
std::vector<ClickZone> g_zones;
std::vector<wsmud::trigger::Trigger> g_trig_cfg;  // 当前生效的全局触发器配置（trigger.json，程序只读）
TrigEditor trig_editor;                          // F8 触发器编辑屏状态（TUI 内）

// ---------- 输出 ----------

namespace {
constexpr int LOG_CAP = 500;      // 输出区日志上限
constexpr int LOG_CAP_PKT = 200;  // 网络包栏容量（按包记），丢最旧

// JSON 美观化：扫描原始单行串，在 { } [ ] , 后换行并按深度缩进；字符串字面量原样跳过（含 \" \\ 转义），
// 使右侧网络包栏以"每行一个字段/元素"的多行缩进呈现，便于阅读与按栏宽折行。
std::string pretty_json(const std::string& in) {
    std::string out;
    int depth = 0;
    bool inStr = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (inStr) {
            out += c;
            if (c == '\\' && i + 1 < in.size()) out += in[++i];   // 转义序列整体保留
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; out += c; continue; }
        switch (c) {
            case '{': case '[':
                out += c;
                out += '\n';
                ++depth;
                out.append(static_cast<std::size_t>(depth) * 2, ' ');
                break;
            case '}': case ']':
                out += '\n';
                if (depth > 0) --depth;
                out.append(static_cast<std::size_t>(depth) * 2, ' ');
                out += c;
                break;
            case ',':
                out += c;
                out += '\n';
                out.append(static_cast<std::size_t>(depth) * 2, ' ');
                break;
            case ':':
                out += ": ";
                break;
            default:
                out += c;
        }
    }
    return out;
}

// 追加一行到日志缓冲：按 \n/\r 拆分多条、剔除控制字符（但保留 ANSI 颜色序列）、
// 单条截断到 400 字符、缓冲超过 LOG_CAP 时丢弃最旧
// （游戏 HTML 经 html_to_ansi 转出的 Truecolor 前景码必须保留，否则 TUI 下
//   所有本该着色的文本会退化成终端默认的白色；渲染端 fit() 已按 0 显示宽度透传转义码）
void push_logs(std::vector<std::string>& buf, const std::string& raw) {
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        if (cur.size() > 400) cur = cur.substr(0, 400);
        buf.push_back(cur);
        cur.clear();
    };
    for (std::size_t i = 0; i < raw.size();) {
        auto c = static_cast<unsigned char>(raw[i]);
        if (c == 0x1b) {  // 原样保留完整 ANSI 序列：引入符/参数/中间字节 + 1 个结束字节
            std::size_t start = i;
            ++i;
            while (i < raw.size()) {
                auto b = static_cast<unsigned char>(raw[i]);
                if ((b >= 0x20 && b <= 0x3F) || b == 0x5B || b == 0x5D) { ++i; continue; }
                break;
            }
            if (i < raw.size()) ++i;
            cur += raw.substr(start, i - start);
            continue;
        }
        if (c == '\n' || c == '\r') { flush(); ++i; continue; }
        if (c < 0x20 && c != '\t') { cur += ' '; ++i; continue; }  // 其他控制字符转空格
        cur += raw[i];
        ++i;
    }
    flush();
    while (buf.size() > LOG_CAP)
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(buf.size() - LOG_CAP));
}
}  // namespace

// 程序命令输出 → 当前选中账号的日志框（经典模式直接打印）
void out(const std::string& line) {
    if (g_tui) push_logs(acc_logs[static_cast<std::size_t>(sel)], line);
    else { std::printf("%s\n", line.c_str()); std::fflush(stdout); }
}

// 账号日志回调：写入该账号自己的缓冲（无论当前选中哪个槽位）。
// 标签页已按账号隔离，TUI 模式下无需再加 [账号N] 前缀；经典模式多账号混看，仍保留
void account_log(int index, const std::string& line) {
    if (g_tui) {
        push_logs(acc_logs[static_cast<std::size_t>(index - 1)], line);
    } else {
        std::printf("%s\n", ("[账号" + std::to_string(index) + "] " + line).c_str());
        std::fflush(stdout);
    }
}

// 聊天回调：写入该账号自己的聊天缓冲（聊天区），与文本日志分流
void account_chat(int index, const std::string& line) {
    if (g_tui) {
        push_logs(acc_chat_logs[static_cast<std::size_t>(index - 1)], line);
    } else {
        std::printf("%s\n", ("[聊天" + std::to_string(index) + "] " + line).c_str());
        std::fflush(stdout);
    }
}

// 网络包回调：写入该账号自己的网络包缓冲（右栏"网络包"）。经典模式无右栏，静默丢弃。
// 每包存一份完整多行字符串（JSON 美化，尚未按宽度折行——折行由渲染时按栏宽动态做）。
void account_packet(int index, const std::string& line) {
    if (!g_tui) return;
    std::string clean;
    clean.reserve(line.size());
    for (char ch : line) {  // 剔除异常控制符（防破坏整屏布局）；保留 \n 供后续按行折行
        auto uc = static_cast<unsigned char>(ch);
        if (uc < 0x20 && uc != '\n' && uc != '\t') clean += ' ';
        else clean += ch;
    }
    auto& buf = acc_pkt_logs[static_cast<std::size_t>(index - 1)];
    buf.push_back(pretty_json(clean));
    if (buf.size() > LOG_CAP_PKT) buf.erase(buf.begin());   // 按包计容量，丢最旧
}

// [ 上翻（看更早的包）/ ] 下翻（回到最新）。渲染会按实际行数钳制。
void pkt_scroll(bool up) {
    if (up) ++pkt_scroll_offset;
    else if (pkt_scroll_offset > 0) --pkt_scroll_offset;
}

// UTF-8 整字退格删除（中文等 3 字节字符一次删完）
void backspace_utf8(std::string& s) {
    if (s.empty()) return;
    std::size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;  // 跳过续字节
    s.erase(i);
}

// ---------- 经典模式（不支持全屏终端的回退） ----------

namespace {

std::string read_line_prompt(const std::string& prompt) {
    std::printf("%s", prompt.c_str());
    std::fflush(stdout);
    std::string line;
    int c;
    while ((c = std::getchar()) != EOF && c != '\n') line += static_cast<char>(c);
    return trim(line);
}

std::string read_password_masked(const std::string& prompt) {
    std::printf("%s", prompt.c_str());
    std::fflush(stdout);
    struct termios oldt{}, newt{};
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    newt.c_lflag &= ~ICANON;
    newt.c_cc[VMIN] = 1;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    std::string pass;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 0x7F) {
            if (!pass.empty()) { pass.pop_back(); std::printf("\b \b"); std::fflush(stdout); }
            continue;
        }
        pass += c;
        std::printf("*");
        std::fflush(stdout);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::printf("\n");
    std::fflush(stdout);
    return pass;
}

int read_int_prompt(const std::string& prompt, int def) {
    std::string s = read_line_prompt(prompt);
    if (s.empty()) return def;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    return static_cast<int>(v);
}

void startup_prompt() {
    std::printf("========================================\n");
    std::printf("   wsmud2 无头挂机客户端（经典模式）\n");
    std::printf("   连接: http://wsmud2.cn（武神传说）\n");
    std::printf("========================================\n");

    int count = read_int_prompt("要挂几个账号？(1-10) [默认2]: ", 2);
    if (count < 1) count = 1;
    if (count > 10) count = 10;
    accounts.resize(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        accounts[i] = std::make_unique<Account>();
        auto& a = *accounts[i];
        a.index = static_cast<int>(i) + 1;
        a.set_log(account_log);
        a.set_chat(account_chat);
    }
    reload_triggers();  // 经典模式：启动即加载 trigger.json

    for (std::size_t i = 0; i < accounts.size(); ++i) {
        auto& a = *accounts[i];
        std::printf("--- 账号 %d ---\n", a.index);
        a.account = read_line_prompt("账号: ");
        if (a.account.empty()) {
            std::printf("账号不能为空，将跳过该账号\n");
            a.stage = Account::Stage::None;
            continue;
        }
        a.password = read_password_masked("密码: ");
        if (a.password.empty()) {
            std::printf("密码为空，将跳过该账号\n");
            a.stage = Account::Stage::None;
            continue;
        }
        a.start_login();
    }

    std::printf("\n已启动 %d 个账号。运行中可输入 help 查看命令。\n", count);
    std::fflush(stdout);
}

void classic_run_loop() {
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    std::string input_buf;

    std::vector<struct pollfd> fds;
    fds.reserve(1 + accounts.size() * 2);

    while (!quitting) {
        fds.clear();
        struct pollfd in = {STDIN_FILENO, POLLIN, 0};
        fds.push_back(in);
        for (auto& a : accounts) {
            if (a->http.conn().fd >= 0) {
                // 仅当有数据要写（或正在连接）才请求 POLLOUT：
                // 已连接的空闲 socket 恒可写，若总是轮询 POLLOUT 会让 poll 永不阻塞、单核跑满
                short ev = POLLIN;
                if (a->http.conn().wants_write()) ev |= POLLOUT;
                struct pollfd p = {a->http.conn().fd, ev, 0};
                fds.push_back(p);
            }
            if (a->ws.conn().fd >= 0) {
                short ev = POLLIN;
                if (a->ws.conn().wants_write()) ev |= POLLOUT;
                struct pollfd p = {a->ws.conn().fd, ev, 0};
                fds.push_back(p);
            }
        }

        int rc = poll(fds.data(), static_cast<nfds_t>(fds.size()), 16);
        int64_t now = now_ms();

        if (rc > 0 && (fds[0].revents & POLLIN)) {
            char buf[256];
            for (;;) {
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n <= 0) break;
                for (ssize_t i = 0; i < n; ++i) {
                    if (buf[i] == '\n') {
                        process_line(input_buf);
                        input_buf.clear();
                    } else {
                        input_buf += buf[i];
                    }
                }
            }
        }

        for (auto& a : accounts) a->tick(now);
        for (auto& a : accounts) a->drain_trig_main();   // 排空 worker 回传的 send/log 闭包
        for (auto& a : accounts) {
            if (a->http.conn().fd >= 0) a->http.conn().flush();
            if (a->ws.conn().fd >= 0) a->ws.conn().flush();
        }
    }
}

// ---------- 终端能力探测 ----------

// 写入 DSR 光标位置查询 \x1b[6n：支持 ANSI 的终端会回 \x1b[r;cR
bool probe_ansi() {
    struct termios oldt{}, newt{};
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    std::printf("\x1b[6n");
    std::fflush(stdout);
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    bool ok = false;
    if (poll(&p, 1, 300) > 0) {
        char buf[32];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        ok = (n > 0 && buf[0] == 0x1b);  // 收到 CSI 响应
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ok;
}

bool detect_tui() {
    // 环境变量可强制：WSMUD_TUI=0 强制经典模式，WSMUD_TUI=1 强制全屏
    const char* force = std::getenv("WSMUD_TUI");
    if (force && force[0] != '\0') {
        return std::strcmp(force, "0") != 0 && std::strcmp(force, "no") != 0 &&
               std::strcmp(force, "false") != 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    const char* term = std::getenv("TERM");
    if (!term || term[0] == '\0') return false;
    if (std::strcmp(term, "dumb") == 0) return false;
    return probe_ansi();
}

}  // namespace

int main() {
    g_tui = detect_tui();
    if (g_tui) {
        // 全屏 TUI：初始 5 个标签页，可 F7 追加 / DEL 删除
        tui::raw_mode(true);
        accounts.resize(static_cast<std::size_t>(slots));
        acc_logs.resize(static_cast<std::size_t>(slots));
        acc_chat_logs.resize(static_cast<std::size_t>(slots));
        acc_pkt_logs.resize(static_cast<std::size_t>(slots));
        for (std::size_t i = 0; i < accounts.size(); ++i) {
            accounts[i] = std::make_unique<Account>();
            accounts[i]->index = static_cast<int>(i) + 1;
            accounts[i]->set_log(account_log);
            accounts[i]->set_chat(account_chat);
            accounts[i]->set_packet(account_packet);
        }
        reload_triggers();  // 启动即加载 trigger.json（不存在则创建空模板）
        refresh_input_state();  // 初次进入：选中 F1 未配置 → 立即提示录入账号
        tui_run_loop();
        tui::raw_mode(false);
        std::printf("\x1b[?1049l\x1b[2J\x1b[H\x1b[0m正在退出...\n");
    } else {
        // 经典行式模式
        startup_prompt();
        classic_run_loop();
        std::printf("正在退出...\n");
    }

    for (auto& a : accounts) a->disconnect();
    WorkerPool::instance().shutdown();   // 停止并 join 共享触发器 worker 线程池
    return 0;
}
