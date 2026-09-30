// 触发器引擎：数据模型 + 15 事件过滤器断言 + QuickJS 执行 Raid 脚本
// 事件/过滤器的语义与浏览器扩展 wsmud_Raid 一致（见 trigger-events-*.js / trigger-core-triggers.js）
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct JSRuntime;   // QuickJS 前置声明（定义见 src/quickjs/quickjs.h；须在全局作用域）

namespace wsmud {
namespace trigger {

// 单个触发器（与扩展本地存储 JSON 结构一致）
struct Trigger {
    std::string name;                            // 触发器名（同一玩家内唯一）
    std::string event;                           // 事件模板名（如 "新聊天信息"）
    std::map<std::string, std::string> conditions;  // 过滤条件：键=条件名，值=用户填写值
    std::string source;                          // Raid 脚本源码
    std::string owner;                           // 归属玩家名；空串=全局共享（所有玩家生效，不进入某玩家 F8 编辑列表）
    bool active = false;                         // 是否启用
    std::string author;                          // 分享作者（可选）
};

// 事件分发参数（键 → 值，值已转为字符串）
using Params = std::map<std::string, std::string>;

// 房间缓存条目（"气血内力改变/伤害已满"事件依赖：sc 消息按 id 查房间缓存）
struct RoomItemData {
    std::string id;
    std::string name;
    double hp = 0, mp = 0, max_hp = 0, max_mp = 0;
    bool have_max_hp = false, have_max_mp = false;  // 是否已知最大值（伤害事件要求 max_hp 已知）
};

// 触发器引擎（每账号一个）。持有 QuickJS 运行时与上下文，禁止拷贝、支持移动。
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&& o) noexcept;
    Engine& operator=(Engine&& o) noexcept;

    // 宿主回调（由 Account 注入）
    using LogFn = void (*)(void* ud, const std::string& line);
    using SendFn = void (*)(void* ud, const std::string& cmd);
    using StateFn = std::string (*)(void* ud);   // 返回 JSON 字符串 {hp,maxHp,mp,maxMp,state,room,living,combat,free,idle,idle_time,level,name}

    void set_log(LogFn fn, void* ud) { log_ = fn; log_ud_ = ud; }
    void set_send(SendFn fn, void* ud) { send_ = fn; send_ud_ = ud; }
    void set_state(StateFn fn, void* ud) { state_ = fn; state_ud_ = ud; }
    // 角色 id：Buff 事件"触发对象"（data.id == 自己）判断
    void set_role_id(const std::string& id) { role_id_ = id; }

    // 持久化：程序只读不写——触发器统一由软件同级目录 trigger.json 维护。
    // 旧接口保留（save 为 no-op，避免脚本 @setTrigger → host_set_trigger → save 写盘）
    void set_save_path(const std::string& path) { save_path_ = path; }
    void load();
    void save();

    // 从 trigger.json 加载到输出列表（静态，不依赖实例）；校验失败返回 false+err
    static bool load_from_file(const std::string& path, std::vector<Trigger>& out, std::string& err);
    // 把触发器列表序列化为 trigger.json 顶层文本（含 version，source 用数组格式，键与 load_from_file 一致）
    static std::string dump_triggers(const std::vector<Trigger>& ts);
    // 用新配置整体替换本引擎触发器（不落盘）
    void replace(const std::vector<Trigger>& ts);

    const std::vector<Trigger>& list() const { return triggers_; }
    std::size_t count() const { return triggers_.size(); }

    // 导入分享码（云端返回的触发器 JSON 字符串，即 importTrigger 中 JSON.parse(data) 的输入）
    bool import(const std::string& data_json, std::string& err);
    // 开关（1-based 编号，与 F8 列表一致）
    bool enable(std::size_t idx, std::string& err);
    bool disable(std::size_t idx, std::string& err);

    // ---------- 编辑/创建（F8 编辑屏 / 创建按钮） ----------
    // 更新指定触发器（idx 0-based）
    bool update(std::size_t idx, const std::string& name, const std::string& event,
                const std::vector<std::pair<std::string, std::string>>& conds,
                const std::string& source, bool active, std::string& err);
    // 新增触发器（末尾追加）
    bool add(const std::string& name, const std::string& event,
             const std::vector<std::pair<std::string, std::string>>& conds,
             const std::string& source, bool active, std::string& err);
    // 删除触发器（idx 0-based）
    bool remove(std::size_t idx, std::string& err);

    // 事件模板清单：事件名 + 该事件的过滤器（条件键, 断言类型中文名），供编辑屏/创建屏使用
    static std::vector<std::string> event_names();
    static std::vector<std::pair<std::string, std::string>> event_filters(const std::string& event);

    // 表单字段：驱动 TUI 条件键编辑器
    struct CondField {
        std::string key;                  // 条件键名
        std::string label;                // 中文说明
        std::string type;                 // "text"=自由文本  "int"=整数  "enum"=从 options 选
        std::vector<std::string> options; // enum 时的候选取值（type 键依事件而异）
    };
    // 该事件无条件表字段（键+label+类型+枚举候选）
    static std::vector<CondField> cond_fields(const std::string& event);
    // 事件英文枚举 → 中文标签
    static const char* event_label(const std::string& event);

    // ---------- 事件入口（由 Account::on_json 分发） ----------
    void on_text(const std::string& msg);                                   // 新提示信息 + 战斗脱离启发
    void on_chat(const std::string& ch, const std::string& name,            // 新聊天信息
                 const std::string& uid, const std::string& content);
    void on_social(const std::string& content);                             // 社交消息（dialog=message）
    void on_auction(const std::string& id, const std::string& raw_name,     // 拍卖查询（pm list 逐项）
                    const std::string& price, const std::string& raw_time);
    void on_activity(const std::string& type, const std::string& name,      // 活动事件（events 逐项）
                     const std::string& keyword, const std::string& grade,
                     const std::string& times);
    void on_itemadd(const std::string& id, const std::string& name);        // 人物刷新（同时更新房间缓存）
    void on_itemremove(const std::string& id);                              // 房间缓存删除
    void on_pack(const std::string& id, const std::string& name,            // 物品拾取（dialog=pack）
                 const std::string& count);
    void on_status(const std::string& id, const std::string& sid,           // Buff 状态改变（逐个 sid 调用）
                   const std::string& action, const std::string& name,
                   const std::string& count, const std::string& duration);
    void on_combat_enter();                                                 // 战斗状态切换·进入战斗
    void on_combat_leave();                                                 // 战斗状态切换·脱离战斗
    void on_die(const std::string& state);                                  // 死亡状态改变（已经死亡/已经复活）
    void on_dispfm(const std::string& id, const std::string& rtime,         // 技能释放 + 冷却结束调度
                   const std::string& distime, int64_t now_ms);
    void on_sc(const std::string& id, const std::string& hp,                // 气血内力改变 + 伤害已满
               const std::string& mp, const std::string& max_hp,
               const std::string& max_mp, const std::string& damage);
    void on_items(const std::vector<RoomItemData>& items);                  // 重建房间缓存（type=items）
    void on_room_clear();                                                   // 房间变化 → 清空缓存（type=room）
    void tick(int64_t now_ms);                                              // 主循环每帧：冷却结束/时辰已到/流程推进
    void feed_text(const std::string& text);                                // @tip 喂文本

    // ---------- 脚本宿主（raid_interp.js 的 __cxx 回调） ----------
    void emit_log(const char* s);
    void emit_send(const std::string& cmd);
    std::string host_state();
    std::string host_room_item_id(const std::string& name, bool exact);
    bool host_room_has_item(const std::string& name, bool exact);
    int64_t host_cooling_until(const std::string& sid);
    void host_set_trigger(const std::string& name, bool active);
    int64_t host_now();

private:
    struct JsEnv;                        // 定义于 trigger.cpp
    void* js_env_ = nullptr;             // JsEnv*
    int interrupt_count_ = 0;
    static int js_interrupt_handler(JSRuntime* rt, void* opaque);   // 防死循环（单次调用上限）

    std::vector<Trigger> triggers_;
    std::string save_path_;
    std::string role_id_;

    LogFn log_ = nullptr;    void* log_ud_ = nullptr;
    SendFn send_ = nullptr;  void* send_ud_ = nullptr;
    StateFn state_ = nullptr; void* state_ud_ = nullptr;

    struct RoomItem {
        std::string name;
        double hp = 0, mp = 0, max_hp = 0, max_mp = 0;
        bool have_max_hp = false, have_max_mp = false;
        double damage = 0, damagePer = 0;
        bool have_damage = false;
    };
    std::map<std::string, RoomItem> room_;        // 房间缓存：id → 条目
    std::map<std::string, int64_t> cooldowns_;    // 技能 id → 冷却结束时刻（ms）
    struct PendingCd { int64_t due; std::string sid; };
    std::vector<PendingCd> pending_cd_;           // 待发"技能冷却结束"事件
    int last_clock_sec_ = -1;                     // 上次"时辰已到"秒

    // 触发链（与扩展 Trigger._action 一致）
    void fire(const char* event, const Params& params);
    void run_trigger(const Trigger& t, const Params& params);

    // JS 桥接
    bool js_init();                               // 初始化运行时 + 内嵌 raid_interp.js
    void js_start(const std::string& name, const std::string& source);
    void js_tick();
    void js_feed(const std::string& text);
};

}  // namespace trigger
}  // namespace wsmud
