// TUI：终端检测、原始模式、整屏 ANSI 渲染
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace wsmud {
namespace tui {

struct Term {
    int rows = 24;
    int cols = 80;
};

// 通过 ioctl(TIOCGWINSZ) 探测终端尺寸，失败时回退 80x24
Term detect();

// 进入/退出终端原始模式（关闭 ICANON/ECHO/ISIG/IEXTEN）
bool raw_mode(bool on);

// 账号槽位（顶栏一个 F 键块）
struct SlotBar {
    bool selected = false;
    int state = 0;        // 0=未配置 1=已配置未登录 2=已登录
    std::string label;    // 块内角色/账号文本
};

// 整屏内容描述
struct Frame {
    int rows = 24;
    int cols = 80;
    std::vector<SlotBar> slots;  // 顶栏 5 个槽位
    bool game_mode = false;      // F6：false=程序命令 true=游戏命令
    std::vector<std::string> logs;  // 输出区日志（文本区，自动取末尾可见行）
    std::vector<std::string> chats;  // 聊天区消息（输出区上 1/3，自动取末尾可见行）
    std::string cmd_prompt;      // "命令" / "游戏命令" / "账号n" / "密码n"
    std::string cmd_text;        // 当前输入（密码模式下会显示为 *）
    bool mask_input = false;     // 密码回显掩码
    int scroll_offset = 0;       // 日志区向上滚动行数（0=显示最新，由 ↑/↓ 调整）
    int chat_scroll_offset = 0;  // 聊天区向上滚动行数（0=显示最新，由 Shift+↑/↓ 调整）

    // F8 触发器列表视图（非空则覆盖输出区渲染列表）
    bool list_view = false;
    std::string list_title;          // 列表标题（如 "账号1 触发器列表"）
    std::vector<std::string> list_lines;  // 列表行（每行自动 fit 截断）
    int list_cursor = -1;            // 键盘高亮行（-1=不高亮；反显显示）
};

// 渲染整屏（清屏+各区域+borders+命令提示行），隐藏光标
std::string frame(const Frame& f);

// 按显示宽度截断 UTF-8 字符串（不切断字符），并填充空格到指定宽度
std::string fit(const std::string& s, std::size_t width, bool pad_left = false);

}  // namespace tui
}  // namespace wsmud
