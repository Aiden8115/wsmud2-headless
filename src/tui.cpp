// TUI 实现：ANSI 转义整屏渲染
#include "tui.hpp"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wsmud {
namespace tui {

namespace {

// 重置所有属性
inline const std::string RST = "\x1b[0m";
// 隐藏/显示光标
inline const std::string HIDE = "\x1b[?25l";
inline const std::string SHOW = "\x1b[?25h";
// 绿色边框竖线（整个界面边框统一绿色）
inline const std::string BORDER = "\x1b[32m|\x1b[0m";

// 取 UTF-8 编码单元长度（1-4）
std::size_t seq_len(unsigned char c) {
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 解码 UTF-8 码点
std::uint32_t decode_cp(const char* s, std::size_t n) {
    unsigned char c = static_cast<unsigned char>(s[0]);
    if (n == 1) return c;
    std::uint32_t cp = c & (0xFFu >> (n + 1));
    for (std::size_t i = 1; i < n; ++i) {
        cp <<= 6;
        cp |= static_cast<unsigned char>(s[i]) & 0x3Fu;
    }
    return cp;
}

// 该码点是否占 2 个显示列（CJK 宽字符）
bool is_wide(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||
           cp == 0x2329 || cp == 0x232A ||
           (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) ||
           (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6);
}

}  // namespace

Term detect() {
    Term t;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        t.rows = static_cast<int>(ws.ws_row);
        t.cols = static_cast<int>(ws.ws_col);
    }
    return t;
}

bool raw_mode(bool on) {
    static struct termios oldt;
    static bool saved = false;
    if (on) {
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &t) != 0) return false;
        if (!saved) { oldt = t; saved = true; }
        t.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
        t.c_iflag &= ~(IXON | ICRNL);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        // 启用鼠标报告：1000=按下/释放，1006=SGR 坐标（\x1b[<b;x;yM）
        std::printf("\x1b[?1000h\x1b[?1006h");
        std::fflush(stdout);
    } else {
        if (saved) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::printf("\x1b[?1000l\x1b[?1006l");
        std::fflush(stdout);
    }
    return true;
}

std::string fit(const std::string& s, std::size_t width, bool pad_left) {
    std::string out;
    std::size_t w = 0;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t n = seq_len(static_cast<unsigned char>(s[i]));
        if (i + n > s.size()) break;
        std::uint32_t cp = decode_cp(s.data() + i, n);
        std::size_t cw = is_wide(cp) ? 2 : 1;
        if (w + cw > width) break;  // 放不下，截断（不切字符）
        out.append(s, i, n);
        w += cw;
        i += n;
    }
    std::size_t pad = width > w ? width - w : 0;
    if (pad_left) {
        return std::string(pad, ' ') + out;
    }
    out.append(pad, ' ');
    return out;
}

std::string frame(const Frame& f) {
    int cols = f.cols;
    if (cols < 20) cols = 20;
    int rows = f.rows;
    if (rows < 6) rows = 6;

    std::string s;
    s.reserve(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols) + 128);
    // 不每帧清屏（\x1b[2J 会造成闪屏），只用 \x1b[H 归位 + 每行 \x1b[K 覆盖
    s += std::string("\x1b[H") + RST + HIDE;

    // ---- 顶栏：5 个账号槽位块 ----
    std::size_t nslots = f.slots.empty() ? 5 : f.slots.size();
    // 块宽 = 总宽均分（不留块间空格），保证整行绝不超出 cols，
    // 避免终端自动换行把顶栏/蓝条挤出屏幕（修复窄窗口下顶部缺失）
    int bw = cols / static_cast<int>(nslots);
    if (bw < 4) bw = 4;
    for (std::size_t i = 0; i < nslots; ++i) {
        const SlotBar& sb = f.slots[i];
        // 前 5 个标签页有 F 键直接切换，序号 6 起的标签页无 F 键（用 ←/→ 移动）
        std::string txt;
        if (i < 5) txt = "F" + std::to_string(i + 1);
        if (!sb.label.empty()) txt += (txt.empty() ? sb.label : " " + sb.label);
        txt = fit(txt, static_cast<std::size_t>(bw - 2), false);  // 留出两侧边距
        // 背景：已登录且选中=绿底黑字（黑字清晰不灰）；其余选中=反显；未选中=黑底白字
        if (sb.state == 2 && sb.selected) {
            s += "\x1b[42;30m";
        } else if (sb.selected) {
            s += "\x1b[7m";
        } else {
            s += "\x1b[40;37m";
        }
        s += "[" + txt + "]" + RST;
    }
    // 行尾补空格到整行（多余列留空，不触发自动换行）
    int used = static_cast<int>(nslots) * bw;
    s += std::string(static_cast<std::size_t>(cols > used ? cols - used : 0), ' ');
    s += "\x1b[K\n";

    // ---- 第二行：F6 / F7 / F8 / DEL / F10（蓝色背景） ----
    s += "\x1b[44;37m";
    std::string bar = f.game_mode ? "F6 切换发送游戏命令/程序命令" : "F6 切换发送程序命令/游戏命令";
    bar += "    F7 新增标签    F8 触发器    DEL 删除标签    F10 退出";
    s += fit(bar, static_cast<std::size_t>(cols), false);  // 超出 cols 时截断，绝不回绕
    s += std::string("\x1b[K") + RST + "\n";

    // ---- 分隔线（绿色等号，边框统一绿色） ----
    s += "\x1b[32m" + std::string(static_cast<std::size_t>(cols), '=') + RST + "\n";

    // ---- 输出区（边框 |，显示最近日志） ----
    // 整帧只画 rows-2 行，留出最后 1 行作安全余量：
    // 某些终端（Windows Terminal/WSL）报告的行数比实际可视行数多 1 且会波动 ±1 行。
    // 若把提示行写到可视区最后一行，终端会滚动，把顶栏槽位挤出屏幕（上下滚动时偶现消失）；
    // 不写最后一行就不会滚动。提示行下方可能残留旧帧内容（如切槽位/quit 前的提示行），
    // 由每帧末尾的 \x1b[J（清除光标以下）统一清掉。
    int log_h = rows - 7;
    if (log_h < 0) log_h = 0;
    if (f.list_view) {
        // ---- F8 触发器列表视图：整区显示列表 ----
        std::size_t vis = log_h > 1 ? static_cast<std::size_t>(log_h) - 1 : 0;  // 第 1 行为标题
        std::size_t avail = f.list_lines.size() > vis ? f.list_lines.size() - vis : 0;
        int off = f.scroll_offset < 0 ? 0 : f.scroll_offset;
        if (static_cast<std::size_t>(off) > avail) off = static_cast<int>(avail);
        for (int i = 0; i < log_h; ++i) {
            s += BORDER;
            if (i == 0) {
                s += "\x1b[36m" + fit(f.list_title, static_cast<std::size_t>(cols - 2), false) + RST;
            } else {
                std::size_t li = static_cast<std::size_t>(i - 1) + static_cast<std::size_t>(off);
                if (li < f.list_lines.size()) {
                    if (f.list_cursor >= 0 && li == static_cast<std::size_t>(f.list_cursor))
                        s += "\x1b[7m" + fit(f.list_lines[li], static_cast<std::size_t>(cols - 2), false) + RST;
                    else
                        s += fit(f.list_lines[li], static_cast<std::size_t>(cols - 2), false);
                } else
                    s += std::string(static_cast<std::size_t>(cols - 2), ' ');
            }
            s += BORDER + "\x1b[K\n";
        }
    } else {
        // ---- 聊天/文本分区：聊天区上 1/3，文本区下 2/3 ----
        int chat_h = log_h / 3;
        if (chat_h < 1 && log_h >= 2) chat_h = 1;   // 至少 1 行聊天区
        int text_h = log_h - chat_h;
        // 聊天区滚动用 chat_scroll_offset（Shift+↑↓），文本区滚动用 scroll_offset（↑↓）
        std::size_t c_avail = f.chats.size() > static_cast<std::size_t>(chat_h)
                              ? f.chats.size() - static_cast<std::size_t>(chat_h) : 0;
        bool sep = chat_h > 0 && text_h > 0;   // 分隔行占用 1 行文本区
        std::size_t tvis = static_cast<std::size_t>(text_h) - (sep ? 1u : 0u);
        std::size_t t_avail = f.logs.size() > tvis
                              ? f.logs.size() - tvis : 0;
        int coff = f.chat_scroll_offset < 0 ? 0 : f.chat_scroll_offset;
        if (static_cast<std::size_t>(coff) > c_avail) coff = static_cast<int>(c_avail);
        std::size_t c_start = f.chats.size() >= static_cast<std::size_t>(chat_h) + static_cast<std::size_t>(coff)
                              ? f.chats.size() - static_cast<std::size_t>(chat_h) - static_cast<std::size_t>(coff) : 0;
        int off2 = f.scroll_offset < 0 ? 0 : f.scroll_offset;
        if (static_cast<std::size_t>(off2) > t_avail) off2 = static_cast<int>(t_avail);
        std::size_t t_start = f.logs.size() > tvis + static_cast<std::size_t>(off2)
                              ? f.logs.size() - tvis - static_cast<std::size_t>(off2) : 0;
        int i = 0;
        for (; i < chat_h; ++i) {
            s += BORDER;
            if (c_start + static_cast<std::size_t>(i) < f.chats.size())
                s += "\x1b[33m" + fit(f.chats[c_start + static_cast<std::size_t>(i)],
                                      static_cast<std::size_t>(cols - 2), false) + RST;  // 聊天区黄色
            else
                s += std::string(static_cast<std::size_t>(cols - 2), ' ');
            s += BORDER + "\x1b[K\n";
        }
        // 聊天/文本分区之间用一排绿色等号分隔
        if (sep) {
            s += "\x1b[32m|" + fit(std::string(static_cast<std::size_t>(cols - 2), '='),
                                   static_cast<std::size_t>(cols - 2), false) + "|\x1b[0m\x1b[K\n";
            ++i;
        }
        for (; i < log_h; ++i) {
            s += BORDER;
            std::size_t li = static_cast<std::size_t>(i) - static_cast<std::size_t>(chat_h) - (sep ? 1u : 0u) + t_start;
            if (li < f.logs.size())
                s += fit(f.logs[li], static_cast<std::size_t>(cols - 2), false);
            else
                s += std::string(static_cast<std::size_t>(cols - 2), ' ');
            s += BORDER + "\x1b[K\n";
        }
    }

    // ---- 底部分隔线（绿色等号） ----
    s += "\x1b[32m" + std::string(static_cast<std::size_t>(cols), '=') + RST + "\n";

    // ---- 命令提示行 ----
    s += RST;
    std::string prompt = f.cmd_prompt + "> ";
    std::string tail = f.mask_input ? std::string(f.cmd_text.size(), '*') : f.cmd_text;
    s += fit(prompt + tail, static_cast<std::size_t>(cols - 1), false);
    // \x1b[K 清当前行剩余列，\x1b[J 清光标以下所有行（抹掉历史帧残留的假提示行）
    s += std::string("\x1b[K\x1b[J") + SHOW;
    return s;
}

}  // namespace tui
}  // namespace wsmud
