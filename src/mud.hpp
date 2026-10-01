// 游戏协议层：账号状态机（登录、认证、进游戏、在线保活、消息分发）
#pragma once

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cstdint>
#include <ctime>

#include "json.hpp"
#include "net.hpp"
#include "ws.hpp"
#include "trigger.hpp"
#include "color.hpp"
#include "thsafe.hpp"
#include "thpool.hpp"

namespace wsmud {
namespace mud {

// 每秒的毫秒数（时间基准统一为毫秒）
inline int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct GameServer {
    int id = 0;
    std::string name;
    std::string ip;
    std::string port;
};

struct Role {
    std::string id;   // 保留服务器原始值（可能是字符串），用于 "login {id}"
    std::string name;
    std::string title;
    int level = 0;
};

// 日志输出回调（由 main 提供，统一带账号前缀）
using LogFn = void (*)(int index, const std::string& line);
// 聊天消息回调（与文本日志分流：聊天区/文本区分开显示）
using ChatFn = void (*)(int index, const std::string& line);
// 网络包回调（右侧"网络包"栏：显示未显示在聊天区/文本区的其余原始包）
using PacketFn = void (*)(int index, const std::string& line);

class Account {
public:
    Account() = default;
    // 确保 worker 线程在对象析构前退出并 join（否则 std::thread 析构触发 terminate）。
    // 对象存于 unique_ptr<Account>，不可拷贝/移动，故可安全停止。
    ~Account() { stop_trig_worker(); }

    // 生命周期阶段
    enum class Stage {
        None,            // 未启动
        WaitServers,     // 等待服务器列表 HTTP
        NeedSelectServer,// 等待用户选择服务器
        WaitLogin,       // 等待登录 HTTP
        AuthWs,          // 认证 WebSocket（连接+握手）
        WaitRoles,       // 已发 "{u} {p}"，等待角色列表
        NeedSelectRole,  // 等待用户选择角色
        EnterWs,         // 进游戏 WebSocket（重连+握手）
        Entering,        // 已发四段认证，等待 login
        Online,          // 在线挂机
        Disconnected,    // 断线，等待用户 reconnect
    };

    // 等待用户输入的类型
    enum class Need { None, Server, Role };

    // 触发器分享码导入状态（复用账号内独立 import_http）
    enum class ImportState { Idle, Downloading, Done };
    ImportState import_state = ImportState::Idle;
    std::string import_data;   // 下载完成的原始 JSON（Done 时有效）
    std::string import_err;    // 失败原因
    int64_t import_deadline_ms = 0;

    int index = 0;                 // 1-based 账号序号
    std::string account;           // 登录账号
    std::string password;          // 登录密码
    std::string website = "http://wsmud2.cn";

    Stage stage = Stage::None;
    Need need = Need::None;
    std::string prompt;            // 等待输入时打印的提示语
    std::string last_error;        // 最近错误（供 status 显示）

    std::vector<GameServer> servers;
    GameServer server;
    std::string session_key, session_token;
    std::vector<Role> roles;
    Role role;

    net::HttpReq http;       // 登录/服务器列表的异步 HTTP 请求
    net::HttpReq import_http; // 分享码下载的异步 HTTP 请求（独立，不与登录 http 争用）
    ws::WsClient ws;          // 游戏 WebSocket 连接

    int64_t stage_deadline_ms = 0; // 当前阶段超时
    int64_t last_ping_ms = 0;      // 上次发 ping 时间
    bool entered = false;          // 是否成功进入过游戏

    // ---------- 触发器引擎 + 角色状态（供触发器脚本使用） ----------
    trigger::Engine trig;          // 每账号一个 Raid 解释器
    std::string my_name;           // 角色名
    int my_level = 0;              // 当前等级
    std::string my_state_text = "发呆"; // 状态关键词（RoleState 中文词，如 疗伤/打坐/发呆）
    std::string my_room_name;      // 当前房间名
    std::string my_room_exits;     // 房间出口（已格式化，如 "南：青草坪；东：木板路"，颜色已转 ANSI）
    std::vector<std::pair<std::string, std::string>> my_room_people;  // 房间人物 (id, 显示名)，保持服务器顺序
    double my_hp = 0, my_max_hp = 0, my_mp = 0, my_max_mp = 0;
    bool my_living = true;         // 是否存活（die/relive）
    bool my_combat = false;        // 是否在战斗
    bool my_idle = false;          // 发呆且不在战斗
    int64_t my_idle_start_ms = 0;  // 本次发呆起点（连续秒数 idle_time 计算）
    std::set<std::string> my_status; // 生效中的 buff sid（busy/faint/rash → :free）

    // 启动登录：拉取服务器列表
    void start_login();
    // 选择服务器（main 交互调用）
    void select_server(int idx);
    // 选择角色（main 交互调用）
    void select_role(int idx);
    // 主循环驱动
    void tick(int64_t now);
    // 触发器分享码导入：发起下载 / 主循环驱动，下载完成后 import_state=Done
    void import_start(const std::string& token);
    void import_tick(int64_t now);
    // 发送游戏命令（仅 Online 有效）
    bool send_command(const std::string& cmd);
    // 手动重连（沿用内存中的账号密码与服务器/角色）
    void reconnect();
    // 主动断开
    void disconnect();

    // 触发器：选角色后初始化（加载触发器文件），并构造脚本状态 JSON
    void init_triggers();
    void update_idle(int64_t now);

    // 触发器按玩家隔离：注入全局配置并同步到本账号引擎。
    // 引擎只执行"该玩家私有(owner==my_name)"与"全局共享(owner 空)"的触发器；
    // 空串 owner 的全局触发器对所有玩家生效，但不进入某玩家的 F8 编辑列表。
    void set_triggers_all(const std::vector<trigger::Trigger>& all);
    void sync_triggers();

    void set_log(LogFn fn) { log_ = fn; }
    void set_chat(ChatFn fn) { chat_ = fn; }
    void set_packet(PacketFn fn) { packet_ = fn; }

    // ---------- 触发器 worker（共享线程池中的一条，独占 QuickJS 运行时） ----------
    // 线程总数由 WorkerPool 封顶（默认 min(硬件并发,8)），账号多时共享负载最轻的 worker，
    // 空闲线程阻塞不占 CPU。主线程把 trig.on_* / tick / replace 等经闭包队列推给该 worker
    // 执行，实现"单个账号的重脚本不冻结全局事件循环"的隔离；worker 产生的命令/日志
    // 经反向队列回主线程实际发包/显示。QuickJS 运行时始终在账号绑定的同一条 worker 上
    // 创建与释放（线程亲和）。
    void start_trig_worker();    // 绑定一条池 worker（幂等；须在设置好 trig 回调后调用）
    void stop_trig_worker();     // 提交 JS 释放闭包并等待其执行完，再归还 worker（幂等）
    void enqueue_trig(std::function<void()> fn);  // 主线程 → 绑定 worker
    void post_main(std::function<void()> fn);     // worker → 主线程（send/log）
    void drain_trig_main();      // 主循环每帧排空 worker→主 出队闭包
    std::string get_state_json();  // worker 读主线程缓存的状态快照（互斥锁保护）

private:
    LogFn log_ = nullptr;
    ChatFn chat_ = nullptr;
    PacketFn packet_ = nullptr;
    std::vector<trigger::Trigger> all_triggers_;   // 注入的全局触发器全集（未过滤），sync_triggers 用它按玩家过滤

    // 统一出口：把服务器下发的 HTML 颜色标记转成 ANSI，再交给显示回调。
    // 触发器用到的 on_text/on_chat 走原始串，不受此处影响。
    void log(const std::string& line) { if (log_) log_(index, html_to_ansi(line)); }
    void log_chat(const std::string& line) { if (chat_) chat_(index, html_to_ansi(line)); }
    // 网络包栏：显示"不显示在聊天区/文本区"的其余原始包。原始串原样给出（不做 HTML→ANSI，
    // 供右栏 JSON 美化/折行，避免颜色码干扰宽度计算）
    void log_pkt(const std::string& line) { if (packet_) packet_(index, line); }
    void set_stage(Stage s, int64_t now, int64_t timeout_ms);
    void enter_disconnected(const std::string& reason);
    void on_ws_open(int64_t now);
    void on_message(const std::string& text);
    void on_json(const json::Value& v, const std::string& raw);

    // 触发器脚本宿主回调（ud = Account*）
    static void trig_log_fn(void* ud, const std::string& line);
    static void trig_send_fn(void* ud, const std::string& cmd);
    static std::string trig_state_fn(void* ud);

    // 状态跟踪辅助：把服务器消息同步到 my_* 字段 + 转发给 trig 事件入口
    void trig_on_items(const json::Value& v);
    void trig_on_itemadd(const json::Value& v);
    void trig_on_sc(const json::Value& v);
    void trig_on_status(const json::Value& v);
    void trig_on_pm(const json::Value& v);
    void trig_on_events(const json::Value& v);
    void trig_on_pack(const json::Value& v);
    void trig_on_social(const json::Value& v);
    static std::string state_word(const std::string& raw);   // state 文本 → 状态关键词

    // 房间面板（右侧"房间"区）数据：由 items/itemadd/itemremove 包维护
    void room_set_people(const json::Value& arr);            // items：整表重建
    void room_add_person(const json::Value& v);              // itemadd：追加（按 id 去重）
    void room_remove_person(const std::string& id);          // itemremove：按 id 删除

    // ---------- worker 支撑 ----------
    int worker_ = -1;                                 // 绑定的池 worker 索引（-1=未绑定）
    ConcurrentQueue<std::function<void()>> main_q_;   // worker → 主线程（send/log 闭包）
    std::mutex st_jm_;                            // 保护 state_j_（worker 快照读取）
    std::string state_j_;                         // 主线程缓存的状态快照 JSON
    void publish_state();                         // 主线程：把 my_* 生成快照写入 state_j_（持 st_jm_）
};

}  // namespace mud
}  // namespace wsmud
