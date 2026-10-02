#include "mud.hpp"

#include <cmath>    // std::isfinite：double→整数转换前的范围守卫
#include <cstdio>
#include <future>   // std::promise：stop_trig_worker 同步等待 release 闭包执行
#include <memory>   // std::make_shared

namespace wsmud {
namespace mud {

namespace {

// 心跳保活间隔（毫秒）。浏览器本身不发送应用层心跳，
// 此 ping 仅用于协议层防中间设备（如云 SLB）空闲断连，不影响游戏状态。
constexpr int64_t PING_INTERVAL_MS = 30000;
// 各阶段超时
constexpr int64_t HTTP_TIMEOUT_MS = 15000;
constexpr int64_t WS_AUTH_TIMEOUT_MS = 12000;

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// double → int64：仅在可表示范围内转换（[conv.fpint] 规定超范围转换是 UB），成功返回 true
bool double_to_i64(double d, int64_t& out) {
    if (!std::isfinite(d)) return false;
    if (d < -9223372036854775808.0 || d >= 9223372036854775808.0) return false;
    out = static_cast<int64_t>(d);
    return true;
}

// 房间出口对象 → "南：青草坪；东：木板路"（按常见方位排序；未知方向键保留原样排最后）
std::string format_exits(const json::Value& items) {
    static const struct { const char* key; const char* label; } DIRS[] = {
        {"north", "北"}, {"south", "南"}, {"east", "东"}, {"west", "西"},
        {"up", "上"}, {"down", "下"}, {"enter", "进"}, {"out", "出"},
        {"northeast", "东北"}, {"northwest", "西北"}, {"southeast", "东南"}, {"southwest", "西南"},
    };
    if (!items.is_object()) return "";
    std::string out;
    auto add = [&](const std::string& label, const std::string& to) {
        if (to.empty()) return;
        if (!out.empty()) out += "；";
        out += label + "：" + html_to_ansi(to);
    };
    for (const auto& d : DIRS) add(d.label, items.get(d.key).as_string());
    for (const auto& kv : items.as_object()) {
        bool known = false;
        for (const auto& d : DIRS) if (kv.first == d.key) { known = true; break; }
        if (!known) add(kv.first, kv.second.as_string());
    }
    return out;
}

// 自动施法：两次施法之间的最小间隔（对齐扩展智能模式的 300ms tick）
constexpr int64_t AUTO_CAST_INTERVAL_MS = 300;

// 自动施法：技能 → 它负责维持的 buff sid（非 buff 类技能返回空串）。
// 对应扩展 wg-combat-auto.js 的 buff_skill_dict；内功类（force.*）在扩展里另有
// force_buff_skill 白名单统一处理，这里归到 "force" buff（force.ztd / force.wang
// 各归其专属 buff，与扩展的 ztd / mingyu 分类一致）。
std::string buff_of_skill(const std::string& sid) {
    if (sid == "sword.wu" || sid == "blade.shi" || sid == "sword.yu") return "weapon";
    if (sid == "force.ztd") return "ztd";
    if (sid == "force.wang") return "mingyu";
    if (sid == "dodge.power" || sid == "dodge.fo" || sid == "dodge.gui" ||
        sid == "dodge.lingbo" || sid == "dodge.zhui") return "dodge";
    if (sid.rfind("force.", 0) == 0) return "force";
    return "";
}

// JSON 值 → 字符串（数字尽量整数；null/缺键 → 空串）
std::string val_str(const json::Value& v) {
    if (v.is_string()) return v.as_string();
    if (v.is_number()) {
        double d = v.as_number();
        int64_t i;
        if (double_to_i64(d, i) && d == static_cast<double>(i)) return std::to_string(i);
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.2f", d);
        return buf;
    }
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    return "";
}

}  // namespace

// 全局设置表（声明见 mud.hpp）：角色名 → 玩家设置，由 commands.cpp 读/写 settings.json
std::map<std::string, PlayerSettings> g_settings;

void Account::set_stage(Stage s, int64_t now, int64_t timeout_ms) {
    stage = s;
    stage_deadline_ms = timeout_ms > 0 ? now + timeout_ms : 0;
}

void Account::enter_disconnected(const std::string& reason) {
    ws.close();
    http.close();
    last_error = reason;
    set_stage(Stage::Disconnected, now_ms(), 0);
    // 自动施法依赖的服务器状态在断线后失效，清空以免重连后用过期的技能表/冷却
    my_skills.clear();
    my_cd_until.clear();
    my_gcd_until = 0;
    stop_trig_worker();   // 退出 worker 并释放 QuickJS 运行时（断线/失败后触发器停止）
    log("[断开] " + reason + "（输入 reconnect " + std::to_string(index) + " 重连）");
}

void Account::start_login() {
    servers.clear();
    roles.clear();
    entered = false;
    last_error.clear();
    log("获取服务器列表...");
    http.start(website + "/api/game/servers", "GET", "", now_ms());
    set_stage(Stage::WaitServers, now_ms(), HTTP_TIMEOUT_MS);
}

void Account::select_server(int idx) {
    if (idx < 0 || idx >= static_cast<int>(servers.size())) {
        log("服务器序号无效，请重新输入");
        return;
    }
    server = servers[idx];
    need = Need::None;
    prompt.clear();
    log("登录账号 " + account + " ...");
    std::string body = "{\"code\":" + json::dump(json::Value(account)) +
                       ",\"pwd\":" + json::dump(json::Value(password)) + "}";
    http.start(website + "/api/user/login", "POST", body, now_ms());
    set_stage(Stage::WaitLogin, now_ms(), HTTP_TIMEOUT_MS);
}

void Account::select_role(int idx) {
    if (idx < 0 || idx >= static_cast<int>(roles.size())) {
        log("角色序号无效，请重新输入");
        return;
    }
    role = roles[idx];
    need = Need::None;
    prompt.clear();
    init_triggers();
    log("进入游戏（角色: " + role.name + "）...");
    // 浏览器行为（ws.js）：保持同一连接，发送 "login {roleId}" 进入游戏
    ws.send_text("login " + role.id);
    set_stage(Stage::Entering, now_ms(), WS_AUTH_TIMEOUT_MS);
}

void Account::on_ws_open(int64_t now) {
    if (stage == Stage::AuthWs) {
        // 认证："{sessionKey} {sessionToken}"，获取角色列表
        ws.send_text(session_key + " " + session_token);
        set_stage(Stage::WaitRoles, now, WS_AUTH_TIMEOUT_MS);
    }
}

void Account::tick(int64_t now) {
    switch (stage) {
        case Stage::None:
        case Stage::NeedSelectServer:
        case Stage::NeedSelectRole:
            return;

        case Stage::WaitServers:
        case Stage::WaitLogin: {
            if (http.tick(now)) {
                if (http.state == net::HttpReq::State::Failed) {
                    last_error = http.error;
                    set_stage(Stage::Disconnected, now, 0);
                    log("[失败] " + http.error);
                    return;
                }
                if (stage == Stage::WaitServers) {
                    std::string err;
                    json::Value v = json::parse(http.body, &err);
                    if (!err.empty() || !v.is_array()) {
                        enter_disconnected("服务器列表解析失败: " + (err.empty() ? "格式错误" : err));
                        return;
                    }
                    servers.clear();
                    for (const auto& item : v.as_array()) {
                        GameServer s;
                        s.id = static_cast<int>(item.get("id").as_int());
                        s.name = item.get("name").as_string();
                        s.ip = item.get("ip").as_string();
                        s.port = item.get("port").as_string();
                        if (!s.ip.empty() && !s.port.empty()) servers.push_back(s);
                    }
                    if (servers.empty()) { enter_disconnected("网站未返回可用服务器"); return; }
                    std::string list;
                    for (std::size_t i = 0; i < servers.size(); ++i)
                        list += "\n  [" + std::to_string(i + 1) + "] " + servers[i].name +
                                " (id=" + std::to_string(servers[i].id) + ", " + servers[i].ip + ":" + servers[i].port + ")";
                    log("服务器列表:" + list);
                    need = Need::Server;
                    prompt = "账号" + std::to_string(index) + " 请选择服务器 [1-" +
                             std::to_string(servers.size()) + "]：";
                    set_stage(Stage::NeedSelectServer, now, 0);
                } else {  // WaitLogin
                    std::string err;
                    json::Value v = json::parse(http.body, &err);
                    if (!err.empty()) {
                        enter_disconnected("登录响应解析失败: " + err);
                        return;
                    }
                    if (v.get("code").as_int() != 1) {
                        enter_disconnected("登录失败: " + v.get("result").as_string("未知错误"));
                        return;
                    }
                    session_key = v.get("u").as_string();
                    session_token = v.get("p").as_string();
                    if (session_key.empty() || session_token.empty()) {
                        enter_disconnected("登录响应缺少会话凭据");
                        return;
                    }
                    log("登录成功，连接游戏服务器...");
                    ws.connect(server.ip, server.port, website, "/", now);
                    set_stage(Stage::AuthWs, now, WS_AUTH_TIMEOUT_MS);
                }
            }
            return;
        }

        case Stage::AuthWs:
        case Stage::EnterWs: {
            if (ws.tick(now)) {
                if (!ws.error.empty()) { enter_disconnected("连接失败: " + ws.error); return; }
                return;
            }
            if (ws.open()) on_ws_open(now);
            return;
        }

        case Stage::WaitRoles:
        case Stage::Entering: {
            if (now > stage_deadline_ms) {
                enter_disconnected(stage == Stage::WaitRoles ? "等待角色列表超时" : "进入游戏超时");
                return;
            }
            std::vector<std::string> msgs;
            if (!ws.drain(msgs)) {
                enter_disconnected(ws.error.empty() ? "连接异常" : ws.error);
                return;
            }
            if (ws.state == ws::WsClient::State::Closed) {
                enter_disconnected(ws.error.empty() ? "连接被关闭" : ws.error);
                return;
            }
            for (auto& m : msgs) on_message(m);
            return;
        }

        case Stage::Online: {
            std::vector<std::string> msgs;
            if (!ws.drain(msgs)) {
                enter_disconnected(ws.error.empty() ? "连接异常" : ws.error);
                return;
            }
            if (ws.state == ws::WsClient::State::Closed) {
                enter_disconnected(ws.error.empty() ? "连接被关闭" : ws.error);
                return;
            }
            for (auto& m : msgs) on_message(m);
            update_idle(now);
            auto_perform_tick(now);
            auto_marry_tick(now);
            // 触发器心跳：合并——绑定 worker 队列里已有待执行 tick 则跳过本帧，避免重脚本时积压
            if (WorkerPool::instance().qempty(worker_))
                enqueue_trig([this, now] { trig.tick(now); });
            if (now - last_ping_ms >= PING_INTERVAL_MS) {
                ws.send_ping();
                last_ping_ms = now;
            }
            return;
        }

        case Stage::Disconnected:
            return;
    }
}

void Account::on_message(const std::string& text) {
    if (text.empty()) return;
    char c = text[0];
    if (c == '{' || c == '[') {
        std::string err;
        json::Value v = json::parse(text, &err);
        if (!err.empty()) {
            log("[文本] " + text);
            return;
        }
        on_json(v, text);
    } else {
        log("[文本] " + text);
    }
}

void Account::on_json(const json::Value& v, const std::string& raw) {
    const std::string type = v.get("type").as_string();

    if (type == "roles") {
        if (stage == Stage::WaitRoles) {
            roles.clear();
            for (const auto& r : v.get("roles").as_array()) {
                Role role2;
                // id 可能是字符串或数字，保留原始形式发送给 login 命令
                const json::Value& rid = r.get("id");
                role2.id = rid.is_string() ? rid.as_string() : std::to_string(rid.as_int());
                role2.name = r.get("name").as_string();
                role2.title = r.get("title").as_string();
                role2.level = static_cast<int>(r.get("level").as_int());
                if (!role2.name.empty() || !role2.id.empty()) roles.push_back(role2);
            }
            if (roles.empty()) {
                enter_disconnected("账号没有可用角色");
                return;
            }
            std::string list;
            for (std::size_t i = 0; i < roles.size(); ++i)
                list += "\n  [" + std::to_string(i + 1) + "] " + roles[i].name + " (" + roles[i].title + ")";
            log("角色列表:" + list);
            need = Need::Role;
            prompt = "账号" + std::to_string(index) + " 请选择角色 [1-" + std::to_string(roles.size()) + "]：";
            set_stage(Stage::NeedSelectRole, now_ms(), 0);
            return;
        }
        // Entering 阶段收到 roles：服务器走旧式流程，回退发送 "login {roleId}"
        if (stage == Stage::Entering) {
            log("服务器返回角色列表，改用 login 指令进入");
            ws.send_text("login " + role.id);
            return;
        }
        return;
    }

    if (type == "login") {
        if (stage == Stage::Entering || stage == Stage::WaitRoles) {
            role.level = static_cast<int>(v.get("level").as_int());
            entered = true;
            last_error.clear();
            set_stage(Stage::Online, now_ms(), 0);
            last_ping_ms = now_ms();
            log("进入游戏成功！玩家: " + role.name + " Lv." + std::to_string(role.level));
            return;
        }
        return;
    }

    if (type == "loginerror") {
        enter_disconnected("认证失败: " + v.get("msg").as_string("未知错误"));
        return;
    }

    if (type == "levelup") {
        role.level = static_cast<int>(v.get("level").as_int());
        my_level = role.level;
        log("[升级] 当前等级 Lv." + std::to_string(role.level));
        return;
    }

    // ---------- 触发器事件分发（仅在线；保留下方原有日志打印） ----------
    if (stage == Stage::Online) {
        if (type == "text") {
            const std::string msg = v.get("msg").as_string();
            // 文本启发 → 脱离战斗（与扩展 raid-role 一致）— 主线程先改状态，快照随事件下发
            if (msg.find("只能在战斗中使用") != std::string::npos ||
                msg.find("这里不允许战斗") != std::string::npos ||
                msg.find("没时间这么做") != std::string::npos) {
                my_combat = false; update_idle(now_ms());
            }
            publish_state();
            enqueue_trig([this, msg] { trig.on_text(msg); });
        } else if (type == "msg") {
            const std::string ch = v.get("ch").as_string(), name = v.get("name").as_string();
            const std::string uid = v.get("uid").as_string(), content = v.get("content").as_string();
            enqueue_trig([this, ch, name, uid, content] {
                trig.on_chat(ch, name, uid, content);
            });
        } else if (type == "status") {
            trig_on_status(v);
        } else if (type == "combat") {
            const auto& start = v.get("start");
            const auto& end = v.get("end");
            if (!start.is_null() && start.as_int() == 1) {
                my_combat = true; update_idle(now_ms());
                publish_state();
                enqueue_trig([this] { trig.on_combat_enter(); });
            } else if (!end.is_null() && end.as_int() == 1) {
                my_combat = false; update_idle(now_ms());
                publish_state();
                enqueue_trig([this] { trig.on_combat_leave(); });
            }
        } else if (type == "die") {
            const auto& relive = v.get("relive");
            bool revived = !relive.is_null() && (!relive.is_bool() || relive.as_bool());
            my_living = revived;
            update_idle(now_ms());
            publish_state();
            const std::string wdy = revived ? "已经复活" : "已经死亡";
            enqueue_trig([this, wdy] { trig.on_die(wdy); });
        } else if (type == "dispfm") {
            // rtime/distime 服务器下发为数字，必须用 val_str 而非 as_string（后者对数字返回空串，
            // 会导致冷却时长恒为 0、冷却表立即过期，(:cd) 恒判 false）
            const std::string rtime = val_str(v.get("rtime"));
            const std::string distime = val_str(v.get("distime"));
            // 自动施法：主线程记录技能冷却与公共冷却的绝对到期时刻（与扩展 WG.cds / WG.gcd 一致）
            const int64_t t = now_ms();
            int64_t r_ms = 0, d_ms = 0;
            if (double_to_i64(v.get("rtime").as_number(), r_ms) && r_ms > 0)
                my_gcd_until = t + r_ms;
            const std::string pfm_id = v.get("id").as_string();
            if (!pfm_id.empty() && double_to_i64(v.get("distime").as_number(), d_ms) && d_ms > 0)
                my_cd_until[pfm_id] = t + d_ms;
            enqueue_trig([this, v, rtime, distime] {
                trig.on_dispfm(v.get("id").as_string(), rtime, distime, now_ms());
            });
        } else if (type == "clearDistime") {
            // 服务器清空所有技能冷却（扩展 clearDistime 分支）
            my_cd_until.clear();
            my_gcd_until = 0;
        } else if (type == "enapfm") {
            // 某技能冷却提前结束（扩展 enapfm 分支）
            const std::string id = v.get("id").as_string();
            if (!id.empty()) my_cd_until.erase(id);
        } else if (type == "perform") {
            // 可释放技能列表：自动施法据此按顺序轮换。元素可能是对象 {id:...} 或裸字符串
            my_skills.clear();
            const auto& arr = v.get("skills");
            if (arr.is_array())
                for (const auto& s : arr.as_array()) {
                    const std::string id = s.is_string() ? s.as_string() : val_str(s.get("id"));
                    if (!id.empty()) my_skills.push_back(id);
                }
            publish_state();
        } else if (type == "sc") {
            trig_on_sc(v);
        } else if (type == "items") {
            trig_on_items(v);
            room_set_people(v.get("items"));
        } else if (type == "itemadd") {
            trig_on_itemadd(v);
            room_add_person(v);
        } else if (type == "itemremove") {
            const std::string id = v.get("id").as_string();
            room_remove_person(id);
            enqueue_trig([this, id] { trig.on_itemremove(id); });
        } else if (type == "state") {
            my_state_text = state_word(v.get("state").as_string());
            update_idle(now_ms());
        } else if (type == "room") {
            my_room_name = v.get("name").as_string();
            // 换房间：出口/人物由随后的 exits/items 包重建，先清空避免残留上一个房间的信息
            my_room_exits.clear();
            my_room_people.clear();
            publish_state();
            enqueue_trig([this] { trig.on_room_clear(); });
        } else if (type == "exits") {
            my_room_exits = format_exits(v.get("items"));
        }
    }

    // 分类显示：聊天消息 → 聊天区；文本提示 → 文本区；其余网络包不显示（仅供触发器消费）
    if (type == "msg") {
        const std::string ch = v.get("ch").as_string();
        const std::string name = v.get("name").as_string();
        const std::string content = v.get("content").as_string();
        std::string chn = ch;
        if (ch == "chat") chn = "世界";
        else if (ch == "tm") chn = "队伍";
        else if (ch == "fam") chn = "门派";
        else if (ch == "es") chn = "全区";
        else if (ch == "pty") chn = "帮派";
        else if (ch == "rumor") chn = "谣言";
        else if (ch == "sys") chn = "系统";
        log_chat("[" + chn + "] " + name + "：" + content);
        return;
    }
    if (type == "text") { log("[文本] " + v.get("msg").as_string()); return; }
    if (type == "warn") { log("[提示] " + v.get("msg").as_string()); return; }
    // 其余网络包不显示；dialog 仅转发给触发器
    const std::string dialog = v.get("dialog").as_string();
    if (!dialog.empty() && stage == Stage::Online) {
        if (dialog == "pack") trig_on_pack(v);
        else if (dialog == "message") trig_on_social(v);
        else if (dialog == "pm") trig_on_pm(v);
        else if (dialog == "events") { trig_on_events(v); auto_marry_on_events(v); }
    }
    // 已被右侧"房间"区消费的包（room/exits/items/itemadd/itemremove）不再记入网络包栏，避免污染
    if (type == "room" || type == "exits" || type == "items" ||
        type == "itemadd" || type == "itemremove") {
        return;
    }
    // 走到这里说明该包未显示在聊天区/文本区：status/combat/die/dispfm/sc/state、
    // dialog 包及未知类型 → 记入右侧"网络包"栏
    log_pkt(raw);
}

bool Account::send_command(const std::string& cmd) {
    if (stage != Stage::Online) { log("[提示] 当前不在线，无法发送命令"); return false; }
    std::string c = trim(cmd);
    if (c.empty()) return false;
    log("→ " + c);
    ws.send_text(c);
    return true;
}

void Account::reconnect() {
    if (stage == Stage::None) return;
    log("手动重连...");
    if (server.id == 0) {
        start_login();  // 尚未选过服务器，重新走完整流程
        return;
    }
    // 沿用已选服务器，重新登录并进游戏
    roles.clear();
    entered = false;
    ws.close();
    http.close();
    last_error.clear();
    std::string body = "{\"code\":" + json::dump(json::Value(account)) +
                       ",\"pwd\":" + json::dump(json::Value(password)) + "}";
    http.start(website + "/api/user/login", "POST", body, now_ms());
    set_stage(Stage::WaitLogin, now_ms(), HTTP_TIMEOUT_MS);
}

void Account::disconnect() {
    ws.close();
    http.close();
    set_stage(Stage::Disconnected, now_ms(), 0);
    last_error = "用户主动断开";
    stop_trig_worker();   // 主动断开/quit 同样停止 worker
    log("[断开] 用户主动断开");
}

// ---------- 触发器引擎集成 ----------

void Account::init_triggers() {
    trig.set_role_id(role.id);
    trig.set_log(trig_log_fn, this);
    trig.set_send(trig_send_fn, this);
    trig.set_state(trig_state_fn, this);
    // 触发器配置统一由软件同级目录 trigger.json 维护（程序只读），
    // 已在程序启动/新建标签页/reloadTrigger 时注入本引擎，这里不重复加载。
    // 重置角色状态
    my_name = role.name;
    my_level = role.level;
    my_state_text = "发呆";
    // 按角色名应用本玩家的持久化设置（settings.json；无记录则保持默认关闭）
    {
        auto it = g_settings.find(my_name);
        if (it != g_settings.end()) {
            auto_perform = it->second.auto_perform;
            auto_marry = it->second.auto_marry;
        } else {
            auto_perform = false;
            auto_marry = false;
        }
        my_cast_next_ms = 0;
        marry_step_ms = 0;
    }
    my_room_name.clear();
    my_hp = my_max_hp = my_mp = my_max_mp = 0;
    my_living = true;
    my_combat = false;
    my_idle = false;
    my_idle_start_ms = 0;
    my_status.clear();
    sync_triggers();   // 玩家名已确定，按 owner 过滤加载该玩家的触发器
    publish_state();   // 初始化快照，worker 首次读取即有完整状态
    start_trig_worker();   // 进游戏即拉起触发器 worker（此后 trig 事件/心跳全部走 worker 队列）
    log("[触发] 触发器引擎就绪");
}

// 注入全局触发器全集（未过滤），并按当前玩家过滤同步到引擎
void Account::set_triggers_all(const std::vector<trigger::Trigger>& all) {
    all_triggers_ = all;
    sync_triggers();
}

// 按归属玩家过滤：引擎执行 owner==my_name 的私有触发器 + owner 空的全局共享触发器
void Account::sync_triggers() {
    std::vector<trigger::Trigger> mine;
    for (const auto& t : all_triggers_)
        if (t.owner == my_name || t.owner.empty()) mine.push_back(t);
    trig.replace(mine);
}

void Account::update_idle(int64_t now) {
    // 发呆且不在战斗（与扩展 Role._syncIdle 一致）
    bool idle = my_state_text == "发呆" && !my_combat;
    if (idle && my_idle_start_ms == 0) my_idle_start_ms = now;
    if (!idle) my_idle_start_ms = 0;
    my_idle = idle;
}

bool Account::is_free() const {
    // 与扩展 WG.is_free 一致：busy/faint/rash/bss 任一在场都不可施法
    return !my_status.count("busy") && !my_status.count("faint") &&
           !my_status.count("rash") && !my_status.count("bss");
}

void Account::set_auto_perform(bool on) {
    auto_perform = on;
    my_cast_next_ms = 0;
    log(std::string("自动施法") + (on ? "开启（智能模式）" : "关闭"));
}

void Account::set_auto_marry(bool on) {
    auto_marry = on;
    marry_step_ms = 0;
    log(std::string("自动喜宴") + (on ? "开启" : "关闭"));
}

void Account::auto_perform_tick(int64_t now) {
    if (!auto_perform || !my_combat || !my_living) return;  // 仅战斗中自动施法
    if (now < my_cast_next_ms) return;                      // 施法节流
    if (now < my_gcd_until) return;                         // 公共冷却中
    if (!is_free()) return;                                 // 疗伤/晕/忙等状态中不施法

    // 第一阶段：补 buff —— 按技能列表顺序，找"该技能负责且当前缺失"的 buff。
    // force.tuoli（脱力）不能主动释放，与扩展的固定黑名单一致。
    for (const auto& sid : my_skills) {
        if (sid == "force.tuoli") continue;
        const std::string buff = buff_of_skill(sid);
        if (buff.empty() || my_status.count(buff)) continue;
        auto it = my_cd_until.find(sid);
        if (it != my_cd_until.end() && it->second > now) continue;   // 技能冷却中
        ws.send_text("perform " + sid);
        my_cast_next_ms = now + AUTO_CAST_INTERVAL_MS;
        return;
    }

    // 第二阶段：主攻 —— 列表顺序中第一个"非 buff 类且不在冷却"的技能
    for (const auto& sid : my_skills) {
        if (sid == "force.tuoli") continue;
        if (!buff_of_skill(sid).empty()) continue;
        auto it = my_cd_until.find(sid);
        if (it != my_cd_until.end() && it->second > now) continue;
        ws.send_text("perform " + sid);
        my_cast_next_ms = now + AUTO_CAST_INTERVAL_MS;
        return;
    }
}

// 自动喜宴（对齐扩展 wg-combat-extra.js 的 xiyan）：
// 发现活动列表含 "marry" 时，先发 stopstate 停止当前状态，1 秒后发 events marry ok 领取。
void Account::auto_marry_on_events(const json::Value& v) {
    if (!auto_marry || !my_living || my_combat) return;   // 与扩展一致：战斗中不自动领取
    if (marry_step_ms != 0) return;                       // 上一次领取尚未发完，避免重复
    const auto& items = v.get("items");
    if (!items.is_array()) return;
    for (const auto& item : items.as_array()) {
        if (!item.is_array()) continue;
        const auto& arr = item.as_array();
        if (arr.empty() || val_str(arr[0]) != "marry") continue;
        ws.send_text("stopstate");
        log("自动喜宴：发现喜宴活动，已发送 stopstate，1 秒后领取");
        marry_step_ms = now_ms() + 1000;
        return;
    }
}

void Account::auto_marry_tick(int64_t now) {
    if (marry_step_ms == 0 || now < marry_step_ms) return;
    marry_step_ms = 0;
    ws.send_text("events marry ok");
    log("自动喜宴：已领取喜宴（events marry ok）");
}

std::string Account::state_word(const std::string& raw) {
    static const char* words[] = {"疗伤", "打坐", "挖矿", "工作", "练习", "学习", "闭关",
                                  "炼药", "领悟", "读书", "聚魂", "推演", "采药", "钓鱼", "双修"};
    if (!raw.empty())
        for (const char* w : words)
            if (raw.find(w) != std::string::npos) return w;
    return "发呆";
}

// 主线程：把 my_*（仅主线程读写）封装成 JSON 快照缓存到 state_j_。
// worker 只读这份带锁快照，不直接触碰任何 my_* 字段，从而把跨线程共享面收敛为一个 string + mutex。
void Account::publish_state() {
    int64_t idle_time = 0;
    if (my_idle_start_ms > 0) idle_time = (now_ms() - my_idle_start_ms) / 1000;
    bool free = my_status.find("busy") == my_status.end() &&
                my_status.find("faint") == my_status.end() &&
                my_status.find("rash") == my_status.end();
    std::string s = "{";
    s += "\"hp\":" + json::dump(json::Value(my_hp));
    s += ",\"maxHp\":" + json::dump(json::Value(my_max_hp));
    s += ",\"mp\":" + json::dump(json::Value(my_mp));
    s += ",\"maxMp\":" + json::dump(json::Value(my_max_mp));
    s += ",\"state\":" + json::dump(json::Value(my_state_text));
    s += ",\"room\":" + json::dump(json::Value(my_room_name));
    s += ",\"living\":" + json::dump(json::Value(my_living));
    s += ",\"combat\":" + json::dump(json::Value(my_combat));
    s += ",\"free\":" + json::dump(json::Value(free));
    s += ",\"idle\":" + json::dump(json::Value(my_idle));
    s += ",\"idle_time\":" + json::dump(json::Value(static_cast<double>(idle_time)));
    s += ",\"level\":" + json::dump(json::Value(my_level));
    s += ",\"name\":" + json::dump(json::Value(my_name.empty() ? role.name : my_name));
    s += "}";
    std::lock_guard<std::mutex> lk(st_jm_);
    state_j_ = std::move(s);
}
std::string Account::get_state_json() {
    std::lock_guard<std::mutex> lk(st_jm_);
    return state_j_;
}

// ---------- 触发器 worker（共享线程池，账号绑定其中一条） ----------

void Account::start_trig_worker() {
    if (worker_ >= 0) return;          // 已绑定（重连时复用），保持 QuickJS 线程亲和
    main_q_.reset();                   // 清残留回传闭包
    worker_ = WorkerPool::instance().acquire();
}
void Account::stop_trig_worker() {
    if (worker_ < 0) return;
    int w = worker_;
    worker_ = -1;
    // 释放 JS 运行时必须在账号绑定的同一条 worker 上执行（QuickJS 线程亲和）。
    // 用 promise 同步等待 release 闭包跑完：队列 FIFO 保证该账号此前入队的所有事件
    // 也在此前都已执行完，随后的对象销毁不会出现 worker 悬挂 this 的竞态。
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    WorkerPool::instance().submit(w, [this, done] { trig.release(); done->set_value(); });
    WorkerPool::instance().release(w);   // 归还绑定，供其他账号复用
    fut.wait();
}
void Account::enqueue_trig(std::function<void()> fn) {
    if (worker_ < 0) return;
    WorkerPool::instance().submit(worker_, std::move(fn));
}
void Account::post_main(std::function<void()> fn) { main_q_.push(std::move(fn)); }
void Account::drain_trig_main() {
    std::function<void()> fn;
    while (main_q_.try_pop(fn)) fn();
}

// 以下三个回调均在 worker 线程被 QuickJS 调用：
// send/log 不能直接在 worker 线程触碰主线程专属的 ws/日志缓冲，改经反向队列回主线程执行；
// state 读取主线程缓存的状态快照（互斥锁保护，无跨线程直接共享字段）。
void Account::trig_log_fn(void* ud, const std::string& line) {
    Account* a = static_cast<Account*>(ud);
    if (a) a->post_main([a, line] { a->log("[触发] " + line); });
}
void Account::trig_send_fn(void* ud, const std::string& cmd) {
    Account* a = static_cast<Account*>(ud);
    if (a) a->post_main([a, cmd] { a->send_command(cmd); });
}
std::string Account::trig_state_fn(void* ud) {
    Account* a = static_cast<Account*>(ud);
    return a ? a->get_state_json() : "{}";
}

void Account::trig_on_items(const json::Value& v) {
    const auto& items = v.get("items");
    if (!items.is_array()) return;
    std::vector<trigger::RoomItemData> rids;
    for (const auto& it : items.as_array()) {
        trigger::RoomItemData d;
        d.id = it.get("id").as_string();
        d.name = it.get("name").as_string();
        d.hp = it.get("hp").as_number();
        d.mp = it.get("mp").as_number();
        d.max_hp = it.get("max_hp").as_number();
        d.max_mp = it.get("max_mp").as_number();
        d.have_max_hp = !it.get("max_hp").is_null();
        d.have_max_mp = !it.get("max_mp").is_null();
        rids.push_back(d);
        // 角色自身 hp/mp（扩展 _monitorHpMp：items 全量更新）— 主线程先行，快照随事件下发
        if (d.id == role.id) {
            my_hp = d.hp; my_max_hp = d.max_hp; my_mp = d.mp; my_max_mp = d.max_mp;
            const auto& st = it.get("status");
            if (st.is_array()) {   // 全量重建生效 buff（扩展 _monitorStatus：duration - overtime > 0）
                my_status.clear();
                for (const auto& s : st.as_array()) {
                    if (s.get("duration").as_int() > s.get("overtime").as_int())
                        my_status.insert(s.get("sid").as_string());
                }
            }
        }
    }
    publish_state();
    enqueue_trig([this, rids] { trig.on_items(rids); });
}

void Account::trig_on_itemadd(const json::Value& v) {
    const std::string id = v.get("id").as_string();
    const std::string name = v.get("name").as_string();
    if (id == role.id) {
        if (!v.get("hp").is_null()) my_hp = v.get("hp").as_number();
        if (!v.get("max_hp").is_null()) my_max_hp = v.get("max_hp").as_number();
        if (!v.get("mp").is_null()) my_mp = v.get("mp").as_number();
        if (!v.get("max_mp").is_null()) my_max_mp = v.get("max_mp").as_number();
        const auto& st = v.get("status");
        if (st.is_array()) {
            my_status.clear();
            for (const auto& s : st.as_array()) {
                if (s.get("duration").as_int() > s.get("overtime").as_int())
                    my_status.insert(s.get("sid").as_string());
            }
        }
    }
    publish_state();
    enqueue_trig([this, id, name] { trig.on_itemadd(id, name); });
}

// ---------- 房间面板数据（右侧"房间"区） ----------

void Account::room_set_people(const json::Value& arr) {
    my_room_people.clear();
    if (!arr.is_array()) return;
    for (const auto& it : arr.as_array()) {
        if (!it.is_object()) continue;                    // 数组尾部可能混入非对象项
        const std::string name = it.get("name").as_string();
        if (name.empty()) continue;
        my_room_people.emplace_back(it.get("id").as_string(), html_to_ansi(name));
    }
}

void Account::room_add_person(const json::Value& v) {
    const std::string name = v.get("name").as_string();
    if (name.empty()) return;
    const std::string id = v.get("id").as_string();
    if (!id.empty()) {
        for (const auto& p : my_room_people)
            if (p.first == id) return;                    // 已在列表中
    }
    my_room_people.emplace_back(id, html_to_ansi(name));
}

void Account::room_remove_person(const std::string& id) {
    if (id.empty()) return;
    for (auto it = my_room_people.begin(); it != my_room_people.end(); ++it) {
        if (it->first == id) { my_room_people.erase(it); return; }
    }
}

void Account::trig_on_sc(const json::Value& v) {
    const std::string id = v.get("id").as_string();
    const std::string hp = val_str(v.get("hp")), mp = val_str(v.get("mp"));
    const std::string max_hp = val_str(v.get("max_hp")), max_mp = val_str(v.get("max_mp"));
    const std::string damage = val_str(v.get("damage"));
    if (id == role.id) {   // 扩展 _monitorHpMp：sc 更新角色自身
        if (!v.get("hp").is_null()) my_hp = v.get("hp").as_number();
        if (!v.get("max_hp").is_null()) my_max_hp = v.get("max_hp").as_number();
        if (!v.get("mp").is_null()) my_mp = v.get("mp").as_number();
        if (!v.get("max_mp").is_null()) my_max_mp = v.get("max_mp").as_number();
    }
    publish_state();
    enqueue_trig([this, id, hp, mp, max_hp, max_mp, damage] {
        trig.on_sc(id, hp, mp, max_hp, max_mp, damage);
    });
}

void Account::trig_on_status(const json::Value& v) {
    const std::string id = v.get("id").as_string();
    const std::string action = v.get("action").as_string();
    const std::string name = v.get("name").as_string();
    const std::string count = val_str(v.get("count"));
    const std::string duration = val_str(v.get("duration"));
    const auto& sid = v.get("sid");
    // 主线程：先更新 buff 状态并收集要下发的 sid；随后统一 publish + 逐条入队
    std::vector<std::string> sids;
    auto post = [&](const std::string& s) {
        if (id == role.id) {   // 扩展 _monitorStatus：busy/faint/rash 跟踪
            if (action == "add") my_status.insert(s);
            else if (action == "remove") my_status.erase(s);
            else if (action == "clear") my_status.clear();   // 服务器一次性清空全部 buff
        }
        sids.push_back(s);
    };
    if (sid.is_array()) {
        for (const auto& s : sid.as_array()) post(val_str(s));
    } else if (!sid.is_null()) {
        post(val_str(sid));
    }
    publish_state();
    for (auto& s : sids)
        enqueue_trig([this, id, action, name, count, duration, s] {
            trig.on_status(id, s, action, name, count, duration);
        });
}

void Account::trig_on_pm(const json::Value& v) {
    const auto& list = v.get("list");
    if (!list.is_array()) return;
    for (const auto& item : list.as_array()) {
        if (!item.is_array()) continue;
        const auto& arr = item.as_array();
        std::string id = arr.size() > 0 ? val_str(arr[0]) : "";
        std::string raw_name = arr.size() > 1 ? arr[1].as_string() : "";
        std::string price = arr.size() > 2 ? val_str(arr[2]) : "";
        std::string raw_time = arr.size() > 3 ? val_str(arr[3]) : "";
        enqueue_trig([this, id, raw_name, price, raw_time] {
            trig.on_auction(id, raw_name, price, raw_time);
        });
    }
}

void Account::trig_on_events(const json::Value& v) {
    const auto& items = v.get("items");
    if (!items.is_array()) return;
    for (const auto& item : items.as_array()) {
        if (!item.is_array()) continue;
        const auto& arr = item.as_array();
        std::string type = arr.size() > 0 ? val_str(arr[0]) : "";
        std::string aname = arr.size() > 1 ? arr[1].as_string() : "";
        std::string keyword = arr.size() > 2 ? arr[2].as_string() : "";
        std::string grade = arr.size() > 3 ? val_str(arr[3]) : "";
        // 时间戳：与扩展一致——boss 且 item[4] 非数字且 item[5] 存在 → 用 item[5]；否则 item[4]
        const json::Value& v4 = arr.size() > 4 ? arr[4] : json::Value();
        const json::Value& v5 = arr.size() > 5 ? arr[5] : json::Value();
        int64_t full_ms = 0;   // double→int64 超范围时会保持 0，避免 UB
        if (aname == "boss" && !v4.is_null() && !v4.is_number() && !v5.is_null()) {
            double_to_i64(v5.is_number() ? v5.as_number() : std::atof(v5.as_string().c_str()), full_ms);
        } else if (!v4.is_null() && v4.is_number() && v4.as_number() != 0) {
            double_to_i64(v4.as_number(), full_ms);
        } else if (!v4.is_null() && !v4.is_number() && !v4.as_string().empty()) {
            double_to_i64(std::atof(v4.as_string().c_str()), full_ms);
        }
        // times = 自当日 0 点起的秒数（与扩展 todayStart 计算一致）
        std::string times;
        if (full_ms > 0) {
            time_t sec = full_ms / 1000;
            struct tm tmv;
            localtime_r(&sec, &tmv);
            int64_t midnight = sec - (tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec);
            times = std::to_string(sec - midnight);
        }
        enqueue_trig([this, type, aname, keyword, grade, times] {
            trig.on_activity(type, aname, keyword, grade, times);
        });
    }
}

void Account::trig_on_pack(const json::Value& v) {
    // 拾取：id/name/count 非空且 remove 为 null（扩展 trigger-events-chat.js）
    const auto& rid = v.get("id");
    const auto& rname = v.get("name");
    const auto& rcnt = v.get("count");
    if (rid.is_null() || rname.is_null() || rcnt.is_null()) return;
    if (!v.get("remove").is_null()) return;
    const std::string id = val_str(rid), name = rname.as_string(), cnt = val_str(rcnt);
    enqueue_trig([this, id, name, cnt] { trig.on_pack(id, name, cnt); });
}

void Account::trig_on_social(const json::Value& v) {
    // 社交消息：dialog=message 且无 id/items（扩展 trigger-events-message.js）
    if (!v.get("id").is_null()) return;
    if (!v.get("items").is_null()) return;
    const std::string content = v.get("message").get("content").as_string();
    enqueue_trig([this, content] { trig.on_social(content); });
}

// ---------- 触发器分享码导入（云端下载） ----------

// form-urlencoded 编码：token 中可能含中文/特殊字符，"·触发" 等需要转码
std::string urlencode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

void Account::import_start(const std::string& token) {
    import_data.clear();
    import_err.clear();
    // 分享码必须带"·触发"标记（与扩展 importTrigger 校验一致）
    if (token.find("\xc2\xb7\xe8\xa7\xa6\xe5\x8f\x91") == std::string::npos &&
        token.find("·触发") == std::string::npos) {
        import_state = ImportState::Done;
        import_err = "错误的触发器分享码（缺少 ·触发 标记）";
        log("[导入] " + import_err);
        return;
    }
    std::string body = "token=" + urlencode(token);
    // 云端地址 wsmud.ii74.com 同时监听 http/https；HttpReq 仅支持明文 http
    import_http.start("http://wsmud.ii74.com/S/downloadSingle", "POST", body, now_ms(),
                      "application/x-www-form-urlencoded");
    import_state = ImportState::Downloading;
    import_deadline_ms = now_ms() + HTTP_TIMEOUT_MS;
    log("[导入] 正在下载分享码...");
}

void Account::import_tick(int64_t now) {
    if (import_state != ImportState::Downloading) return;
    if (now > import_deadline_ms) {
        import_http.close();
        import_state = ImportState::Done;
        import_err = "下载超时";
        log("[导入失败] " + import_err);
        return;
    }
    import_http.tick(now);   // 驱动 import_http 的网络进度（连接/发送/接收）；缺失则下载永不完成
    if (!import_http.done()) return;
    import_state = ImportState::Done;
    if (import_http.state == net::HttpReq::State::Failed) {
        import_err = import_http.error;
        log("[导入失败] " + import_err);
        return;
    }
    if (import_http.status != 200) {
        import_err = "服务器返回 HTTP " + std::to_string(import_http.status);
        log("[导入失败] " + import_err);
        return;
    }
    // 解析 {code, data}；扩展约定 data 为可执行的 JS 字符串片段
    std::string perr;
    json::Value v = json::parse(import_http.body, &perr);
    if (!perr.empty() || !v.is_object()) {
        import_err = "分享码数据解析失败";
        log("[导入失败] " + import_err);
        return;
    }
    if (v.get("code").as_int() != 200) {
        std::string hint = v.get("message").as_string("分享码无效");
        import_err = v.get("msg").as_string(hint.c_str());
        log("[导入失败] " + import_err);
        return;
    }
    const json::Value& data = v.get("data");
    if (data.is_string()) import_data = data.as_string();
    else if (data.is_object()) import_data = json::dump(data);
    if (import_data.empty()) {
        import_err = "分享码无有效数据";
        log("[导入失败] " + import_err);
        return;
    }
    log("[导入] 分享码下载成功，等待确认...");
}

}  // namespace mud
}  // namespace wsmud
