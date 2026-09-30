#include "mud.hpp"

#include <cstdio>

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

// JSON 值 → 字符串（数字尽量整数；null/缺键 → 空串）
std::string val_str(const json::Value& v) {
    if (v.is_string()) return v.as_string();
    if (v.is_number()) {
        double d = v.as_number();
        int64_t i = static_cast<int64_t>(d);
        if (d == static_cast<double>(i)) return std::to_string(i);
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.2f", d);
        return buf;
    }
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    return "";
}

}  // namespace

void Account::set_stage(Stage s, int64_t now, int64_t timeout_ms) {
    stage = s;
    stage_deadline_ms = timeout_ms > 0 ? now + timeout_ms : 0;
}

void Account::enter_disconnected(const std::string& reason) {
    ws.close();
    http.close();
    last_error = reason;
    set_stage(Stage::Disconnected, now_ms(), 0);
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
            // 触发器心跳：合并——队列里已有待执行 tick 则跳过本帧，避免重脚本时积压
            if (trig_q_.empty()) enqueue_trig([this, now] { trig.tick(now); });
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
            enqueue_trig([this, v] {
                trig.on_dispfm(v.get("id").as_string(), v.get("rtime").as_string(),
                               v.get("distime").as_string(), now_ms());
            });
        } else if (type == "sc") {
            trig_on_sc(v);
        } else if (type == "items") {
            trig_on_items(v);
        } else if (type == "itemadd") {
            trig_on_itemadd(v);
        } else if (type == "itemremove") {
            const std::string id = v.get("id").as_string();
            enqueue_trig([this, id] { trig.on_itemremove(id); });
        } else if (type == "state") {
            my_state_text = state_word(v.get("state").as_string());
            update_idle(now_ms());
        } else if (type == "room") {
            my_room_name = v.get("name").as_string();
            publish_state();
            enqueue_trig([this] { trig.on_room_clear(); });
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
        else if (dialog == "events") trig_on_events(v);
    }
    // 走到这里说明该包未显示在聊天区/文本区：status/combat/die/dispfm/sc/items/
    // itemadd/itemremove/state/room、dialog 包及未知类型 → 记入右侧"网络包"栏
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

// ---------- 触发器 worker 线程 ----------

void Account::worker_loop() {
    for (;;) {
        std::function<void()> fn;
        if (!trig_q_.pop(fn)) break;   // stop() 且队列清空 → 退出
        fn();
    }
}
void Account::start_trig_worker() {
    if (worker_started_) return;
    worker_started_ = true;
    trig_q_.reset();   // 复位 stop 位并清残留命令（可重复启动，如重连）
    main_q_.reset();
    trig_worker_ = std::thread(&Account::worker_loop, this);
}
void Account::stop_trig_worker() {
    if (!worker_started_) return;
    worker_started_ = false;
    // 在 worker 线程释放 JS 运行时（QuickJS 线程亲和：创建与销毁须同线程），
    // 该闭包执行完后队列空、stop 位置位 → worker pop 返回 false 退出。
    trig_q_.push([this] { trig.release(); });
    trig_q_.stop();
    if (trig_worker_.joinable()) trig_worker_.join();
}
void Account::enqueue_trig(std::function<void()> fn) {
    if (!worker_started_) return;
    trig_q_.push(std::move(fn));
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
        int64_t full_ms = 0;
        if (aname == "boss" && !v4.is_null() && !v4.is_number() && !v5.is_null()) {
            full_ms = static_cast<int64_t>(v5.is_number() ? v5.as_number() : std::atof(v5.as_string().c_str()));
        } else if (!v4.is_null() && v4.is_number() && v4.as_number() != 0) {
            full_ms = static_cast<int64_t>(v4.as_number());
        } else if (!v4.is_null() && !v4.is_number() && !v4.as_string().empty()) {
            full_ms = static_cast<int64_t>(std::atof(v4.as_string().c_str()));
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

}  // namespace mud
}  // namespace wsmud
