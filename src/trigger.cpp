// 触发器引擎实现（QuickJS 内嵌 raid_interp.js 执行 Raid 脚本）
#include "trigger.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "quickjs/quickjs.h"
#include "raid_interp.gen.hpp"   // 由 CMake configure_file 生成，内嵌 raid_interp.js

namespace wsmud {
namespace trigger {

namespace {

// ---------- 小工具 ----------

int64_t mono_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// JS 数值转字符串（整数不打小数位）
std::string num_str(double d) {
    if (std::isfinite(d) && d == static_cast<int64_t>(d)) {
        return std::to_string(static_cast<int64_t>(d));
    }
    char b[40];
    snprintf(b, sizeof b, "%g", d);
    return b;
}

// 任意 JSON 值 → 字符串
std::string val_str(const json::Value& v) {
    if (v.is_string()) return v.as_string();
    if (v.is_number()) return num_str(v.as_number());
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    return "";
}

// JS toFixed(2)
std::string fmt2(double v) {
    if (!std::isfinite(v)) return "0.00";
    char b[32];
    snprintf(b, sizeof b, "%.2f", v);
    return b;
}

// 宽松字符串转浮点（无法解析 → NaN，语义同 JS parseFloat）
double parse_float(const std::string& s) {
    if (s.empty()) return NAN;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return NAN;
    return v;
}

// 通配条件：空 / 空白 / * （/^\s*\*?\s*$/）
bool is_wild(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return b == a || (b - a == 1 && s[a] == '*');
}

// 纯整数字符串（可选 +/- 前缀）：用于判定整数条件键的值是否合法
bool is_int_str(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// 值必须为整数的条件键（trigger.json 中这些键写整数，不写字符串）
bool is_int_key(const std::string& key) {
    return key == "hour" || key == "minute" || key == "second" ||
           key == "item_level";
}

// 整数字符串规范化：去前导零 / 正号（"08"→"8"、"+8"→"8"），非整数返回原串
std::string norm_int_str(const std::string& s) {
    if (!is_int_str(s)) return s;
    char* endp = nullptr;
    long long v = std::strtoll(s.c_str(), &endp, 10);
    if (endp == s.c_str()) return s;
    return std::to_string(v);
}

// 按 "|" 精确匹配（不 trim，语义同扩展 split("|").indexOf）
bool pipe_match(const std::string& user, const std::string& game) {
    if (is_wild(user)) return true;
    std::size_t start = 0;
    for (;;) {
        std::size_t p = user.find('|', start);
        std::string part = p == std::string::npos ? user.substr(start) : user.substr(start, p - start);
        if (part == game) return true;
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return false;
}

// KeyAssert 表达式求值：() 括号 → MATCHED/UNMATCHED，&& 且、|| 或，裸词=包含匹配
bool key_eval_and(const std::string& expr, const std::string& rh, bool& ok) {
    ok = true;
    std::size_t start = 0;
    for (;;) {
        std::size_t p = expr.find("&&", start);
        std::string part = p == std::string::npos ? expr.substr(start) : expr.substr(start, p - start);
        std::size_t a = 0, b = part.size();
        while (a < b && (part[a] == ' ' || part[a] == '\t')) ++a;
        while (b > a && (part[b - 1] == ' ' || part[b - 1] == '\t')) --b;
        std::string t = part.substr(a, b - a);
        if (t == "MATCHED") { /* 继续 */ }
        else if (t == "UNMATCHED") return false;
        else if (rh.find(t) == std::string::npos) return false;
        if (p == std::string::npos) break;
        start = p + 2;
    }
    return true;
}

bool key_eval_or(const std::string& expr, const std::string& rh) {
    std::size_t start = 0;
    for (;;) {
        std::size_t p = expr.find("||", start);
        std::string part = p == std::string::npos ? expr.substr(start) : expr.substr(start, p - start);
        bool ok = false;
        if (key_eval_and(part, rh, ok)) return true;
        if (p == std::string::npos) break;
        start = p + 2;
    }
    return false;
}

bool key_assert(const std::string& user, const std::string& game) {
    if (is_wild(user)) return true;
    std::string expr = user;
    // 括号：从最内层开始递归替换（lastIndexOf('(') = 最内层）
    while (expr.find('(') != std::string::npos) {
        std::size_t st = expr.rfind('(');
        std::size_t en = expr.find(')', st);
        if (en == std::string::npos) break;
        std::string sub = expr.substr(st + 1, en - st - 1);
        expr = expr.substr(0, st) + (key_eval_or(sub, game) ? "MATCHED" : "UNMATCHED") + expr.substr(en + 1);
    }
    return key_eval_or(expr, game);
}

// Crossing：跨过阈值（气血/内力）
bool crossing_assert(const std::string& user, const std::string& game) {
    double uv = parse_float(user);
    std::size_t p = game.find(';');
    double old_v = parse_float(p == std::string::npos ? game : game.substr(0, p));
    double new_v = p == std::string::npos ? NAN : parse_float(game.substr(p + 1));
    return (old_v >= uv && new_v < uv) || (old_v <= uv && new_v > uv);
}
// CrossingUp：上穿阈值（伤害）
bool crossing_up_assert(const std::string& user, const std::string& game) {
    double uv = parse_float(user);
    std::size_t p = game.find(';');
    double old_v = parse_float(p == std::string::npos ? game : game.substr(0, p));
    double new_v = p == std::string::npos ? NAN : parse_float(game.substr(p + 1));
    return old_v <= uv && new_v > uv;
}

// 事件模板表：过滤器（条件键名 + 断言类型）。与 trigger-core-templates.js / trigger-events-*.js 一一对应
enum class FilterType { Equal, Contain, ContainReverse, Key, TimeReached, Crossing, CrossingUp, ActivityName, Channel };
struct FilterDef { const char* name; FilterType type; };
struct EventDef {
    const char* name;
    const FilterDef* filters;
    int nfilters;
};

// 断言类型的英文名（模板清单展示用）
const char* filter_type_name(FilterType ty) {
    switch (ty) {
        case FilterType::Equal: return "equal";
        case FilterType::Contain: return "contains";
        case FilterType::ContainReverse: return "not_contains";
        case FilterType::Key: return "keyword";
        case FilterType::TimeReached: return "time_reached";
        case FilterType::Crossing: return "crossing";
        case FilterType::CrossingUp: return "crossing_up";
        case FilterType::ActivityName: return "activity_name";
        case FilterType::Channel: return "channel";
    }
    return "unknown";
}

const FilterDef F_TEXT[]  = {{"keyword", FilterType::Key}};
const FilterDef F_SOCIAL[]= {{"keyword", FilterType::Key}};
const FilterDef F_AUCTION[]= {{"keyword", FilterType::Key}, {"item_level", FilterType::Contain}};
const FilterDef F_EVENTS[]= {{"name", FilterType::ActivityName}, {"keyword", FilterType::Key}};
const FilterDef F_CHAT[]  = {{"channel", FilterType::Channel}, {"speaker", FilterType::Contain},
                             {"ignore_speaker", FilterType::ContainReverse}, {"keyword", FilterType::Key}};
const FilterDef F_ITEMADD[]={{ "char_name", FilterType::Key }};
const FilterDef F_PACK[] = {{"name_keyword", FilterType::Key}};
const FilterDef F_STATUS[]= {{"change_type", FilterType::Equal}, {"buff_id", FilterType::Contain},
                             {"target", FilterType::Equal}};
const FilterDef F_COMBAT[]= {{"type", FilterType::Equal}};
const FilterDef F_DIE[]  = {{"type", FilterType::Equal}};
const FilterDef F_TIME[] = {{"hour", FilterType::TimeReached}, {"minute", FilterType::TimeReached},
                            {"second", FilterType::TimeReached}};
const FilterDef F_DISPFM[]= {{"skill_id", FilterType::Contain}};
const FilterDef F_HPMP[] = {{"name_keyword", FilterType::Key}, {"type", FilterType::Equal},
                            {"when", FilterType::Equal}, {"value_type", FilterType::Equal}, {"value", FilterType::Crossing}};
const FilterDef F_DMG[]  = {{"name_keyword", FilterType::Key}, {"value_type", FilterType::Equal}, {"value", FilterType::CrossingUp}};

const EventDef EVENTS[] = {
    {"hint",         F_TEXT,    1},
    {"social",       F_SOCIAL,  1},
    {"auction",      F_AUCTION, 2},
    {"activity",     F_EVENTS,  2},
    {"chat",         F_CHAT,    4},
    {"char_refresh", F_ITEMADD, 1},
    {"item_pickup",  F_PACK,    1},
    {"buff_change",  F_STATUS,  3},
    {"combat",       F_COMBAT,  1},
    {"death",        F_DIE,     1},
    {"time_reached", F_TIME,    3},
    {"skill_cast",   F_DISPFM,  1},
    {"skill_cd",     F_DISPFM,  1},
    {"hp_mp",        F_HPMP,    5},
    {"damage_full",  F_DMG,     3},
};
constexpr int N_EVENTS = static_cast<int>(sizeof(EVENTS) / sizeof(EVENTS[0]));

const EventDef* find_event(const std::string& name) {
    for (int i = 0; i < N_EVENTS; ++i)
        if (name == EVENTS[i].name) return &EVENTS[i];
    return nullptr;
}

// ---------- 表单元数据（供 TUI 编辑屏驱动） ----------

// 事件名的中文标签
const char* event_label_name(const std::string& ev) {
    if (ev == "hint") return "新提示信息";
    if (ev == "social") return "社交消息";
    if (ev == "auction") return "拍卖查询";
    if (ev == "activity") return "活动事件";
    if (ev == "chat") return "新聊天信息";
    if (ev == "char_refresh") return "人物刷新";
    if (ev == "item_pickup") return "物品拾取";
    if (ev == "buff_change") return "Buff状态改变";
    if (ev == "combat") return "战斗状态切换";
    if (ev == "death") return "死亡状态改变";
    if (ev == "time_reached") return "时辰已到";
    if (ev == "skill_cast") return "技能释放";
    if (ev == "skill_cd") return "技能冷却结束";
    if (ev == "hp_mp") return "气血内力改变";
    if (ev == "damage_full") return "伤害已满";
    return ev.c_str();
}

// 条件键的中文说明
const char* cond_label_name(const std::string& key) {
    if (key == "keyword") return "任意文本（包含匹配）";
    if (key == "name") return "活动名称";
    if (key == "item_level") return "颜色等级";
    if (key == "channel") return "频道";
    if (key == "speaker") return "发言人";
    if (key == "ignore_speaker") return "忽略的发言人";
    if (key == "char_name") return "人物名称";
    if (key == "name_keyword") return "名称关键字";
    if (key == "change_type") return "变化类型";
    if (key == "buff_id") return "Buff ID";
    if (key == "target") return "触发对象";
    if (key == "type") return "类型";
    if (key == "hour") return "时（0-23）";
    if (key == "minute") return "分（0-59）";
    if (key == "second") return "秒（0-59）";
    if (key == "skill_id") return "技能名称";
    if (key == "when") return "触发时机";
    if (key == "value_type") return "值类型";
    if (key == "value") return "阈值";
    return key.c_str();
}

// 枚举候选（依事件给 type 键不同取值）；非枚举键返回空
std::vector<std::string> cond_options(const std::string& event, const std::string& key) {
    std::vector<std::string> out;
    if (key == "channel") { out = {"全部", "世界", "队伍", "门派", "全区", "帮派", "谣言", "系统"}; }
    else if (key == "item_level") { out = {"0", "1", "2", "3", "4", "5"}; }
    else if (key == "change_type") { out = {"新增", "移除", "层数刷新"}; }
    else if (key == "target") { out = {"自己", "他人"}; }
    else if (key == "when") { out = {"低于", "高于"}; }
    else if (key == "value_type") { out = {"百分比", "数值"}; }
    else if (key == "type") {
        if (event == "combat") out = {"进入战斗", "脱离战斗"};
        else if (event == "death") out = {"已经死亡", "已经复活"};
        else if (event == "hp_mp") out = {"气血", "内力"};
    }
    return out;
}

// 单个候选的字符串断言（不含 || 或；空/通配已在上层处理）
bool assert_single(FilterType ty, const std::string& user, const std::string& game) {
    switch (ty) {
        case FilterType::Equal:
            // JS 宽松相等：双方均可解析为数字 → 数值比较；否则字符串比较
            {
                double a = parse_float(user), b = parse_float(game);
                if (std::isfinite(a) && std::isfinite(b) && !user.empty() && !game.empty()) return a == b;
            }
            return user == game;
        case FilterType::Contain:        return pipe_match(user, game);
        case FilterType::ContainReverse: return pipe_match(user, game);
        case FilterType::TimeReached:    return pipe_match(user, game);
        case FilterType::ActivityName: {
            if (game.empty()) return false;
            std::string a = game, b = user;
            for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return a.find(b) != std::string::npos;
        }
        case FilterType::Channel:        return user == "全部" || user == game;
        default:                         return false;
    }
}

bool assert_filter(FilterType ty, const std::string& user, const std::string& game) {
    // 空/通配用户条件 = 不设限制，一律通过。
    // 否则导入/加载进来的触发器（如"忽略发言人"=空，load() 还会自动补空值）
    // 会在 ContainReverse/Equal 等断言上误判失败而永不触发。
    if (is_wild(user)) return true;
    // 表达式类（Key，内建 || / && / 括号）与数值类（Crossing/CrossingUp，阈值跨线）不做 || 拆分
    if (ty == FilterType::Key)          return key_assert(user, game);
    if (ty == FilterType::Crossing)     return crossing_assert(user, game);
    if (ty == FilterType::CrossingUp)   return crossing_up_assert(user, game);
    // 其余字符串断言统一支持 "||" 表示或：任一候选命中即通过（ContainReverse 相反：任一命中即排除）
    std::size_t start = 0;
    for (;;) {
        std::size_t p = user.find("||", start);
        std::string part = p == std::string::npos ? user.substr(start) : user.substr(start, p - start);
        if (assert_single(ty, part, game)) return ty != FilterType::ContainReverse;
        if (p == std::string::npos) break;
        start = p + 2;
    }
    // 全部候选均未命中：普通断言失败；反向断言（排除）成功
    return ty == FilterType::ContainReverse;
}

// 注入参数值的 JS 文本形式：空 → null；纯数字 → 原样；字符串 → 双引号（解释器 tokenize 不支持转义，剔除引号/反斜杠/控制字符）
std::string inject_value(const std::string& v) {
    if (v.empty()) return "null";
    bool numeric = !v.empty();
    for (char c : v)
        if (!(c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9'))) { numeric = false; break; }
    if (numeric) return v;
    std::string out = "\"";
    for (char c : v) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\' || u < 0x20) out += ' ';
        else out += c;
    }
    out += '"';
    return out;
}

bool is_cjk_utf8(const std::string& s, std::size_t i) {
    if (i + 2 >= s.size()) return false;
    unsigned char b1 = static_cast<unsigned char>(s[i]);
    unsigned char b2 = static_cast<unsigned char>(s[i + 1]);
    unsigned char b3 = static_cast<unsigned char>(s[i + 2]);
    if (b1 >= 0xE4 && b1 <= 0xE9) {
        if (b1 == 0xE4) return b2 >= 0xB8 && b2 <= 0xBF;      // U+4E00..U+4FFF 区间上界处理放宽
        if (b1 == 0xE9) return b2 <= 0xBE;                    // U+9Fxx 部分；放宽到 U+9FFF
        return b2 >= 0x80 && b2 <= 0xBF;
    }
    return false;
}

bool name_ok(const std::string& name) {
    if (name.empty()) return false;
    std::size_t i = 0;
    while (i < name.size()) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') { ++i; continue; }
        if (is_cjk_utf8(name, i)) { i += 3; continue; }
        return false;
    }
    return true;
}

}  // namespace

// ============================================================
// QuickJS 桥接
// ============================================================

struct Engine::JsEnv {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSValue fn_start = JS_UNDEFINED;
    JSValue fn_tick = JS_UNDEFINED;
    JSValue fn_feed = JS_UNDEFINED;
};

static Engine* engine_of(JSContext* ctx) {
    return static_cast<Engine*>(JS_GetContextOpaque(ctx));
}

int Engine::js_interrupt_handler(JSRuntime* rt, void* opaque) {
    (void)rt;
    Engine* e = static_cast<Engine*>(opaque);
    e->interrupt_count_++;
    return e->interrupt_count_ > 20000000 ? 1 : 0;   // 单次调用上限，防死循环
}

static JSValue js_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Engine* e = engine_of(ctx);
    if (!e) return JS_UNDEFINED;
    const char* s = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    if (s) { e->emit_log(s); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue js_send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Engine* e = engine_of(ctx);
    if (!e) return JS_UNDEFINED;
    const char* s = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    if (s) { e->emit_send(s); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue js_state(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Engine* e = engine_of(ctx);
    if (!e) return JS_UNDEFINED;
    return JS_NewString(ctx, e->host_state().c_str());
}
static JSValue js_roomItemId(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Engine* e = engine_of(ctx);
    if (!e || argc < 1) return JS_NULL;
    const char* name = JS_ToCString(ctx, argv[0]);
    bool exact = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    std::string id = name ? e->host_room_item_id(name, exact) : "";
    if (name) JS_FreeCString(ctx, name);
    return id.empty() ? JS_NULL : JS_NewString(ctx, id.c_str());
}
static JSValue js_roomHasItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Engine* e = engine_of(ctx);
    if (!e || argc < 1) return JS_FALSE;
    const char* name = JS_ToCString(ctx, argv[0]);
    bool exact = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    bool has = name ? e->host_room_has_item(name, exact) : false;
    if (name) JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, has);
}
static JSValue js_coolingUntil(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Engine* e = engine_of(ctx);
    if (!e || argc < 1) return JS_NewInt64(ctx, 0);
    const char* sid = JS_ToCString(ctx, argv[0]);
    int64_t v = sid ? e->host_cooling_until(sid) : 0;
    if (sid) JS_FreeCString(ctx, sid);
    return JS_NewInt64(ctx, v);
}
static JSValue js_setTrigger(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Engine* e = engine_of(ctx);
    if (!e || argc < 1) return JS_UNDEFINED;
    const char* name = JS_ToCString(ctx, argv[0]);
    bool active = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    if (name) { e->host_set_trigger(name, active); JS_FreeCString(ctx, name); }
    return JS_UNDEFINED;
}
static JSValue js_now(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Engine* e = engine_of(ctx);
    if (!e) return JS_NewInt64(ctx, 0);
    return JS_NewInt64(ctx, e->host_now());
}

// ---------- 构造 / 析构 / 移动 ----------

Engine::Engine() {
    js_init();
}
Engine::~Engine() {
    if (js_env_) {
        JsEnv* j = static_cast<JsEnv*>(js_env_);
        JS_FreeValue(j->ctx, j->fn_start);
        JS_FreeValue(j->ctx, j->fn_tick);
        JS_FreeValue(j->ctx, j->fn_feed);
        JS_FreeContext(j->ctx);
        JS_FreeRuntime(j->rt);
        delete j;
        js_env_ = nullptr;
    }
}
Engine::Engine(Engine&& o) noexcept
    : js_env_(o.js_env_), interrupt_count_(o.interrupt_count_),
      triggers_(std::move(o.triggers_)), save_path_(std::move(o.save_path_)),
      role_id_(std::move(o.role_id_)),
      log_(o.log_), log_ud_(o.log_ud_), send_(o.send_), send_ud_(o.send_ud_),
      state_(o.state_), state_ud_(o.state_ud_),
      room_(std::move(o.room_)), cooldowns_(std::move(o.cooldowns_)),
      pending_cd_(std::move(o.pending_cd_)), last_clock_sec_(o.last_clock_sec_) {
    o.js_env_ = nullptr;
}
Engine& Engine::operator=(Engine&& o) noexcept {
    if (this == &o) return *this;
    this->~Engine();
    js_env_ = o.js_env_; o.js_env_ = nullptr;
    interrupt_count_ = o.interrupt_count_;
    triggers_ = std::move(o.triggers_);
    save_path_ = std::move(o.save_path_);
    role_id_ = std::move(o.role_id_);
    log_ = o.log_; log_ud_ = o.log_ud_;
    send_ = o.send_; send_ud_ = o.send_ud_;
    state_ = o.state_; state_ud_ = o.state_ud_;
    room_ = std::move(o.room_);
    cooldowns_ = std::move(o.cooldowns_);
    pending_cd_ = std::move(o.pending_cd_);
    last_clock_sec_ = o.last_clock_sec_;
    return *this;
}

bool Engine::js_init() {
    JsEnv* j = new JsEnv();
    j->rt = JS_NewRuntime();
    if (!j->rt) { delete j; return false; }
    JS_SetInterruptHandler(j->rt, js_interrupt_handler, this);
    j->ctx = JS_NewContext(j->rt);
    if (!j->ctx) { JS_FreeRuntime(j->rt); delete j; return false; }
    JS_SetContextOpaque(j->ctx, this);

    // 定义 __cxx 宿主对象
    JSValue global = JS_GetGlobalObject(j->ctx);
    JSValue cxx = JS_NewObject(j->ctx);
    struct Cfn { const char* name; JSCFunction* fn; };
    const Cfn cfns[] = {
        {"log", js_log}, {"send", js_send}, {"state", js_state},
        {"roomItemId", js_roomItemId}, {"roomHasItem", js_roomHasItem},
        {"coolingUntil", js_coolingUntil}, {"setTrigger", js_setTrigger}, {"now", js_now},
    };
    for (const auto& c : cfns) {
        JSValue f = JS_NewCFunction2(j->ctx, c.fn, c.name, 1, JS_CFUNC_generic, 0);
        JS_DefinePropertyValueStr(j->ctx, cxx, c.name, f, JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(j->ctx, global, "__cxx", cxx, JS_PROP_C_W_E);

    // 内嵌 raid_interp.js
    JSValue r = JS_Eval(j->ctx, INTERP_JS, std::strlen(INTERP_JS), "raid_interp.js", 0);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(j->ctx);
        const char* s = JS_ToCString(j->ctx, exc);
        std::string msg = s ? std::string("解释器初始化失败: ") + s : "解释器初始化失败";
        emit_log(msg.c_str());
        if (s) JS_FreeCString(j->ctx, s);
        JS_FreeValue(j->ctx, exc);
        JS_FreeValue(j->ctx, r);
        JS_FreeValue(j->ctx, global);
        JS_FreeContext(j->ctx);
        JS_FreeRuntime(j->rt);
        delete j;
        js_env_ = nullptr;
        return false;
    }
    JS_FreeValue(j->ctx, r);

    j->fn_start = JS_GetPropertyStr(j->ctx, global, "__start");
    j->fn_tick  = JS_GetPropertyStr(j->ctx, global, "__tick");
    j->fn_feed  = JS_GetPropertyStr(j->ctx, global, "__feed_text");
    JS_FreeValue(j->ctx, global);
    js_env_ = j;
    return true;
}

static void js_call_void(JSContext* ctx, JSValueConst fn, int argc, JSValueConst* argv) {
    if (JS_IsUndefined(fn)) return;
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, exc);
        if (s) { JS_FreeCString(ctx, s); }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);
}

void Engine::js_start(const std::string& name, const std::string& source) {
    if (!js_env_) return;
    JsEnv* j = static_cast<JsEnv*>(js_env_);
    interrupt_count_ = 0;
    JSValue args[2] = { JS_NewString(j->ctx, name.c_str()), JS_NewString(j->ctx, source.c_str()) };
    js_call_void(j->ctx, j->fn_start, 2, args);
    JS_FreeValue(j->ctx, args[0]);
    JS_FreeValue(j->ctx, args[1]);
}
void Engine::js_tick() {
    if (!js_env_) return;
    JsEnv* j = static_cast<JsEnv*>(js_env_);
    interrupt_count_ = 0;
    js_call_void(j->ctx, j->fn_tick, 0, nullptr);
}
void Engine::js_feed(const std::string& text) {
    if (!js_env_ || text.empty()) return;
    JsEnv* j = static_cast<JsEnv*>(js_env_);
    interrupt_count_ = 0;
    JSValue arg = JS_NewString(j->ctx, text.c_str());
    js_call_void(j->ctx, j->fn_feed, 1, &arg);
    JS_FreeValue(j->ctx, arg);
}

// ============================================================
// 触发链（与扩展 Trigger._action 一致）
// ============================================================

void Engine::fire(const char* event, const Params& params) {
    for (const auto& t : triggers_) {
        if (!t.active) continue;
        if (t.event != event) continue;
        run_trigger(t, params);
    }
}

void Engine::run_trigger(const Trigger& t, const Params& params) {
    const EventDef* ev = find_event(t.event);
    if (!ev) return;   // 未知事件模板：不触发

    Params real = params;
    for (const auto& kv : t.conditions) {
        // 按条件键名查事件模板同名过滤器；找不到 → 不触发
        const FilterDef* f = nullptr;
        for (int i = 0; i < ev->nfilters; ++i)
            if (std::string(ev->filters[i].name) == kv.first) { f = &ev->filters[i]; break; }
        if (!f) return;
        auto it = params.find(kv.first);
        std::string from_game = it == params.end() ? "" : it->second;
        if (!assert_filter(f->type, kv.second, from_game)) return;
        real.erase(kv.first);   // 被条件使用的键不注入脚本
    }

    // 注入剩余参数：($键) = 值
    std::string source = t.source;
    for (auto it = real.rbegin(); it != real.rend(); ++it)
        source = "($" + it->first + ") = " + inject_value(it->second) + "\n" + source;

    // ~silent 检查：源码中含 "// ~silent" 注释行则不打印触发提示
    bool silent = false;
    std::size_t p = t.source.find("~silent");
    if (p != std::string::npos) {
        std::size_t ls = t.source.rfind('\n', p);
        std::size_t start = (ls == std::string::npos) ? 0 : ls + 1;
        std::size_t j = start;
        while (j < p && (t.source[j] == ' ' || t.source[j] == '\t')) ++j;
        if (j + 1 < p && t.source[j] == '/' && t.source[j + 1] == '/') silent = true;
    }
    if (!silent)
        source = "@print 💡<hio>触发=>" + t.name + "</hio>\n" + source;

    js_start(t.name, source);
}

// ============================================================
// 事件分发
// ============================================================

void Engine::on_text(const std::string& msg) {
    if (msg.empty()) return;
    js_feed(msg);   // @tip 等待文本
    Params p;
    std::string flat;
    for (char c : msg) if (c != '\n' && c != '\r') flat += c;
    p["keyword"] = flat;
    p["text"] = flat;
    fire("hint", p);
    // 文本提示 → 脱离战斗
    if (msg.find("只能在战斗中使用") != std::string::npos ||
        msg.find("这里不允许战斗") != std::string::npos ||
        msg.find("没时间这么做") != std::string::npos) {
        Params c; c["type"] = "脱离战斗";
        fire("combat", c);
    }
}

void Engine::on_chat(const std::string& ch, const std::string& name,
                     const std::string& uid, const std::string& content) {
    std::string channel;
    if (ch == "chat") channel = "世界";
    else if (ch == "tm") channel = "队伍";
    else if (ch == "fam") channel = "门派";
    else if (ch == "es") channel = "全区";
    else if (ch == "pty") channel = "帮派";
    else if (ch == "rumor") channel = "谣言";
    else if (ch == "sys") channel = "系统";
    if (channel.empty()) return;
    std::string flat;
    for (char c : content) if (c != '\n' && c != '\r') flat += c;
    Params p;
    p["channel"] = channel;
    p["speaker"] = name.empty() ? "无" : name;
    p["keyword"] = content;
    p["ignore_speaker"] = name.empty() ? "无" : name;
    p["content"] = flat;
    p["name"] = name.empty() ? "无" : name;
    p["id"] = uid;
    p["channel"] = channel;
    fire("chat", p);
}

void Engine::on_social(const std::string& content) {
    Params p;
    p["keyword"] = content;
    p["msg"] = content;
    fire("social", p);
}

void Engine::on_auction(const std::string& id, const std::string& raw_name,
                        const std::string& price, const std::string& raw_time) {
    // 名称/等级提取（与扩展一致：颜色标签取中间文本；无标签去掉"^N份"前缀）
    std::string name = "未知物品";
    std::string grade = "0";
    std::size_t tag_end = raw_name.find(">");
    if (tag_end != std::string::npos && raw_name.size() > tag_end + 1) {
        std::size_t close = raw_name.find("</", tag_end);
        std::size_t e2 = close == std::string::npos ? raw_name.size() : close;
        std::string inner = raw_name.substr(tag_end + 1, e2 - tag_end - 1);
        if (!inner.empty()) name = inner;
    } else {
        name = raw_name;
        std::size_t d = raw_name.find("份");
        if (d != std::string::npos) {
            bool all_digit = d > 0;
            for (std::size_t k = 0; k < d; ++k) if (!(raw_name[k] >= '0' && raw_name[k] <= '9')) { all_digit = false; break; }
            if (all_digit) name = raw_name.substr(d + 1);
        }
    }
    // trim
    std::size_t a = 0, b = name.size();
    while (a < b && (name[a] == ' ' || name[a] == '\t')) ++a;
    while (b > a && (name[b - 1] == ' ' || name[b - 1] == '\t')) --b;
    name = name.substr(a, b - a);
    struct { const char* tag; const char* g; } gmap[] = {
        {"<hig>", "1"}, {"<hic>", "2"}, {"<hiy>", "3"}, {"<HIZ>", "4"}, {"<hio>", "5"}};
    for (const auto& g : gmap) {
        if (raw_name.find(g.tag) != std::string::npos) { grade = g.g; break; }
    }
    int64_t time = 0;
    double tsec = parse_float(raw_time) / 1000.0;
    if (std::isfinite(tsec)) time = static_cast<int64_t>(std::floor(tsec));
    Params p;
    p["keyword"] = name;
    p["item_level"] = grade;
    p["id"] = id;
    p["name"] = name;
    p["grade"] = grade;
    p["price"] = price;
    p["time"] = num_str(time);
    fire("auction", p);
}

void Engine::on_activity(const std::string& type, const std::string& name,
                         const std::string& keyword, const std::string& grade,
                         const std::string& times) {
    Params p;
    p["name"] = name;
    p["keyword"] = keyword;
    p["type"] = type;
    p["grade"] = grade;
    p["times"] = times;
    p["content"] = keyword;
    p["event"] = name;
    fire("activity", p);
}

void Engine::on_itemadd(const std::string& id, const std::string& name) {
    if (id.empty() || name.empty()) return;
    // 更新房间缓存（与扩展一致：整条替换）
    RoomItem it;
    it.name = name;
    room_[id] = it;
    Params p;
    p["char_name"] = name;
    p["id"] = id;
    p["name"] = name;
    fire("char_refresh", p);
}

void Engine::on_itemremove(const std::string& id) {
    if (!id.empty()) room_.erase(id);
}

void Engine::on_pack(const std::string& id, const std::string& name, const std::string& count) {
    // 品质：首个 <xxx> 标签映射
    std::string quality = "未知";
    struct { const char* tag; const char* q; } qmap[] = {
        {"<wht>", "白"}, {"<hig>", "绿"}, {"<hic>", "蓝"}, {"<hiy>", "黄"},
        {"<HIZ>", "紫"}, {"<hio>", "橙"}, {"<ord>", "红"}};
    for (const auto& q : qmap) {
        if (name.find(q.tag) != std::string::npos) { quality = q.q; break; }
    }
    Params p;
    p["name_keyword"] = name;
    p["id"] = id;
    p["name"] = name;
    p["count"] = count;
    p["quality"] = quality;
    fire("item_pickup", p);
}

void Engine::on_status(const std::string& id, const std::string& sid, const std::string& action,
                       const std::string& name, const std::string& count, const std::string& duration) {
    std::string atype;
    if (action == "add") atype = "新增";
    else if (action == "remove") atype = "移除";
    else if (action == "refresh") atype = "层数刷新";
    else return;
    Params p;
    p["change_type"] = atype;
    p["buff_id"] = sid;
    p["target"] = (id == role_id_) ? "自己" : "他人";
    p["id"] = id;
    p["sid"] = sid;
    p["count"] = count.empty() ? "0" : count;
    p["duration"] = duration.empty() ? "0" : duration;
    p["name"] = name;
    fire("buff_change", p);
}

void Engine::on_combat_enter() {
    Params p; p["type"] = "进入战斗";
    fire("combat", p);
}
void Engine::on_combat_leave() {
    Params p; p["type"] = "脱离战斗";
    fire("combat", p);
}
void Engine::on_die(const std::string& state) {
    Params p; p["type"] = state;
    fire("death", p);
}

void Engine::on_dispfm(const std::string& id, const std::string& rtime,
                       const std::string& distime, int64_t now) {
    Params p;
    p["skill_id"] = id;
    p["id"] = id;
    p["rtime"] = rtime;
    p["distime"] = distime;
    fire("skill_cast", p);

    // 冷却跟踪 + 调度"skill_cd"
    double cd = parse_float(distime);
    int64_t cd_ms = std::isfinite(cd) && cd > 0 ? static_cast<int64_t>(cd) : 0;
    cooldowns_[id] = now + cd_ms;
    pending_cd_.push_back({now + cd_ms, id});
}

void Engine::on_sc(const std::string& id, const std::string& hp, const std::string& mp,
                   const std::string& max_hp, const std::string& max_mp, const std::string& damage) {
    if (id.empty()) return;
    auto it = room_.find(id);
    if (it == room_.end()) return;   // 无房间缓存：事件 14/15 均不触发（与扩展一致）
    RoomItem& item = it->second;

    Params base;
    base["id"] = id;

    if (!hp.empty()) {
        std::string cmp = "低于";
        if (parse_float(hp) > item.hp) cmp = "高于";
        double old_value = item.hp;
        double old_per = item.have_max_hp && item.max_hp > 0 ? item.hp / item.max_hp * 100.0 : 0.0;
        item.hp = parse_float(hp);
        if (item.max_hp < item.hp) { item.max_hp = item.hp; item.have_max_hp = true; }
        if (!max_hp.empty()) { item.max_hp = parse_float(max_hp); item.have_max_hp = true; }
        double new_per = item.have_max_hp && item.max_hp > 0 ? item.hp / item.max_hp * 100.0 : 0.0;
        Params p1 = base, p2 = base;
        p1["name_keyword"] = item.name; p2["name_keyword"] = item.name;
        p1["type"] = "气血"; p2["type"] = "气血";
        p1["when"] = cmp; p2["when"] = cmp;
        p1["value_type"] = "百分比"; p2["value_type"] = "数值";
        p1["value"] = fmt2(old_per) + ";" + fmt2(new_per);
        p2["value"] = num_str(old_value) + ";" + num_str(item.hp);
        p1["hp"] = num_str(item.hp); p2["hp"] = num_str(item.hp);
        p1["maxHp"] = num_str(item.max_hp); p2["maxHp"] = num_str(item.max_hp);
        p1["mp"] = num_str(item.mp); p2["mp"] = num_str(item.mp);
        p1["maxMp"] = num_str(item.max_mp); p2["maxMp"] = num_str(item.max_mp);
        fire("hp_mp", p1);
        fire("hp_mp", p2);
    }

    if (!mp.empty()) {
        std::string cmp = "低于";
        if (parse_float(mp) > item.mp) cmp = "高于";
        double old_value = item.mp;
        double old_per = item.have_max_mp && item.max_mp > 0 ? item.mp / item.max_mp * 100.0 : 0.0;
        item.mp = parse_float(mp);
        if (item.max_mp < item.mp) { item.max_mp = item.mp; item.have_max_mp = true; }
        if (!max_mp.empty()) { item.max_mp = parse_float(max_mp); item.have_max_mp = true; }
        double new_per = item.have_max_mp && item.max_mp > 0 ? item.mp / item.max_mp * 100.0 : 0.0;
        Params p1 = base, p2 = base;
        p1["name_keyword"] = item.name; p2["name_keyword"] = item.name;
        p1["type"] = "内力"; p2["type"] = "内力";
        p1["when"] = cmp; p2["when"] = cmp;
        p1["value_type"] = "百分比"; p2["value_type"] = "数值";
        p1["value"] = fmt2(old_per) + ";" + fmt2(new_per);
        p2["value"] = num_str(old_value) + ";" + num_str(item.mp);
        p1["hp"] = num_str(item.hp); p2["hp"] = num_str(item.hp);
        p1["maxHp"] = num_str(item.max_hp); p2["maxHp"] = num_str(item.max_hp);
        p1["mp"] = num_str(item.mp); p2["mp"] = num_str(item.mp);
        p1["maxMp"] = num_str(item.max_mp); p2["maxMp"] = num_str(item.max_mp);
        fire("hp_mp", p1);
        fire("hp_mp", p2);
    }

    if (!damage.empty()) {
        if (!item.have_max_hp) return;   // max_hp 未知 → 不触发（与扩展一致）
        double old_value = item.have_damage ? item.damage : 0.0;
        double old_per = item.have_damage ? item.damagePer : 0.0;
        double value = parse_float(damage);
        double percent = item.max_hp > 0 ? value / item.max_hp * 100.0 : 0.0;
        item.damage = value;
        item.damagePer = percent;
        item.have_damage = true;
        Params p1 = base, p2 = base;
        p1["name_keyword"] = item.name; p2["name_keyword"] = item.name;
        p1["value_type"] = "百分比"; p2["value_type"] = "数值";
        p1["value"] = fmt2(old_per) + ";" + fmt2(percent);
        p2["value"] = num_str(old_value) + ";" + num_str(value);
        p1["name"] = item.name; p2["name"] = item.name;
        p1["value"] = num_str(value); p2["value"] = num_str(value);
        p1["percent"] = fmt2(percent); p2["percent"] = fmt2(percent);
        fire("damage_full", p1);
        fire("damage_full", p2);
    }
}

void Engine::on_items(const std::vector<RoomItemData>& items) {
    room_.clear();
    for (const auto& it : items) {
        if (it.id.empty()) continue;
        RoomItem r;
        r.name = it.name;
        r.hp = it.hp; r.mp = it.mp;
        r.max_hp = it.max_hp; r.max_mp = it.max_mp;
        r.have_max_hp = it.have_max_hp; r.have_max_mp = it.have_max_mp;
        room_[it.id] = r;
    }
}
void Engine::on_room_clear() { room_.clear(); }

void Engine::tick(int64_t now) {
    // 技能冷却结束（到期逐个触发）
    for (std::size_t i = 0; i < pending_cd_.size();) {
        if (now >= pending_cd_[i].due) {
            Params p;
            p["skill_id"] = pending_cd_[i].sid;
            p["id"] = pending_cd_[i].sid;
            fire("skill_cd", p);
            pending_cd_.erase(pending_cd_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    // 清理过期冷却记录
    for (auto it = cooldowns_.begin(); it != cooldowns_.end();) {
        if (now >= it->second) it = cooldowns_.erase(it);
        else ++it;
    }
    // 时辰已到：本地时钟整秒边界
    time_t t = std::time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    int sec = tm_buf.tm_hour * 3600 + tm_buf.tm_min * 60 + tm_buf.tm_sec;
    if (sec != last_clock_sec_) {
        last_clock_sec_ = sec;
        Params p;
        p["hour"] = num_str(tm_buf.tm_hour);
        p["minute"] = num_str(tm_buf.tm_min);
        p["second"] = num_str(tm_buf.tm_sec);
        fire("time_reached", p);
    }
    // 推进 Raid 流程
    js_tick();
}

void Engine::feed_text(const std::string& text) { js_feed(text); }

// ============================================================
// 脚本宿主（__cxx）
// ============================================================

void Engine::emit_log(const char* s) { if (log_ && s) log_(log_ud_, s); }
void Engine::emit_send(const std::string& cmd) { if (send_) send_(send_ud_, cmd); }
std::string Engine::host_state() { return state_ ? state_(state_ud_) : "{}"; }

std::string Engine::host_room_item_id(const std::string& name, bool exact) {
    for (const auto& kv : room_) {
        if (exact ? kv.second.name == name : kv.second.name.find(name) != std::string::npos)
            return kv.first;
    }
    return "";
}
bool Engine::host_room_has_item(const std::string& name, bool exact) {
    return !host_room_item_id(name, exact).empty();
}
int64_t Engine::host_cooling_until(const std::string& sid) {
    auto it = cooldowns_.find(sid);
    return it == cooldowns_.end() ? 0 : it->second;
}
void Engine::host_set_trigger(const std::string& name, bool active) {
    for (auto& t : triggers_) {
        if (t.name == name) {
            t.active = active;
            save();
            return;
        }
    }
}
int64_t Engine::host_now() { return mono_ms(); }

// ============================================================
// 导入 / 开关 / 持久化
// ============================================================

bool Engine::import(const std::string& data_json, std::string& err) {
    std::string perr;
    json::Value v = json::parse(data_json, &perr);
    if (!perr.empty() || !v.is_object()) { err = "分享码数据解析失败"; return false; }
    std::string name = v.get("name").as_string();
    if (!name_ok(name)) { err = "触发器的名称只能使用中文、英文和数字字符。"; return false; }
    for (const auto& t : triggers_)
        if (t.name == name) { err = "无法修改名称，已经存在同名触发器！"; return false; }
    std::string event = v.get("event").as_string();
    if (event.empty()) { err = "触发器缺少事件类型"; return false; }
    Trigger tg;
    tg.name = name;
    tg.event = event;
    tg.active = v.get("active").as_bool(false);
    tg.author = v.get("author").as_string();
    const json::Value& conds = v.get("conditions");
    if (conds.is_object()) {
        for (const auto& kv : conds.as_object())
            tg.conditions[kv.first] = val_str(kv.second);
    }
    tg.source = v.get("source").as_string();
    if (tg.source.empty()) { err = "触发器缺少脚本源码"; return false; }
    triggers_.push_back(std::move(tg));
    save();
    return true;
}

bool Engine::enable(std::size_t idx, std::string& err) {
    if (idx < 1 || idx > triggers_.size()) { err = "触发器编号无效（1-" + num_str(triggers_.size()) + "）"; return false; }
    Trigger& t = triggers_[idx - 1];
    if (t.active) { err = "触发器已处于启用状态"; return false; }
    t.active = true;
    save();
    return true;
}
bool Engine::disable(std::size_t idx, std::string& err) {
    if (idx < 1 || idx > triggers_.size()) { err = "触发器编号无效（1-" + num_str(triggers_.size()) + "）"; return false; }
    Trigger& t = triggers_[idx - 1];
    if (!t.active) { err = "触发器已处于停用状态"; return false; }
    t.active = false;
    save();
    return true;
}

// 编辑保存：名称/事件/条件/源码/启用 全量替换（idx 0-based）
bool Engine::update(std::size_t idx, const std::string& name, const std::string& event,
                    const std::vector<std::pair<std::string, std::string>>& conds,
                    const std::string& source, bool active, std::string& err) {
    if (idx >= triggers_.size()) { err = "触发器编号无效"; return false; }
    if (!name_ok(name)) { err = "触发器的名称只能使用中文、英文和数字字符。"; return false; }
    for (std::size_t i = 0; i < triggers_.size(); ++i)
        if (i != idx && triggers_[i].name == name) { err = "无法修改名称，已经存在同名触发器！"; return false; }
    const EventDef* ev = find_event(event);
    if (!ev) { err = "未知事件类型"; return false; }
    Trigger& t = triggers_[idx];
    t.name = name;
    t.event = event;
    t.active = active;
    t.conditions.clear();
    for (const auto& kv : conds)
        if (!kv.second.empty()) t.conditions[kv.first] = kv.second;  // 空值条件 = 仅键存在（Key 断言）
    t.source = source;
    save();
    return true;
}

// 创建：末尾追加（idx 参数校验同上）
bool Engine::add(const std::string& name, const std::string& event,
                 const std::vector<std::pair<std::string, std::string>>& conds,
                 const std::string& source, bool active, std::string& err) {
    if (!name_ok(name)) { err = "触发器的名称只能使用中文、英文和数字字符。"; return false; }
    for (const auto& t : triggers_)
        if (t.name == name) { err = "无法修改名称，已经存在同名触发器！"; return false; }
    const EventDef* ev = find_event(event);
    if (!ev) { err = "未知事件类型"; return false; }
    Trigger tg;
    tg.name = name;
    tg.event = event;
    tg.active = active;
    for (const auto& kv : conds)
        if (!kv.second.empty()) tg.conditions[kv.first] = kv.second;
    tg.source = source;
    triggers_.push_back(std::move(tg));
    save();
    return true;
}

bool Engine::remove(std::size_t idx, std::string& err) {
    if (idx >= triggers_.size()) { err = "触发器编号无效"; return false; }
    triggers_.erase(triggers_.begin() + static_cast<std::ptrdiff_t>(idx));
    save();
    return true;
}

std::vector<std::string> Engine::event_names() {
    std::vector<std::string> out;
    for (int i = 0; i < N_EVENTS; ++i) out.push_back(EVENTS[i].name);
    return out;
}
std::vector<std::pair<std::string, std::string>> Engine::event_filters(const std::string& event) {
    std::vector<std::pair<std::string, std::string>> out;
    const EventDef* ev = find_event(event);
    if (!ev) return out;
    for (int i = 0; i < ev->nfilters; ++i)
        out.emplace_back(ev->filters[i].name, filter_type_name(ev->filters[i].type));
    return out;
}

// 该事件的条件键表单字段（含中文说明、值类型、枚举候选）。供 TUI 编辑屏驱动。
std::vector<Engine::CondField> Engine::cond_fields(const std::string& event) {
    std::vector<CondField> out;
    const EventDef* ev = find_event(event);
    if (!ev) return out;
    for (int i = 0; i < ev->nfilters; ++i) {
        CondField f;
        f.key = ev->filters[i].name;
        f.label = cond_label_name(f.key);
        f.type = is_int_key(f.key) ? "int" : "text";
        auto opts = cond_options(event, f.key);
        if (!opts.empty()) { f.type = "enum"; f.options = std::move(opts); }
        out.push_back(std::move(f));
    }
    return out;
}

const char* Engine::event_label(const std::string& event) { return event_label_name(event); }

void Engine::load() {
    if (save_path_.empty()) return;
    FILE* fp = std::fopen(save_path_.c_str(), "rb");
    if (!fp) return;
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    std::fclose(fp);
    std::string err;
    json::Value v = json::parse(text, &err);
    if (!err.empty() || !v.is_array()) return;
    triggers_.clear();
    for (const auto& it : v.as_array()) {
        if (!it.is_object()) continue;
        Trigger t;
        t.name = it.get("name").as_string();
        if (t.name.empty()) continue;
        t.event = it.get("event").as_string();
        t.source = it.get("source").as_string();
        t.active = it.get("active").as_bool(false);
        t.author = it.get("author").as_string();
        const json::Value& conds = it.get("conditions");
        if (conds.is_object())
            for (const auto& kv : conds.as_object())
                t.conditions[kv.first] = val_str(kv.second);
        // 旧数据兼容（扩展行为）：chat 事件补 ignore_speaker 字段
        if (t.event == "chat" && t.conditions.find("ignore_speaker") == t.conditions.end())
            t.conditions["ignore_speaker"] = "";
        triggers_.push_back(std::move(t));
    }
}

void Engine::save() {
    // no-op：程序只读不写，触发器统一由软件同级目录 trigger.json 维护。
    // 保留此接口仅为兼容旧调用（如脚本 @setTrigger → host_set_trigger → save）。
}

// 从 trigger.json 加载：顶层对象 {version, triggers:[]}，逐项校验后写入 out。
// source 兼容字符串（旧格式/分享码，\n 已解码）与字符串数组（新格式，逐元素 join \n）。
// 校验失败返回 false + err，out 保持不变（由调用方决定是否覆盖）。
bool Engine::load_from_file(const std::string& path, std::vector<Trigger>& out, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { err = "无法打开 " + path; return false; }
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    std::fclose(fp);

    json::Value v = json::parse(text, &err);
    if (!err.empty()) return false;
    if (!v.is_object()) { err = "顶层应为对象（{\"version\":1,\"triggers\":[]}）"; return false; }
    const json::Value& arr = v.get("triggers");
    if (!arr.is_array()) { err = "缺少 triggers 数组"; return false; }

    std::vector<Trigger> parsed;
    for (const auto& it : arr.as_array()) {
        if (!it.is_object()) { err = "triggers 内存在非对象条目"; return false; }
        Trigger t;
        t.name = it.get("name").as_string();
        if (!name_ok(t.name)) { err = "触发器名称非法（只能使用中文、英文和数字字符）"; return false; }
        for (const auto& ot : parsed)
            if (ot.owner == t.owner && ot.name == t.name) { err = "存在同名触发器「" + t.name + "」"; return false; }
        t.event = it.get("event").as_string();
        if (!find_event(t.event)) { err = "触发器「" + t.name + "」的事件类型未知：" + t.event; return false; }
        t.active = it.get("active").as_bool(false);
        t.author = it.get("author").as_string();
        t.owner = it.get("owner").as_string();   // 空串=全局共享；否则归属该玩家
        const json::Value& conds = it.get("conditions");
        if (conds.is_object()) {
            for (const auto& kv : conds.as_object()) {
                // 条件键必须属于该事件模板的过滤器
                const EventDef* ev = find_event(t.event);
                bool known = false;
                for (int i = 0; ev && i < ev->nfilters; ++i)
                    if (kv.first == ev->filters[i].name) { known = true; break; }
                if (!known) { err = "触发器「" + t.name + "」的条件键「" + kv.first +
                              "」不属于事件「" + t.event + "」"; return false; }
                // 整数键只允许整数（JSON 整数或纯数字字符串），空/通配除外；其余键为字符串
                std::string sval = val_str(kv.second);
                if (is_int_key(kv.first) && !is_wild(sval) && !is_int_str(sval)) {
                    err = "触发器「" + t.name + "」的条件「" + kv.first + "」的值必须是整数";
                    return false;
                }
                t.conditions[kv.first] = is_int_key(kv.first) ? norm_int_str(sval) : sval;
            }
        }
        const json::Value& src = it.get("source");
        if (src.is_string()) t.source = src.as_string();
        else if (src.is_array()) {
            for (const auto& el : src.as_array()) {
                if (!t.source.empty()) t.source += '\n';
                t.source += val_str(el);
            }
        }
        // 允许空 source（TUI 编辑时可先存骨架再补充脚本）
        parsed.push_back(std::move(t));
    }
    out = std::move(parsed);
    return true;
}

void Engine::replace(const std::vector<Trigger>& ts) {
    triggers_ = ts;
}

// 把触发器列表序列化为 trigger.json 顶层文本。
// source 统一输出为数组（一行一条）；条件对象键为 conditions（与 load_from_file 读取一致）。
std::string Engine::dump_triggers(const std::vector<Trigger>& ts) {
    json::Value::Array arr;
    for (const auto& t : ts) {
        json::Value::Object obj;
        obj["name"] = json::Value(t.name);
        obj["event"] = json::Value(t.event);
        obj["active"] = json::Value(t.active);
        obj["owner"] = json::Value(t.owner);   // 归属玩家名；空串=全局共享
        json::Value::Object conds;
        for (const auto& kv : t.conditions)
            conds[kv.first] = json::Value(kv.second);
        obj["conditions"] = json::Value(std::move(conds));
        json::Value::Array src;
        std::string line;
        for (char c : t.source) {
            if (c == '\n') { src.push_back(json::Value(std::move(line))); line.clear(); }
            else line += c;
        }
        src.push_back(json::Value(std::move(line)));
        obj["source"] = json::Value(std::move(src));
        arr.push_back(json::Value(std::move(obj)));
    }
    json::Value::Object top;
    top["version"] = json::Value(1);
    top["triggers"] = json::Value(std::move(arr));
    return json::dump_pretty(json::Value(std::move(top)));
}

}  // namespace trigger
}  // namespace wsmud
