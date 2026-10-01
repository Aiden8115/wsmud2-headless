// tui_ui.cpp — 全屏 TUI：输入状态机、键盘解析（原始模式）、整屏渲染、运行循环
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>   // printf/fwrite/fflush
#include <string>
#include <vector>

#include "app.hpp"

namespace {

// ---------- 前置声明（定义在下方，供 handle_byte 提前调用） ----------
void leader_key(unsigned char c);   // 触发器列表视图快捷键（a新增 / d删除 / 空格开关 / e编辑）

// ---------- 键盘输入解析（原始模式） ----------

struct EscState {
    int st = 0;          // 0=普通 1=ESC 2=ESC[ 3=ESCO
    char buf[4] = {0};   // 数字序列缓存（F5-F12）
    int len = 0;
};

// 匿名命名空间的辅助函数，凡在定义前被调用的都须先在此声明（EscState 已定义，可被引用）
// 注：open_list/close_list/handle_click 等已由 app.hpp 全局声明，无需（也不可）在此重复声明。
void leader_edit();
void close_editor();
bool handle_editor_byte_ns(unsigned char c, EscState& es);   // 触发器编辑屏按键（Esc 放弃 / s 保存）
bool handle_source_byte_ns(unsigned char c, EscState& es);   // source 多行编辑屏按键（Enter 换行 · Esc 保存返回）
void source_open();      // focus 到 source 行按 Enter → 打开全屏多行编辑器
void source_save_return();  // 保存 source 缓冲并退回表单

// ---------- 触发器分享码导入状态（F8 列表按 i 输入分享码 → 下载 → 预览 y/n 确认） ----------
void open_import_input();              // 进入行内分享码输入
void import_begin();                   // 行内输入回车 → 发起下载
void import_confirm_key(unsigned char c);  // 预览确认屏 y=导入/信任执行 n=跳过
void close_import();                   // 复位导入状态
bool import_has_exec(const std::string& s);  // 源码是否含 @js/#js 可执行脚本

bool imp_editing = false;    // 正在行内输入分享码
std::string imp_buf;         // 行内分享码缓冲
bool imp_confirm = false;    // 预览确认中（下载完成的触发器待 y/n）
wsmud::trigger::Trigger imp_cand;  // 待确认触发器
int imp_stage = 0;           // 0=导入确认  1=含可执行脚本时的二次信任确认
bool imp_js_extra = false;   // 源码含 @js/#js → 需二次 y/n

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
    // 编辑屏激活时，除鼠标外的所有字节都交给编辑屏状态机（Esc 放弃 / s 保存 / 编辑字段）
    if (trig_editor.active) {
        if (trig_editor.src_active) return handle_source_byte_ns(c, es);   // source 多行编辑
        return handle_editor_byte_ns(c, es);
    }
    switch (es.st) {
        case 0:
            if (c == 0x1b) { es.st = 1; return false; }
            if (c == 0x03) { quitting = true; return true; }   // Ctrl+C
            // ---------- 触发器分享码导入：预览确认 / 行内分享码输入 ----------
            if (view == View::TrigList && imp_confirm) {
                if (c == 'y' || c == 'Y' || c == 'n' || c == 'N') { import_confirm_key(c); return false; }
                return false;
            }
            if (view == View::TrigList && imp_editing) {
                if (c == '\r' || c == '\n') { import_begin(); return false; }
                if (c == 0x7f || c == 0x08) { backspace_utf8(imp_buf); return false; }
                if (c >= 0x20) imp_buf += static_cast<char>(c);
                return false;
            }
            // ---------- 触发器列表视图：快捷键不进命令框 ----------
            if (view == View::TrigList) {
                if (c == '\r' || c == '\n') { if (cmd_buf.empty()) leader_edit(); else submit(); return false; }
                if (c >= 0x20) { leader_key(c); return false; }
                return false;
            }
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
        case 5:  // 修饰键箭头序列 \x1b[1;2A：es.buf 存修饰键号（2=Shift，5=Ctrl）
            if (c >= '0' && c <= '9' && es.len < 4) { es.buf[es.len++] = static_cast<char>(c); return false; }
            if (c == 'A' || c == 'B') {
                int mod = es.len > 0 ? es.buf[es.len - 1] - '0' : 0;
                es.len = 0;
                es.st = 0;
                if (mod == 2) return chat_scroll(c == 'A');   // Shift+↑/↓ 只滚聊天区
                if (mod == 5) { pkt_scroll(c == 'A'); return false; }  // Ctrl+↑/↓ 滚动右侧网络包栏
                return c == 'A' ? dir_up() : dir_down();
            }
            if (c == 'C' || c == 'D') { es.len = 0; es.st = 0; select_slot(sel + (c == 'C' ? 1 : -1)); return false; }
            es.len = 0;
            es.st = 0;
            return false;
    }
    return false;
}

// ---------- 触发器编辑屏（F8 列表按 a/e/d/空格 进菜单编辑屏；Esc 放弃，s 保存） ----------

bool g_del_arm = false;   // 删除二次确认：仅当最近一次在列表按下 d 时置位

// ---- 字段行布局：0=名称 1=事件 2=启用 3..3+n-1=条件 最后=source ----
int editor_cond_n() { return static_cast<int>(trig_editor.fields.size()); }
int editor_rows() { return 4 + editor_cond_n(); }
int editor_cond_i(int row) { return (row >= 3 && row < 3 + editor_cond_n()) ? row - 3 : -1; }

std::string join_options(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) s += "|"; s += v[i]; }
    return s;
}

void editor_set_cond(int i, const std::string& val) {
    trig_editor.conds[trig_editor.fields[static_cast<std::size_t>(i)].key] = val;
}
std::string editor_cond_val(int i) {
    auto it = trig_editor.conds.find(trig_editor.fields[static_cast<std::size_t>(i)].key);
    return it == trig_editor.conds.end() ? std::string() : it->second;
}

// 渲染一行（frame 会按列宽 fit；正在编辑的行把键名染成绿色突出）
std::string editor_line_text(int row) {
    using wsmud::trigger::Engine;
    auto& e = trig_editor;
    bool ed = e.editing && e.focus == row;   // 该行处于编辑中 → 键名标绿
    std::string G = ed ? "\x1b[32m" : "";
    std::string R = ed ? "\x1b[0m" : "";
    if (row == 0) {
        std::string v = (e.editing && e.focus == 0) ? e.buf : e.name;
        return G + "名称: " + R + v;
    }
    if (row == 1) {
        return "事件: " + std::string(Engine::event_label(e.event)) + "  <" + e.event + ">（Enter 切换）";
    }
    if (row == 2) {
        return "启用: " + std::string(e.active_flag ? "启用" : "停用") + "（Enter 切换）";
    }
    int i = editor_cond_i(row);
    if (i >= 0) {
        const auto& f = e.fields[static_cast<std::size_t>(i)];
        std::string cur = editor_cond_val(i);
        std::string shown = cur;
        if (f.type == "enum") shown = cur.empty() ? "（通配，可选 " + join_options(f.options) + "）" : cur;
        if (e.editing && e.focus == row) shown = e.buf;
        return G + f.key + R + "「" + f.label + "」: " + shown;
    }
    std::string s = e.source;
    if (e.editing && e.focus == row) s = e.buf;
    std::size_t nl = s.find('\n');
    if (nl != std::string::npos) { s = s.substr(0, nl); s += " …"; }
    return G + "脚本 source: " + R + (s.empty() ? "（空）" : s);
}

void editor_move(int d) {
    auto& e = trig_editor;
    if (e.editing) return;   // 正在输入时不移动焦点
    int mx = editor_rows() - 1;
    e.focus += d;
    if (e.focus < 0) e.focus = 0;
    if (e.focus > mx) e.focus = mx;
}

// 事件切换（左右箭头或 事件行 Enter）：循环事件列表并刷新条件字段
void editor_switch_event(int d) {
    using wsmud::trigger::Engine;
    auto& e = trig_editor;
    auto names = Engine::event_names();
    int n = static_cast<int>(names.size());
    if (n == 0) return;
    int idx = 0;
    for (int i = 0; i < n; ++i) if (names[static_cast<std::size_t>(i)] == e.event) { idx = i; break; }
    idx = (idx + d + n) % n;
    e.event = names[static_cast<std::size_t>(idx)];
    e.fields = Engine::cond_fields(e.event);   // 保留 conds 中仍存在键的值
}

// 枚举字段循环（含通配态）
void editor_cycle(int i) {
    const auto& opts = trig_editor.fields[static_cast<std::size_t>(i)].options;
    std::string cur = editor_cond_val(i);
    if (cur.empty() && !opts.empty()) { editor_set_cond(i, opts[0]); return; }
    for (std::size_t k = 0; k < opts.size(); ++k) {
        if (opts[k] == cur) {
            if (k + 1 < opts.size()) editor_set_cond(i, opts[k + 1]);
            else editor_set_cond(i, "");   // 回到通配
            return;
        }
    }
    if (!opts.empty()) editor_set_cond(i, opts[0]);
}

void editor_commit_text(int row) {
    auto& e = trig_editor;
    if (row == 0) { e.name = e.buf; return; }
    int i = editor_cond_i(row);
    if (i >= 0) { editor_set_cond(i, e.buf); return; }
    e.source = e.buf;   // 末行为 source
}

void editor_enter() {
    auto& e = trig_editor;
    if (e.editing) { editor_commit_text(e.focus); e.editing = false; e.buf.clear(); return; }
    int n = editor_cond_n();
    if (e.focus == 0) { e.editing = true; e.buf = e.name; return; }
    if (e.focus == 1) { editor_switch_event(1); return; }
    if (e.focus == 2) { e.active_flag = !e.active_flag; return; }
    int i = editor_cond_i(e.focus);
    if (i >= 0) {
        if (e.fields[static_cast<std::size_t>(i)].type == "enum") { editor_cycle(i); return; }
        e.editing = true; e.buf = editor_cond_val(i); return;
    }
    // 末行为 source：按 Enter 打开全屏多行编辑器（不再是行内单行编辑）
    source_open();
}

// 当前账号可见（可编辑）的私有触发器：仅 owner==该玩家名（全局共享触发器不进入某玩家的编辑列表）
std::vector<wsmud::trigger::Trigger> my_visible() {
    const auto& my = accounts[static_cast<std::size_t>(sel)]->my_name;
    std::vector<wsmud::trigger::Trigger> mine;
    for (const auto& t : accounts[static_cast<std::size_t>(sel)]->trig.list())
        if (t.owner == my) mine.push_back(t);
    return mine;
}

// 用"其它玩家 + 全局共享" + 当前玩家新私有列表，重建全局配置（g_trig_cfg）
void apply_mine(const std::vector<wsmud::trigger::Trigger>& mine) {
    const auto& my = accounts[static_cast<std::size_t>(sel)]->my_name;
    std::vector<wsmud::trigger::Trigger> next;
    for (const auto& t : g_trig_cfg)
        if (t.owner != my) next.push_back(t);
    for (const auto& t : mine) next.push_back(t);
    g_trig_cfg = std::move(next);
}

// 保存：借用临时引擎对当前玩家的私有列表做同 add/update 一致的校验，再重建全局并原子写回
void editor_save() {
    using wsmud::trigger::Engine;
    auto& e = trig_editor;
    std::vector<std::pair<std::string, std::string>> cv;
    for (auto& f : e.fields) {
        std::string v = editor_cond_val(static_cast<int>(&f - e.fields.data()));
        if (!v.empty()) cv.emplace_back(f.key, v);
    }
    auto mine = my_visible();
    Engine tmp;
    std::string err;
    tmp.replace(mine);
    if (e.target >= 0) {
        if (!tmp.update(static_cast<std::size_t>(e.target), e.name, e.event, cv, e.source, e.active_flag, err)) {
            out("[保存失败] " + err); return;
        }
    } else {
        if (!tmp.add(e.name, e.event, cv, e.source, e.active_flag, err)) {
            out("[保存失败] " + err); return;
        }
    }
    mine = tmp.list();
    if (e.target < 0 && !mine.empty())
        mine.back().owner = accounts[static_cast<std::size_t>(sel)]->my_name;   // 新建归属当前玩家
    apply_mine(mine);
    close_editor();
    persist_triggers();
}

// 返回 true 表示需要退出（仅 Ctrl+C）
bool handle_editor_byte_ns(unsigned char c, EscState& es) {
    auto& e = trig_editor;
    switch (es.st) {
        case 0:
            if (c == 0x03) { quitting = true; return true; }   // Ctrl+C 退出
            if (c == 0x1b) { es.st = 1; return false; }
            if (c == '\r' || c == '\n') { editor_enter(); return false; }
            if (c == 0x7f || c == 0x08) { if (e.editing) backspace_utf8(e.buf); return false; }
            if (e.editing) { if (c >= 0x20) e.buf += static_cast<char>(c); return false; }
            if (c == 's' || c == 'S') { editor_save(); return false; }
            return false;
        case 1:
            if (c == '[') { es.st = 2; return false; }
            es.st = 0;
            return false;
        case 2:
            if (c == 'A') { es.st = 0; editor_move(-1); return false; }
            if (c == 'B') { es.st = 0; editor_move(1); return false; }
            if (c == 'C') { es.st = 0; editor_switch_event(1); return false; }
            if (c == 'D') { es.st = 0; editor_switch_event(-1); return false; }
            es.st = 0;
            return false;
    }
    return false;
}

void open_editor(int target) {
    using wsmud::trigger::Engine;
    auto& e = trig_editor;
    e.target = target;
    auto mine = my_visible();
    if (target >= 0 && static_cast<std::size_t>(target) < mine.size()) {
        const auto& t = mine[static_cast<std::size_t>(target)];
        e.name = t.name; e.event = t.event; e.active_flag = t.active;
        e.conds = t.conditions; e.source = t.source;
    } else {
        e.name = "新触发器"; e.event = "hint"; e.active_flag = true;
        e.conds.clear(); e.conds["keyword"] = ""; e.source = "";   // 骨架不预设内容
    }
    e.fields = Engine::cond_fields(e.event);
    e.focus = 0; e.editing = false; e.buf.clear();
    e.active = true;
    g_del_arm = false;
}

void close_editor() {
    trig_editor.active = false;
    trig_editor.editing = false;
    trig_editor.buf.clear();
    trig_editor.src_active = false;
    trig_editor.src_lines.clear();
    view = View::TrigList;
    trig_view = true;
    list_cursor = 0;
    g_del_arm = false;
    refresh_input_state();
}

// ---- source 多行编辑（vim/nano 式全屏编辑；Enter 换行，Esc 保存并退出） ----

// 多字节 UTF-8 字符累积缓冲（逐字节输入时暂存，收齐一个整字再插入）
static std::string s_src_pend;
static int s_src_pend_len = 0;

void src_insert(const std::string& ch) {
    auto& e = trig_editor;
    if ((std::size_t)e.src_row >= e.src_lines.size()) e.src_lines.emplace_back();
    auto& ln = e.src_lines[(std::size_t)e.src_row];
    if ((std::size_t)e.src_col > ln.size()) e.src_col = static_cast<int>(ln.size());
    ln.insert((std::size_t)e.src_col, ch);
    e.src_col += static_cast<int>(ch.size());
}

void src_newline() {
    auto& e = trig_editor;
    if ((std::size_t)e.src_row >= e.src_lines.size()) e.src_lines.emplace_back();
    auto& ln = e.src_lines[(std::size_t)e.src_row];
    if ((std::size_t)e.src_col > ln.size()) e.src_col = static_cast<int>(ln.size());
    std::string tail = ln.substr((std::size_t)e.src_col);
    ln.resize((std::size_t)e.src_col);
    ++e.src_row;
    e.src_col = 0;
    e.src_lines.insert(e.src_lines.begin() + e.src_row, tail);
}

void src_backspace() {
    auto& e = trig_editor;
    if ((std::size_t)e.src_row >= e.src_lines.size()) e.src_lines.emplace_back();
    auto& ln = e.src_lines[(std::size_t)e.src_row];
    if (e.src_col > 0) {
        // 删除光标前一个整字（中文等多字节一次删完）
        int s = e.src_col - 1;
        while (s > 0 && (static_cast<unsigned char>(ln[(std::size_t)s]) & 0xC0) == 0x80) --s;
        ln.erase((std::size_t)s, (std::size_t)(e.src_col - s));
        e.src_col = s;
    } else if (e.src_row > 0) {
        // 行首退格：合并到上一行
        std::string prev = e.src_lines[(std::size_t)e.src_row - 1];
        int col = static_cast<int>(prev.size());
        e.src_lines[(std::size_t)e.src_row - 1] = prev + ln;
        e.src_lines.erase(e.src_lines.begin() + e.src_row);
        --e.src_row;
        e.src_col = col;
    }
}

void src_move(int dr, int dc) {
    auto& e = trig_editor;
    int n = static_cast<int>(e.src_lines.size());
    if (n <= 0) e.src_lines.emplace_back();
    n = static_cast<int>(e.src_lines.size());
    if (dr != 0) {
        int nr = e.src_row + dr;
        if (nr < 0) nr = 0;
        if (nr > n - 1) nr = n - 1;
        e.src_row = nr;
    }
    if (dc != 0) e.src_col += dc;
    auto& ln = e.src_lines[(std::size_t)e.src_row];
    if (e.src_col < 0) e.src_col = 0;
    if (e.src_col > static_cast<int>(ln.size())) e.src_col = static_cast<int>(ln.size());
}

void source_open() {
    auto& e = trig_editor;
    e.src_lines.clear();
    std::string cur;
    for (char ch : e.source) {
        if (ch == '\n') { e.src_lines.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    e.src_lines.push_back(cur);
    if (e.src_lines.empty()) e.src_lines.emplace_back();
    e.src_row = 0; e.src_col = 0; e.src_off = 0;
    e.editing = false; e.buf.clear();
    e.src_active = true;
}

void source_save_return() {
    auto& e = trig_editor;
    std::string out;
    for (std::size_t i = 0; i < e.src_lines.size(); ++i) {
        if (i) out += '\n';
        out += e.src_lines[i];
    }
    e.source = out;
    e.src_active = false;
    e.editing = false;
    e.buf.clear();
}

// 返回 true 表示需要退出（仅 Ctrl+C）
bool handle_source_byte_ns(unsigned char c, EscState& es) {
    switch (es.st) {
        case 0:
            if (c == 0x03) { quitting = true; return true; }   // Ctrl+C 退出
            if (c == 0x1b) { es.st = 1; return false; }        // 独立 Esc 由主循环消歧后走保存
            if (c == '\r' || c == '\n') { s_src_pend.clear(); s_src_pend_len = 0; src_newline(); return false; }
            if (c == 0x7f || c == 0x08) { s_src_pend.clear(); s_src_pend_len = 0; src_backspace(); return false; }
            if (c >= 0x20) {
                if (c < 0x80) {   // ASCII：立即插入
                    s_src_pend.clear(); s_src_pend_len = 0;
                    src_insert(std::string(1, static_cast<char>(c)));
                } else if ((c & 0xC0) != 0x80) {   // 多字节 UTF-8 前导字节
                    s_src_pend.clear();
                    s_src_pend += static_cast<char>(c);
                    s_src_pend_len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
                } else {   // 续字节：收齐一个整字后插入
                    s_src_pend += static_cast<char>(c);
                    if (static_cast<int>(s_src_pend.size()) == s_src_pend_len) {
                        src_insert(s_src_pend);
                        s_src_pend.clear(); s_src_pend_len = 0;
                    }
                }
                return false;
            }
            return false;
        case 1:
            if (c == '[') { es.st = 2; return false; }
            if (c == 'O') { es.st = 3; return false; }
            // 有后续字节但非箭头：视为单独 Esc 按下 → 保存并返回（该字节作消歧丢弃）
            es.st = 0;
            source_save_return();
            return false;
        case 2:
            es.st = 0;
            if (c == 'A') { src_move(-1, 0); return false; }   // ↑
            if (c == 'B') { src_move(1, 0); return false; }    // ↓
            if (c == 'C') { src_move(0, 1); return false; }    // →
            if (c == 'D') { src_move(0, -1); return false; }   // ←
            return false;
        case 3:
        case 4:
        case 5:
            es.st = 0;
            return false;
    }
    return false;
}

// ---- 列表视图操作（作用于当前玩家私有列表，改后重建全局并写盘） ----
void leader_edit() {
    if (my_visible().empty()) { out("暂无触发器，按 a 新建"); return; }
    open_editor(list_cursor);
}
void leader_new() { open_editor(-1); }
void leader_toggle() {
    auto mine = my_visible();
    if (mine.empty()) return;
    if (list_cursor < 0 || list_cursor >= static_cast<int>(mine.size())) return;
    mine[static_cast<std::size_t>(list_cursor)].active =
        !mine[static_cast<std::size_t>(list_cursor)].active;
    apply_mine(mine);
    persist_triggers();
}
void leader_del() {
    auto mine = my_visible();
    if (mine.empty()) return;
    if (!g_del_arm) { g_del_arm = true; out("再按 d 确认删除该触发器，Esc 取消"); return; }
    g_del_arm = false;
    std::size_t idx = static_cast<std::size_t>(list_cursor);
    if (idx >= mine.size()) return;
    std::string n = mine[idx].name;
    mine.erase(mine.begin() + static_cast<std::ptrdiff_t>(idx));
    apply_mine(mine);
    std::size_t left = my_visible().size();
    if (list_cursor >= static_cast<int>(left)) list_cursor = left > 0 ? static_cast<int>(left) - 1 : 0;
    persist_triggers();
    out("已删除触发器：" + n);
}
// 列表视图快捷键：a=新增骨架 / e=编辑 / 空格 或 t=开关 active / d=删除（第一次置位确认，第二次删除）
void leader_key(unsigned char c) {
    if (c == 'd' || c == 'D') { leader_del(); return; }   // 首次调用置 g_del_arm，第二次真正删除
    if (g_del_arm) g_del_arm = false;                     // 其它键取消删除确认
    if (c == 'a' || c == 'A') { leader_new(); return; }
    if (c == ' ' || c == 't' || c == 'T') { leader_toggle(); return; }
    if (c == 'e' || c == 'E') { leader_edit(); return; }
    if (c == 'i' || c == 'I') { open_import_input(); return; }
}

// ---------- 触发器分享码导入（i → 行内输入 → 下载 → 预览 y/n 确认） ----------

bool import_has_exec(const std::string& s) {
    if (s.find("@js") != std::string::npos) return true;
    if (s.find("#js") != std::string::npos) return true;
    return false;
}

void open_import_input() {
    imp_editing = true;
    imp_buf.clear();
}
void close_import() {
    imp_editing = false;
    imp_confirm = false;
    imp_buf.clear();
    imp_stage = 0;
    imp_js_extra = false;
    imp_cand = wsmud::trigger::Trigger();
}

void import_begin() {
    int idx = accounts[static_cast<std::size_t>(sel)]->index;
    std::string code = trim(imp_buf);
    imp_buf.clear();
    imp_editing = false;
    if (code.empty()) { account_log(idx, "[导入] 分享码为空，已取消"); return; }
    accounts[static_cast<std::size_t>(sel)]->import_start(code);
}

void import_confirm_key(unsigned char c) {
    auto& a = *accounts[static_cast<std::size_t>(sel)];
    int idx = a.index;
    bool yes = (c == 'y' || c == 'Y');
    if (!yes) {
        account_log(idx, "[导入] 已跳过「" + imp_cand.name + "」，不导入");
        close_import();
        return;
    }
    // 含 @js/#js：第一次 y 只确认导入，二次 y 才信任执行
    if (imp_js_extra && imp_stage == 0) { imp_stage = 1; return; }
    if (import_accept_trigger(imp_cand)) {
        account_log(idx, "[导入] 已导入「" + imp_cand.name + "」并归属角色 " + a.my_name);
    }
    close_import();
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
        const Account& a = *accounts[static_cast<std::size_t>(i)];
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
    f.pkt_logs = acc_pkt_logs[static_cast<std::size_t>(sel)];  // 网络包栏（右侧独立一列）
    f.pkt_scroll_offset = pkt_scroll_offset;  // [上翻 / ]下翻
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
    // source 全屏编辑的光标屏幕坐标（1-based；仅 src_active 时有效）
    int src_scr_row = -1, src_scr_col = -1;
    if (view == View::TrigList) {
        f.list_view = true;
        if (trig_editor.active) {
            if (trig_editor.src_active) {
                // ---- source 多行编辑屏（vim/nano 式；Enter 换行 · Esc 保存返回） ----
                auto& L = trig_editor.src_lines;
                int cap = log_h - 1;   // 第 1 行为标题，余下为源行
                if (cap < 1) cap = 1;
                // 保持光标行可见（自动滚动）
                if (trig_editor.src_row < trig_editor.src_off) trig_editor.src_off = trig_editor.src_row;
                if (trig_editor.src_row >= trig_editor.src_off + cap)
                    trig_editor.src_off = trig_editor.src_row - cap + 1;
                f.list_title = "账号" + std::to_string(sel + 1) +
                    " 脚本编辑 source（Enter 换行 · ↑↓←→ 移动 · Esc 保存并返回）";
                f.list_lines.clear();
                int n = static_cast<int>(L.size());
                for (int k = trig_editor.src_off; k < n && static_cast<int>(f.list_lines.size()) < cap; ++k)
                    f.list_lines.push_back(L[static_cast<std::size_t>(k)]);
                while (static_cast<int>(f.list_lines.size()) < cap) f.list_lines.push_back("");   // 占满清残留
                // 光标放在源行对应屏幕坐标（内容从第 2 列开始，可视行 offset=5）
                int scr_row = 5 + trig_editor.src_row - trig_editor.src_off;
                if (scr_row >= 4 && scr_row <= 3 + log_h) src_scr_row = scr_row;
                std::string pre = (static_cast<std::size_t>(trig_editor.src_row) < L.size())
                    ? L[static_cast<std::size_t>(trig_editor.src_row)].substr(0,
                        static_cast<std::size_t>(trig_editor.src_col)) : "";
                src_scr_col = 2 + static_cast<int>(tui::display_width(pre));
            } else {
                // ---- 编辑屏：字段表单（0=名称 1=事件 2=启用 3..=条件 最后=source） ----
                // 编辑中：键名已标绿，不再叠加反色；仅导航态用反色高亮焦点行（+1 偏移提示行）
                f.list_cursor = trig_editor.editing ? -1 : (trig_editor.focus + 1);
                f.list_title = "账号" + std::to_string(sel + 1) +
                    " 编辑触发器（↑↓ 选择 · Enter 编辑/切换 · s 保存写回 trigger.json · Esc 放弃）";
                f.list_lines.clear();
                f.list_lines.push_back("s 保存 · Esc 放弃");
                for (int r = 0; r < editor_rows(); ++r)
                    f.list_lines.push_back(editor_line_text(r));
            }
        } else if (imp_editing) {
            f.list_cursor = -1;
            f.list_title = "账号" + std::to_string(sel + 1) +
                " 粘贴/输入触发器分享码（含·触发）→ Enter 下载 · Esc 取消";
            f.list_lines.clear();
            f.list_lines.push_back(fit("分享码> " + (imp_buf.empty() ? "（空）" : imp_buf),
                static_cast<std::size_t>(t.cols - 2), false));
            f.list_lines.push_back("");
            f.list_lines.push_back("说明：粘贴完整分享码后按 Enter 下载，将逐条预览并 y/n 确认后导入");
        } else if (imp_confirm) {
            f.list_cursor = -1;
            const auto& a = *accounts[static_cast<std::size_t>(sel)];
            const auto& cd = imp_cand;
            f.list_title = "账号" + std::to_string(sel + 1) + " 确认导入触发器（y 导入 · n 跳过）";
            f.list_lines.clear();
            f.list_lines.push_back("名称: " + cd.name);
            f.list_lines.push_back("事件: " + std::string(wsmud::trigger::Engine::event_label(cd.event)) +
                "  <" + cd.event + ">");
            f.list_lines.push_back("归属: " + (a.my_name.empty() ? "（未进入游戏）" : a.my_name));
            for (const auto& kv : cd.conditions)
                f.list_lines.push_back("条件  " + kv.first + " = " + (kv.second.empty() ? "（通配）" : kv.second));
            f.list_lines.push_back("脚本:");
            std::string src = cd.source;
            if (src.find('\n') != std::string::npos)
                src = src.substr(0, src.find('\n')) + " …";
            f.list_lines.push_back("  " + (src.empty() ? "（无脚本）" : src));
            if (imp_js_extra)
                f.list_lines.push_back("⚠ 脚本含 @js/#js 可执行代码，需二次确认信任执行");
            f.list_lines.push_back(imp_js_extra && imp_stage == 1
                ? "确认要执行脚本中的原生代码吗？ y 信任并导入 / n 取消"
                : "确认导入该触发器吗？ y 导入 / n 跳过");
        } else {
        // list_lines[0] 是提示行，触发器 #i 在 list_lines[i+1]；
        // 反色高亮必须 +1，否则落在选中触发器上方一行
        f.list_cursor = list_cursor + 1;
        f.list_title = "账号" + std::to_string(sel + 1) + " 触发器列表（F8 编辑 · 空格开关 · d 删除 · a 新增 · i 导入 · Esc 返回）";
        const auto& tl = accounts[static_cast<std::size_t>(sel)]->trig.list();
        f.list_lines.clear();
        f.list_lines.push_back("trigger list 查看 · reloadTrigger 重载");
        std::size_t n = tl.size();
        if (n == 0) {
            f.list_lines.push_back("暂无触发器（按 a 新增）");
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
    // source 全屏编辑：把可见光标放到对应文本位置（frame 末尾已 SHOW 光标，此处仅移位）
    if (trig_editor.src_active && src_scr_row >= 1 && src_scr_col >= 1)
        frame += "\x1b[" + std::to_string(src_scr_row) + ";" + std::to_string(src_scr_col) + "H";
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
    if (accounts[static_cast<std::size_t>(sel)]->account.empty()) {
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
    // 触发器界面仅对已登录槽位开放：切到未进入游戏的槽位时退出触发界面
    // （防止停留在无身份的空列表，或看到他人触发器）
    if (view == View::TrigList) {
        const Account& a = *accounts[static_cast<std::size_t>(sel)];
        if (a.stage != Account::Stage::Online) close_list();
    }
    refresh_input_state();
}

void submit() {
    std::string line = cmd_buf;
    cmd_buf.clear();
    line = trim(line);

    if (input_stage == InputStage::Account) {
        auto& a = *accounts[static_cast<std::size_t>(sel)];
        if (line.empty()) {
            out("账号不能为空");
            return;
        }
        a.account = line;
        input_stage = InputStage::Password;
        return;
    }
    if (input_stage == InputStage::Password) {
        auto& a = *accounts[static_cast<std::size_t>(sel)];
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
        if (a->need != Account::Need::None) {
            process_line(line);
            return;
        }
    }
    if (game_mode) {
        auto& a = *accounts[static_cast<std::size_t>(sel)];
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
    // 指针稳定存储：unique_ptr 使 Account 地址在 vector 重排时保持不变，
    // 触发器 worker 线程上捕获的 this 因此不会悬垂。
    accounts.push_back(std::make_unique<Account>());
    Account& a = *accounts.back();
    a.index = slots + 1;   // 新标签页序号（1-based）
    a.set_log(account_log);
    a.set_chat(account_chat);
    a.set_packet(account_packet);
    a.set_triggers_all(g_trig_cfg);  // 新账号继承当前 trigger.json 配置（按自身玩家过滤）
    acc_logs.emplace_back();
    acc_chat_logs.emplace_back();
    acc_pkt_logs.emplace_back();
    ++slots;
    select_slot(slots - 1);  // 自动移到新标签页
}

// DEL 删除当前标签页（仅允许删除序号大于 5 的标签页）
void del_tab() {
    if (sel < 5) {
        out("只能删除序号大于 5 的标签页（当前为 F" + std::to_string(sel + 1) + "）");
        return;
    }
    accounts[static_cast<std::size_t>(sel)]->disconnect();  // 先关闭该标签页的网络连接
    accounts.erase(accounts.begin() + static_cast<std::ptrdiff_t>(sel));
    acc_logs.erase(acc_logs.begin() + static_cast<std::ptrdiff_t>(sel));
    acc_chat_logs.erase(acc_chat_logs.begin() + static_cast<std::ptrdiff_t>(sel));
    acc_pkt_logs.erase(acc_pkt_logs.begin() + static_cast<std::ptrdiff_t>(sel));
    --slots;
    // 删除后索引前移，重新编号保持 account.index == 标签页序号。
    // unique_ptr 存储下元素仅移动指针，Address 不变，worker 线程闭包缓存的 this 依旧有效。
    for (std::size_t i = 0; i < accounts.size(); ++i)
        accounts[i]->index = static_cast<int>(i) + 1;
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
        std::size_t n = accounts[static_cast<std::size_t>(sel)]->trig.list().size();
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
        if (view == View::Logs) {
            // 触发器是玩家独有的：仅在已进入游戏（Online）时可见/本玩家自己的触发器，
            // 未登录（Not 进入游戏）的槽位阻止进入，避免在无玩家身份时空看或误改他人触发器。
            const Account& a = *accounts[static_cast<std::size_t>(sel)];
            if (a.stage != Account::Stage::Online) {
                out("触发器界面仅对已登录玩家开放：当前槽位未进入游戏，无法访问自己的触发器");
                return;
            }
            open_list();
        } else {
            close_list();
        }
        return;
    }
    if (fn == 10) { quitting = true; return; }
}

// ---------- 触发器界面：只读列表（配置由 trigger.json 维护） ----------

void open_list() { view = View::TrigList; trig_view = true; list_cursor = 0; cmd_buf.clear(); scroll_offset = 0; chat_scroll_offset = 0; refresh_input_state(); }
void close_list() { view = View::Logs; trig_view = false; cmd_buf.clear(); scroll_offset = 0; chat_scroll_offset = 0; g_del_arm = false; close_import(); refresh_input_state(); }

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
    // 渲染节流：触发引擎 tick 保持 16ms 的高频响应，但整帧重绘以不高于 20Hz 节流，
    // 避免无活动时也 60Hz 重建整帧（含网络包 JSON 折行）导致 CPU 反噬、拖慢主循环。
    int64_t last_render_ms = 0;
    constexpr int64_t RENDER_MIN_INTERVAL_MS = 50;

    std::vector<struct pollfd> fds;
    fds.reserve(1 + accounts.size() * 2);

    render();  // 首帧

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

        poll(fds.data(), static_cast<nfds_t>(fds.size()), 16);
        int64_t now = now_ms();
        bool need_render = false;   // 本帧是否有用户输入 → 强制立即重绘

        if (fds[0].revents & POLLIN) {
            need_render = true;   // 用户有输入 → 本帧立即重绘
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
            need_render = true;   // ESC 序列处理同样改变视图 → 强制重绘
            struct pollfd p = {STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, 25) > 0) {
                ssize_t n2 = read(STDIN_FILENO, kb, sizeof(kb));
                for (ssize_t i = 0; n2 > 0 && i < n2; ++i) {
                    if (handle_byte(static_cast<unsigned char>(kb[i]), es)) break;
                }
            }
            if (es.st == 1) {
                es.st = 0;
                if (trig_editor.active) {
                    if (trig_editor.src_active) source_save_return();   // source 编辑：Esc 保存并返回
                    else close_editor();
                } else if (view == View::TrigList) close_list();
            }
        }
        if (quitting) break;

        for (auto& a : accounts) a->tick(now);
        for (auto& a : accounts) a->drain_trig_main();   // 排空 worker 回传的 send/log 闭包
        // 触发器分享码导入推进（当前账号）：下载 → 完成后转入预览确认
        {
            auto& a = *accounts[static_cast<std::size_t>(sel)];
            if (a.import_state == Account::ImportState::Downloading) a.import_tick(now);
            if (view == View::TrigList && !imp_editing && !imp_confirm &&
                a.import_state == Account::ImportState::Done) {
                if (a.import_err.empty()) {   // mud.cpp 已就失败写过日志
                    wsmud::trigger::Trigger cand;
                    std::string err;
                    if (wsmud::trigger::Engine::parse_share(a.import_data, cand, err)) {
                        imp_cand = std::move(cand);
                        imp_confirm = true;
                        imp_stage = 0;
                        imp_js_extra = import_has_exec(imp_cand.source);
                    } else {
                        account_log(a.index, "[导入失败] " + err);
                    }
                }
                a.import_state = Account::ImportState::Idle;
            }
        }
        // 登录失败（如密码错误）：自动回退到该槽位的账号/密码录入阶段，避免卡死
        for (auto& a : accounts) {
            if (a->stage == Account::Stage::Disconnected &&
                !a->account.empty() &&
                a->last_error.rfind("登录失败", 0) == 0) {
                a->account.clear();
                a->password.clear();
                a->need = Account::Need::None;
                a->stage = Account::Stage::None;
                acc_logs[static_cast<std::size_t>(a->index - 1)].clear();  // 槽位重置时清空该标签页输出区
                acc_chat_logs[static_cast<std::size_t>(a->index - 1)].clear();  // 同步清空聊天区
                acc_pkt_logs[static_cast<std::size_t>(a->index - 1)].clear();   // 同步清空网络包栏
                account_log(a->index, "登录失败，该槽位已重置，请重新录入账号密码");
                if (sel == a->index - 1) {
                    input_stage = InputStage::Account;
                    cmd_buf.clear();
                }
            }
        }
        for (auto& a : accounts) {
            if (a->http.conn().fd >= 0) a->http.conn().flush();
            if (a->ws.conn().fd >= 0) a->ws.conn().flush();
        }
        // 渲染节流：有输入立即刷新；无输入则内容更新最迟 RENDER_MIN_INTERVAL_MS 刷新一次
        if (need_render || now - last_render_ms >= RENDER_MIN_INTERVAL_MS) {
            render();
            last_render_ms = now;
            need_render = false;
        }
    }
}
