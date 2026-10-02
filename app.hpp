// app.hpp — 主程序跨文件共享声明（拆分 main.cpp 后的统一入口）
// 全局状态一律定义于 main.cpp（此处 extern）；类型与跨文件函数在此声明，
// 供 commands.cpp / tui_ui.cpp / main.cpp 使用。文件内私有的小函数不进本头。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "./src/mud.hpp"
#include "./src/tui.hpp"
#include "./src/trigger.hpp"

using wsmud::mud::Account;
using wsmud::mud::now_ms;
using wsmud::tui::Term;
using wsmud::tui::fit;
using namespace wsmud;  // 使 wsmud::tui::* 可用

// ---------- 全局状态（定义于 main.cpp） ----------

extern int slots;                     // 顶栏标签页数量（初始 5，可 F7 追加 / DEL 删除）
extern std::vector<std::unique_ptr<Account>> accounts;
extern bool quitting;                 // true=退出程序（F10 / Ctrl+C）
extern int sel;                       // 当前选中槽位（0-based）
extern bool game_mode;                // F6：false=程序命令 true=游戏命令
extern std::vector<std::vector<std::string>> acc_logs;        // 每账号独立日志缓冲（文本区，全屏模式）
extern std::vector<std::vector<std::string>> acc_chat_logs;   // 每账号独立聊天缓冲（聊天区，全屏模式）
extern std::vector<std::vector<std::string>> acc_pkt_logs;    // 每账号独立网络包缓冲（右侧"网络包"栏，全屏模式）
extern std::string cmd_buf;           // 命令行当前输入
extern int scroll_offset;            // 日志区向上滚动行数（0=显示最新）
extern int chat_scroll_offset;       // 聊天区向上滚动行数（0=显示最新，Shift+↑/↓）
extern int pkt_scroll_offset;        // 网络包栏向上滚动行数（0=显示最新，[上翻 / ]下翻）
extern bool g_tui;                    // true=全屏 TUI，false=经典行式输出

// 输入阶段：None=命令  Account=录入账号  Password=录入密码
enum class InputStage { None, Account, Password };
extern InputStage input_stage;

// F8 触发器列表视图（覆盖输出区显示当前账号触发器，只读）
extern bool trig_view;
extern int list_cursor;               // 列表视图键盘高亮行（0=首条触发器）

enum class View { Logs, TrigList, Settings };
extern View view;

// 点击区域（render 每帧重建，鼠标点击时匹配）
struct ClickZone {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // 屏幕列/行（1-based，含端点）
    int action = 0;                        // 0=切槽位 5=关闭列表视图
    int arg = 0;
};
extern std::vector<ClickZone> g_zones;

// 触发器编辑屏状态（在内存配置上编辑，Esc 放弃，s 保存走原子写回）
struct TrigEditor {
    bool active = false;      // 是否处于编辑屏
    bool editing = false;     // 正在编辑某个文本字段的值
    std::string buf;          // 正编辑的文本缓冲
    int target = -1;          // >=0 编辑已有触发器的 idx；-1=新增
    int focus = 0;            // 焦点字段行（0=名称 1=事件 2=启用 3..=条件 最后=source）
    std::string name;
    std::string event = "hint";
    bool active_flag = true;
    std::map<std::string, std::string> conds;
    std::string source;
    std::vector<wsmud::trigger::Engine::CondField> fields;  // 当前 event 的条件字段缓存
    // source 多行编辑屏（focus 到 source 行按 Enter 打开；Enter 换行，Esc 保存并返回表单）
    bool src_active = false;
    std::vector<std::string> src_lines;   // 多行缓冲（退出时 join '\n' 写回 source）
    int src_row = 0;                      // 光标行
    int src_col = 0;                      // 光标列（字节偏移）
    int src_off = 0;                      // 首行滚动偏移
};
extern TrigEditor trig_editor;

// 当前生效的全局触发器配置（来自软件同级目录 trigger.json，程序只读不写）
extern std::vector<wsmud::trigger::Trigger> g_trig_cfg;

// F9 设置屏：焦点设置项（0=自动施法 1=自动喜宴）
extern int settings_cursor;

// ---------- 函数（按所在编译单元分组） ----------

// main.cpp：输出层
void out(const std::string& line);
void account_log(int index, const std::string& line);
void account_chat(int index, const std::string& line);
void account_packet(int index, const std::string& line);
void pkt_scroll(bool up);             // [ 上翻 / ] 下翻网络包栏
void backspace_utf8(std::string& s);  // UTF-8 整字退格删除（中文等 3 字节字符一次删完）

// commands.cpp：程序命令处理
const char* stage_name(Account::Stage s);
std::string trim(const std::string& s);
bool parse_int(const std::string& s, int& out);
void reload_triggers();   // 重载软件同级 trigger.json（失败保留当前配置）
void persist_triggers();  // 把内存配置原子写回 trigger.json（写前往返校验，失败保留内存配置）
void load_settings();     // 加载软件同级 settings.json（不存在则创建空模板）
void persist_settings();  // 把玩家设置表原子写回 settings.json
bool import_accept_trigger(const wsmud::trigger::Trigger& cand);  // 导入分享码触发器并归属当前角色
void process_line(const std::string& raw);

// tui_ui.cpp：全屏 TUI 状态机 + 渲染 + 运行循环
void refresh_input_state();
void select_slot(int n);  // 0-based
void submit();
void add_tab();
void del_tab();
bool dir_up();
bool dir_down();
bool chat_scroll(bool up);
void handle_fn(int fn);   // fn: 1-12
void open_list();
void close_list();
void open_settings();   // F9：打开设置屏（仅对已登录玩家开放）
void close_settings();  // 关闭设置屏，回到日志视图
void settings_move(int d);     // ↑↓：移动设置项焦点
void settings_toggle();        // Enter/空格：切换当前设置项并写回 settings.json
void handle_click(int x, int y);
std::string trig_line_text(int num, const std::string& name, bool active, const std::string& ev, int cols);
void tui_run_loop();
