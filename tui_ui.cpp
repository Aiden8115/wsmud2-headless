// tui_ui.cpp — 全屏 TUI：输入状态机、键盘解析（原始模式）、整屏渲染、运行循环
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>   // printf/fwrite/fflush
#include <string>
#include <vector>

#include "app.hpp"

namespace {

// ---------- 键盘输入解析（原始模式） ----------

struct EscState {
    int st = 0;          // 0=普通 1=ESC 2=ESC[ 3=ESCO
    char buf[4] = {0};   // 数字序列缓存（F5-F12）
    int len = 0;
};

int fn_from_digits(const char* b, int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) v = v * 10 + (b[i] - '0');
    return v;
}

// 返回 true 表示需要退出
bool handle_byte(unsigned char c, EscState& es) {
    if (es.st == 4) {  // SGR 鼠标序列（\x1b[<b;x;yM/m），收集参数
        static int mx = 0, my = 0, mbtn = 0, mst = 0;
        if (c >= '0' && c <= '9') {
            if (mst == 0) mbtn = mbtn * 10 + (c - '0');
            else if (mst == 1) mx = mx * 10 + (c - '0');
            else if (mst == 2) my = my * 10 + (c - '0');
        } else if (c == ';') { if (mst < 2) ++mst; }
        else if (c == 'M') {  // 按下：仅记录位置，不触发（动作绑定到松开）
            mst = 0; mbtn = 0; mx = 0; my = 0;
            es.st = 0;
            return false;
        } else if (c == 'm') {  // 松开：执行点击动作
            if (mst == 2) handle_click(mx, my);
            mst = 0; mbtn = 0; mx = 0; my = 0;
            es.st = 0;
            return false;
        } else { mst = 0; mbtn = 0; mx = 0; my = 0; es.st = 0; }
        return false;
    }
    switch (es.st) {
        case 0:
            if (c == 0x1b) { es.st = 1; return false; }
            if (c == 0x03) { quitting = true; return true; }   // Ctrl+C
            // ---------- 普通命令框 ----------
            if (c == '\r' || c == '\n') { submit(); return false; }
            if (c == 0x7f || c == 0x08) { if (!cmd_buf.empty()) backspace_utf8(cmd_buf); return false; }
            if (c >= 0x20) cmd_buf += static_cast<char>(c);
            return false;
        case 1:
            if (c == '[') { es.st = 2; return false; }
            if (c == 'O') { es.st = 3; return false; }
            es.st = 0;
            if (view == View::TrigList) close_list();   // 单独 Esc：关闭触发器列表
            return false;
        case 2:
            if (c == '<') {  // SGR 鼠标序列引导符
                es.st = 4;
                return false;
            }
            if (c == ';') { es.len = 0; es.st = 5; return false; }  // 修饰键序列 \x1b[1;2A
            {
                if (c == 'A') { es.st = 0; return dir_up(); }     // ↑
                if (c == 'B') { es.st = 0; return dir_down(); }   // ↓
                if (c == 'C') { es.st = 0; select_slot(sel + 1); return false; }
                if (c == 'D') { es.st = 0; select_slot(sel - 1); return false; }
            }
            if (c >= '0' && c <= '9' && es.len < 4) { es.buf[es.len++] = static_cast<char>(c); return false; }
            if (c == '~' && es.len > 0) {
                // DEL 键 = \x1b[3~
                if (es.len == 1 && es.buf[0] == '3') {
                    es.len = 0;
                    es.st = 0;
                    if (view == View::Logs) del_tab();
                    return quitting;
                }
                int fn = fn_from_digits(es.buf, es.len);
                es.len = 0;
                es.st = 0;
                // xterm/Windows Terminal 键码 → F 键号：15=F5, 17~21=F6~F10, 23/24=F11/F12
                if (fn >= 11 && fn <= 14) fn -= 10;          // 11~14 = F1~F4（部分终端）
                else if (fn == 15) fn = 5;
                else if (fn >= 17 && fn <= 21) fn -= 11;     // 17~21 = F6~F10
                else if (fn >= 23 && fn <= 24) fn -= 12;     // 23~24 = F11~F12
                else fn = 0;
                if (fn) handle_fn(fn);
                return quitting;
            }
            es.len = 0;
            es.st = 0;
            return false;
        case 3:
            if (c >= 'P' && c <= 'S') {  // F1-F4
                es.st = 0;
                handle_fn(c - 'P' + 1);
                return quitting;
            }
            es.st = 0;
            return false;
        case 5:  // 修饰键箭头序列 \x1b[1;2A：es.buf 存修饰键号（2=Shift）
            if (c >= '0' && c <= '9' && es.len < 4) { es.buf[es.len++] = static_cast<char>(c); return false; }
            if (c == 'A' || c == 'B') {
                int mod = es.len > 0 ? es.buf[es.len - 1] - '0' : 0;
                es.len = 0;
                es.st = 0;
                if (mod == 2) return chat_scroll(c == 'A');   // Shift+↑/↓ 只滚聊天区
                return c == 'A' ? dir_up() : dir_down();
            }
            if (c == 'C' || c == 'D') { es.len = 0; es.st = 0; select_slot(sel + (c == 'C' ? 1 : -1)); return false; }
            es.len = 0;
            es.st = 0;
            return false;
    }
    return false;
}

// ---------- 渲染（全屏模式） ----------

void render() {
    tui::Frame f;
    Term t = tui::detect();
    f.rows = t.rows;
    f.cols = t.cols;

    for (int i = 0; i < slots; ++i) {
        tui::SlotBar sb;
        sb.selected = (i == sel);
        const Account& a = accounts[static_cast<std::size_t>(i)];
        if (a.account.empty()) {
            sb.state = 0;
        } else if (a.stage == Account::Stage::Online) {
            sb.state = 2;
            sb.label = a.role.name;
        } else {
            sb.state = 1;
            sb.label = "角色" + std::to_string(i + 1);
        }
        f.slots.push_back(sb);
    }
    f.game_mode = game_mode;
    f.logs = acc_logs[static_cast<std::size_t>(sel)];  // 只显示当前选中账号的输出
    f.chats = acc_chat_logs[static_cast<std::size_t>(sel)];  // 聊天区（输出区上 1/3）
    // 点击区域：顶栏槽位（所有视图都可用）
    g_zones.clear();
    {
        int bw = t.cols / slots;
        if (bw < 4) bw = 4;
        for (int i = 0; i < slots; ++i)
            g_zones.push_back({i * bw + 1, 1, (i + 1) * bw, 1, 0, i});
    }
    int log_start = 4;                 // 输出区第 1 行（1-based，标题行）
    int log_h = t.rows - 7;
    if (log_h < 0) log_h = 0;
    f.list_cursor = -1;
    if (view == View::TrigList) {
        f.list_view = true;
        // list_lines[0] 是提示行，触发器 #i 在 list_lines[i+1]；
        // 反色高亮必须 +1，否则落在选中触发器上方一行
        f.list_cursor = list_cursor + 1;
        f.list_title = "账号" + std::to_string(sel + 1) + " 触发器列表（只读，编辑 trigger.json 后输入 reloadTrigger 重载，Esc 返回）";
        const auto& tl = accounts[static_cast<std::size_t>(sel)].trig.list();
        f.list_lines.clear();
        f.list_lines.push_back("trigger list 查看 · reloadTrigger 重载");
        std::size_t n = tl.size();
        if (n == 0) {
            f.list_lines.push_back("暂无触发器（trigger.json 为空或尚未配置）");
        } else {
            for (std::size_t i = 0; i < n; ++i)
                f.list_lines.push_back(trig_line_text(static_cast<int>(i) + 1,
                    tl[i].name, tl[i].active, tl[i].event, t.cols));
        }
        // 底部 [返回] 按钮
        int by = log_start + 2 + static_cast<int>(n) - scroll_offset;  // 提示行+触发器行之后
        f.list_lines.push_back(fit("[返回]", static_cast<std::size_t>(t.cols - 2), false));
        g_zones.push_back({2, by, t.cols - 1, by, 5, 0});
    }
    switch (input_stage) {
        case InputStage::Account: f.cmd_prompt = "账号" + std::to_string(sel + 1); break;
        case InputStage::Password: f.cmd_prompt = "密码" + std::to_string(sel + 1); break;
        default: f.cmd_prompt = game_mode ? "游戏命令" : "命令"; break;
    }
    f.cmd_text = cmd_buf;
    f.mask_input = (input_stage == InputStage::Password);
    f.scroll_offset = scroll_offset;
    f.chat_scroll_offset = chat_scroll_offset;

    static bool first = true;
    static std::string last;
    static int last_sel = -1, last_rows = -1, last_cols = -1;
    std::string frame = tui::frame(f);
    if (first) {  // 首次进入备用屏并清屏一次
        frame = "\x1b[?1049h\x1b[2J" + frame;
        first = false;
    } else if (sel != last_sel || f.rows != last_rows || f.cols != last_cols) {
        // 切换槽位或终端尺寸变化时整屏清空再重绘：
        // 帧只画 rows-1 行，若此前某帧把提示行画到了最后一行的残留位置
        // （终端报告行数波动/切标签），不清屏会在底部叠出旧的假输入提示行
        frame = "\x1b[2J" + frame;
    }
    last_sel = sel;
    last_rows = f.rows;
    last_cols = f.cols;
    // 内容无变化时不重绘（避免挂机时无意义刷新闪烁）
    if (frame == last) return;
    last = frame;
    fwrite(frame.data(), 1, frame.size(), stdout);
    fflush(stdout);
}

}  // namespace

// ---------- TUI：输入状态机 ----------

void refresh_input_state() {
    if (game_mode) { input_stage = InputStage::None; return; }
    if (accounts[static_cast<std::size_t>(sel)].account.empty()) {
        if (input_stage == InputStage::None) input_stage = InputStage::Account;
    } else if (input_stage == InputStage::Account || input_stage == InputStage::Password) {
        input_stage = InputStage::None;
    }
}

void select_slot(int n) {  // 0-based
    if (n < 0 || n >= slots) return;
    sel = n;
    scroll_offset = 0;  // 切换账号后从该账号最新日志开始看
    chat_scroll_offset = 0;
    refresh_input_state();
}

void submit() {
    std::string line = cmd_buf;
    cmd_buf.clear();
    line = trim(line);

    if (input_stage == InputStage::Account) {
        auto& a = accounts[static_cast<std::size_t>(sel)];
        if (line.empty()) {
            out("账号不能为空");
            return;
        }
        a.account = line;
        input_stage = InputStage::Password;
        return;
    }
    if (input_stage == InputStage::Password) {
        auto& a = accounts[static_cast<std::size_t>(sel)];
        if (line.empty()) {
            out("密码不能为空");
            input_stage = InputStage::Account;
            return;
        }
        a.password = line;
        a.start_login();
        input_stage = InputStage::None;
        refresh_input_state();  // 停留在当前槽位，不自动切换（用户手动 F1-F5 / 左右键切换）
        return;
    }

    // 命令模式：优先路由需要输入的账号（选服务器/角色）
    for (auto& a : accounts) {
        if (a.need != Account::Need::None) {
            process_line(line);
            return;
        }
    }
    if (game_mode) {
        auto& a = accounts[static_cast<std::size_t>(sel)];
        if (a.account.empty()) {
            out("槽位" + std::to_string(sel + 1) + " 未配置账号");
            return;
        }
        if (!a.send_command(line)) {
            out("账号" + std::to_string(a.index) + " 不在线（" + stage_name(a.stage) + "），无法发送游戏命令");
        }
        return;
    }
    process_line(line);
}

// F7 追加标签页（新增标签页无直接切换快捷键，用 ←/→ 移动）
void add_tab() {
    if (slots >= 20) { out("标签页已达上限（20）"); return; }
    // Account 含 move-only 的触发器引擎，用 emplace_back 就地构造
    accounts.emplace_back();
    Account& a = accounts.back();
    a.index = slots + 1;   // 新标签页序号（1-based）
    a.set_log(account_log);
    a.set_chat(account_chat);
    a.trig.replace(g_trig_cfg);  // 新账号继承当前 trigger.json 配置
    acc_logs.emplace_back();
    acc_chat_logs.emplace_back();
    ++slots;
    select_slot(slots - 1);  // 自动移到新标签页
}

// DEL 删除当前标签页（仅允许删除序号大于 5 的标签页）
void del_tab() {
    if (sel < 5) {
        out("只能删除序号大于 5 的标签页（当前为 F" + std::to_string(sel + 1) + "）");
        return;
    }
    accounts[static_cast<std::size_t>(sel)].disconnect();  // 先关闭该标签页的网络连接
    accounts.erase(accounts.begin() + static_cast<std::ptrdiff_t>(sel));
    acc_logs.erase(acc_logs.begin() + static_cast<std::ptrdiff_t>(sel));
    acc_chat_logs.erase(acc_chat_logs.begin() + static_cast<std::ptrdiff_t>(sel));
    --slots;
    // 删除后索引前移，重新编号保持 account.index == 标签页序号
    for (std::size_t i = 0; i < accounts.size(); ++i)
        accounts[i].index = static_cast<int>(i) + 1;
    if (sel >= slots) sel = slots - 1;
    scroll_offset = 0;
    chat_scroll_offset = 0;
    refresh_input_state();
    out("已删除标签页");
}

// 列表/日志视图的上下移动（↑↓ 键）
bool dir_up() {
    if (view == View::TrigList) {
        if (list_cursor > 0) --list_cursor;
        return false;
    }
    scroll_offset++;   // 普通日志区上滚
    return false;
}
bool dir_down() {
    if (view == View::TrigList) {
        std::size_t n = accounts[static_cast<std::size_t>(sel)].trig.list().size();
        if (n > 0 && static_cast<std::size_t>(list_cursor) < n - 1) ++list_cursor;
        return false;
    }
    if (scroll_offset > 0) --scroll_offset;   // 普通日志区下滚
    return false;
}

// Shift+↑↓：仅滚动聊天区（只在 Logs 视图生效，列表视图忽略）
bool chat_scroll(bool up) {
    if (view != View::Logs) return false;
    if (up) ++chat_scroll_offset;              // 聊天区上滚
    else if (chat_scroll_offset > 0) --chat_scroll_offset;  // 聊天区下滚
    return false;
}

void handle_fn(int fn) {  // fn: 1-12
    // 只有前 5 个标签页有直接 F 键切换；新增标签页用 ←/→ 移动
    if (fn >= 1 && fn <= 5) { select_slot(fn - 1); return; }
    if (fn == 6) {
        game_mode = !game_mode;
        refresh_input_state();
        return;
    }
    if (fn == 7) { add_tab(); return; }
    if (fn == 8) {  // F8 触发器列表：开关
        if (view == View::Logs) open_list(); else close_list();
        return;
    }
    if (fn == 10) { quitting = true; return; }
}

// ---------- 触发器界面：只读列表（配置由 trigger.json 维护） ----------

void open_list() { view = View::TrigList; trig_view = true; list_cursor = 0; cmd_buf.clear(); scroll_offset = 0; chat_scroll_offset = 0; refresh_input_state(); }
void close_list() { view = View::Logs; trig_view = false; cmd_buf.clear(); scroll_offset = 0; chat_scroll_offset = 0; refresh_input_state(); }

// 点击处理（鼠标）：顶栏切槽位 / 列表 [返回] 按钮
void handle_click(int x, int y) {
    for (const auto& z : g_zones) {
        if (x < z.x1 || x > z.x2 || y < z.y1 || y > z.y2) continue;
        if (z.action == 0 && z.arg < slots) { select_slot(z.arg); return; }
        if (z.action == 5) { close_list(); return; }
    }
}

// 构建只读触发器列表行："#编号 [启用|停用] 名称 ← 事件"，无 [编辑] 列
std::string trig_line_text(int num, const std::string& name, bool active, const std::string& ev, int cols) {
    std::string base = "#" + std::to_string(num) + " [" + std::string(active ? "启用" : "停用") +
                       "] " + name + " ← " + ev;
    return fit(base, static_cast<std::size_t>(cols - 2), false);
}

void tui_run_loop() {
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    EscState es;
    char kb[256];

    std::vector<struct pollfd> fds;
    fds.reserve(1 + accounts.size() * 2);

    render();  // 首帧

    while (!quitting) {
        fds.clear();
        struct pollfd in = {STDIN_FILENO, POLLIN, 0};
        fds.push_back(in);
        for (auto& a : accounts) {
            if (a.http.conn().fd >= 0) {
                // 仅当有数据要写（或正在连接）才请求 POLLOUT：
                // 已连接的空闲 socket 恒可写，若总是轮询 POLLOUT 会让 poll 永不阻塞、单核跑满
                short ev = POLLIN;
                if (a.http.conn().wants_write()) ev |= POLLOUT;
                struct pollfd p = {a.http.conn().fd, ev, 0};
                fds.push_back(p);
            }
            if (a.ws.conn().fd >= 0) {
                short ev = POLLIN;
                if (a.ws.conn().wants_write()) ev |= POLLOUT;
                struct pollfd p = {a.ws.conn().fd, ev, 0};
                fds.push_back(p);
            }
        }

        poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
        int64_t now = now_ms();

        if (fds[0].revents & POLLIN) {
            for (;;) {
                ssize_t n = read(STDIN_FILENO, kb, sizeof(kb));
                if (n <= 0) break;
                for (ssize_t i = 0; i < n; ++i) {
                    if (handle_byte(static_cast<unsigned char>(kb[i]), es)) break;
                }
                if (quitting) break;
            }
        }

        // ESC 歧义消解：单独 Esc 永不自行生效，必须等下一个字节进 case 1。
        // 此处 st==1 时短等 25ms；无后续字节 → 判定为单独 Esc，立即生效；
        // 有后续字节（如 \x1b[A 箭头、\x1b[1;2A）→ 按序列继续解析。
        if (es.st == 1 && !quitting) {
            struct pollfd p = {STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, 25) > 0) {
                ssize_t n2 = read(STDIN_FILENO, kb, sizeof(kb));
                for (ssize_t i = 0; n2 > 0 && i < n2; ++i) {
                    if (handle_byte(static_cast<unsigned char>(kb[i]), es)) break;
                }
            }
            if (es.st == 1) { es.st = 0; if (view == View::TrigList) close_list(); }
        }
        if (quitting) break;

        for (auto& a : accounts) a.tick(now);
        // 登录失败（如密码错误）：自动回退到该槽位的账号/密码录入阶段，避免卡死
        for (auto& a : accounts) {
            if (a.stage == Account::Stage::Disconnected &&
                !a.account.empty() &&
                a.last_error.rfind("登录失败", 0) == 0) {
                a.account.clear();
                a.password.clear();
                a.need = Account::Need::None;
                a.stage = Account::Stage::None;
                acc_logs[static_cast<std::size_t>(a.index - 1)].clear();  // 槽位重置时清空该标签页输出区
                acc_chat_logs[static_cast<std::size_t>(a.index - 1)].clear();  // 同步清空聊天区
                account_log(a.index, "登录失败，该槽位已重置，请重新录入账号密码");
                if (sel == a.index - 1) {
                    input_stage = InputStage::Account;
                    cmd_buf.clear();
                }
            }
        }
        for (auto& a : accounts) {
            if (a.http.conn().fd >= 0) a.http.conn().flush();
            if (a.ws.conn().fd >= 0) a.ws.conn().flush();
        }
        render();
    }
}
