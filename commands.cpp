// commands.cpp — 程序命令处理（process_line 及全部 cmd_*，含 trigger.json 重载）
#include <unistd.h>    // readlink
#include <cstdio>      // fopen/fwrite/fclose
#include <cstdlib>     // strtol
#include <string>
#include <utility>     // std::move
#include <vector>

#ifdef _WIN32
#include <windows.h>   // 原生 Windows 构建：直接用 Win32 剪贴板 API
#include <cstring>     // memcpy/memset
#endif

#include "app.hpp"

namespace {

// ---------- trigger.json 加载（程序只读，reloadTrigger 重载） ----------

// 可执行文件所在目录（trigger.json 与之同级）
std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0) return ".";
    buf[n] = '\0';
    std::string p = buf;
    std::size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

// 原子写：先写同目录临时文件，flush + fsync 落盘后 rename 原位覆盖。
// rename 为原子操作，写盘中途崩溃也只会留下可删除的临时文件，绝不损坏原配置。
bool write_atomic(const std::string& path, const std::string& content, std::string& err) {
    std::string tmp = path + ".tmp";
    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) { err = "无法写入 " + tmp; return false; }
    bool ok = std::fwrite(content.data(), 1, content.size(), fp) == content.size();
    if (ok && std::fflush(fp) == 0) {
        if (::fsync(fileno(fp)) != 0) { ok = false; err = "fsync 失败"; }
    } else {
        ok = false;
        if (err.empty()) err = "写入失败";
    }
    std::fclose(fp);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;
    err = "重命名覆盖失败";
    std::remove(tmp.c_str());
    return false;
}

void cmd_status() {
    for (auto& a : accounts) {
        const char* st = stage_name(a->stage);
        if (a->stage == Account::Stage::Online) {
            out("[账号" + std::to_string(a->index) + "] " + st + " | " + a->role.name + " Lv." +
                std::to_string(a->role.level) + " | 服务器: " + a->server.name);
        } else if (a->stage == Account::Stage::Disconnected) {
            out("[账号" + std::to_string(a->index) + "] " + st + " | 原因: " + a->last_error);
        } else {
            out("[账号" + std::to_string(a->index) + "] " + st);
        }
    }
}

void cmd_help() {
    out("程序命令：status / reconnect [N|all] / send <N> <命令> / reloadTrigger / copyId <N>（复制房间第 N 个人物的 ID）/ quit（退出当前槽位账号）/ help");
    out("触发器：trigger list 查看；F8 列表内 a/e/空格/d 可直接增删改并原子写回 trigger.json，也可改文件后 reloadTrigger 重载");
    out("按键：F1-F5 选中槽位 | ←→ 切换标签页 | ↑↓ 滚动日志 | F6 切换命令/游戏命令 | "
        "F7 新增标签页 | F8 触发器列表 | F9 设置（自动施法/自动喜宴，按玩家） | DEL 删除当前标签页（仅序号>5） | F10 退出程序");
}

void cmd_reconnect(const std::string& arg) {
    if (arg.empty() || arg == "all") {
        for (auto& a : accounts) a->reconnect();
        return;
    }
    int n;
    std::size_t nacc = accounts.size();
    if (!parse_int(arg, n) || n < 1 || static_cast<std::size_t>(n) > nacc) {
        out("账号序号无效（1-" + std::to_string(nacc) + "）");
        return;
    }
    accounts[n - 1]->reconnect();
}

void cmd_send(const std::string& arg) {
    std::size_t sp = arg.find(' ');
    if (sp == std::string::npos) {
        out("用法：send <账号序号> <命令>，如：send 1 look");
        return;
    }
    int n;
    std::size_t nacc = accounts.size();
    if (!parse_int(arg.substr(0, sp), n) || n < 1 || static_cast<std::size_t>(n) > nacc) {
        out("账号序号无效（1-" + std::to_string(nacc) + "）");
        return;
    }
    accounts[n - 1]->send_command(trim(arg.substr(sp + 1)));
}

// ---------- 触发器命令（trigger list 只读查看） ----------

void cmd_trigger(const std::string& arg) {
    if (arg.empty() || arg == "list") {
        const auto& list = accounts[static_cast<std::size_t>(sel)]->trig.list();
        if (list.empty()) { out("账号" + std::to_string(sel + 1) + " 暂无触发器，请编辑 trigger.json 后输入 reloadTrigger 重载"); return; }
        for (std::size_t i = 0; i < list.size(); ++i)
            out("#" + std::to_string(i + 1) + " [" + (list[i].active ? "启用" : "停用") + "] " +
                list[i].name + "  ← " + list[i].event);
        return;
    }
    out("用法：trigger list");
}

// ---------- copyId：把房间人物的 ID 复制到系统剪贴板 ----------

// Base64（RFC4648 标准表 + '=' 填充），供 OSC 52 剪贴板序列使用
std::string base64(const std::string& s) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    while (i + 3 <= s.size()) {
        unsigned v = (static_cast<unsigned char>(s[i]) << 16) |
                     (static_cast<unsigned char>(s[i + 1]) << 8) |
                     static_cast<unsigned char>(s[i + 2]);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
        i += 3;
    }
    std::size_t rem = s.size() - i;
    if (rem == 1) {
        unsigned v = static_cast<unsigned char>(s[i]) << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        unsigned v = (static_cast<unsigned char>(s[i]) << 16) |
                     static_cast<unsigned char>(s[i + 1]) << 8;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// UTF-8 → UTF-16LE。Windows 的 clip.exe 按 CF_UNICODETEXT 读取 stdin，
// 直接送 UTF-8 字节会把中文等非 ASCII 变成乱码，故必须先转换。
std::string utf8_to_utf16le(const std::string& s) {
    std::string out;
    auto put = [&](unsigned u) {
        out.push_back(static_cast<char>(u & 0xFF));
        out.push_back(static_cast<char>((u >> 8) & 0xFF));
    };
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = 1;
        std::uint32_t cp = c;
        if (c >= 0xF0) { n = 4; cp = c & 0x07u; }
        else if (c >= 0xE0) { n = 3; cp = c & 0x0Fu; }
        else if (c >= 0xC0) { n = 2; cp = c & 0x1Fu; }
        if (i + n > s.size()) { n = 1; cp = c; }   // 截断的残缺序列：按单字节兜底
        for (std::size_t k = 1; k < n; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        i += n;
        if (cp <= 0xFFFF) {
            put(cp);
        } else {                                   // 补充平面 → UTF-16 代理对
            cp -= 0x10000;
            put(0xD800 + (cp >> 10));
            put(0xDC00 + (cp & 0x3FF));
        }
    }
    return out;
}

// 把文本送进 Windows 剪贴板。
// 两种构建各走一条通路：
//   _WIN32（MinGW 原生 Windows）：直接调 Win32 剪贴板 API，不依赖终端、也不依赖 clip.exe；
//   非 Windows（WSL/Linux）：调 Windows 侧的 clip.exe（WSL 互操作），路径不存在则返回 false，
//   由调用方回退到 OSC 52 交给终端处理。
#ifdef _WIN32
bool copy_to_windows_clipboard(const std::string& text) {
    const std::string u16 = utf8_to_utf16le(text);   // UTF-16LE 字节流
    if (!::OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (::EmptyClipboard()) {
        const SIZE_T bytes = u16.size() + 2;         // +2 字节：末尾 UTF-16 NUL
        HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            void* dst = ::GlobalLock(h);
            if (dst) {
                std::memcpy(dst, u16.data(), u16.size());
                std::memset(static_cast<char*>(dst) + u16.size(), 0, 2);
                ::GlobalUnlock(h);
                // SetClipboardData 成功后所有权归系统，不得再 GlobalFree
                if (::SetClipboardData(CF_UNICODETEXT, h)) ok = true;
            }
            if (!ok) ::GlobalFree(h);
        }
    }
    ::CloseClipboard();
    return ok;
}
#else
bool copy_to_windows_clipboard(const std::string& text) {
    const char* clip = "/mnt/c/Windows/System32/clip.exe";
    if (::access(clip, X_OK) != 0) return false;
    // popen 的 "w" 会把子进程 stdin 接到管道上（不会抢走终端的输入）；
    // 2>/dev/null 避免失败信息打到 TUI 画面上
    FILE* p = ::popen((std::string(clip) + " 2>/dev/null").c_str(), "w");
    if (!p) return false;
    const std::string u16 = utf8_to_utf16le(text);
    bool ok = std::fwrite(u16.data(), 1, u16.size(), p) == u16.size();
    if (::pclose(p) != 0) ok = false;
    return ok;
}
#endif

// copyId <N>：复制房间人物列表第 N 个人的 ID（N 从 1 开始，按房间区的显示顺序）。
// 房间区只显示 5 行，但 N 可大于 5 —— 只要该人物还在列表中就能准确复制。
void cmd_copy_id(const std::string& arg) {
    auto& a = *accounts[static_cast<std::size_t>(sel)];
    int n = 0;
    if (!parse_int(trim(arg), n) || n < 1) {
        out("用法：copyId <序号>，如 copyId 1（复制房间第 1 个人物的 ID）");
        return;
    }
    const auto& ppl = a.my_room_people;
    if (static_cast<std::size_t>(n) > ppl.size()) {
        out("房间人物只有 " + std::to_string(ppl.size()) + " 个，序号 " + std::to_string(n) + " 超出范围");
        return;
    }
    // 与房间区显示顺序一致：第 N 个人 = 列表中倒数第 N 个
    const auto& p = ppl[ppl.size() - static_cast<std::size_t>(n)];
    if (p.first.empty()) { out("第 " + std::to_string(n) + " 个人物没有可用 ID"); return; }
    // 优先写 Windows 剪贴板（WSL 下由 clip.exe 完成，与终端是否支持 OSC 52 无关）；
    // 非 WSL 环境退回 OSC 52，交给终端把 base64 解出后写它所在系统的剪贴板。
    if (copy_to_windows_clipboard(p.first)) {
        out("已复制第 " + std::to_string(n) + " 个人物的 ID：" + p.first + "（Windows 剪贴板）");
        return;
    }
    const std::string seq = "\x1b]52;c;" + base64(p.first) + "\x07";
    std::fwrite(seq.data(), 1, seq.size(), stdout);
    std::fflush(stdout);
    out("已复制第 " + std::to_string(n) + " 个人物的 ID：" + p.first +
        "（OSC 52，终端需支持）");
}

}  // namespace

bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

const char* stage_name(Account::Stage s) {
    using St = Account::Stage;
    switch (s) {
        case St::None: return "未启动";
        case St::WaitServers: return "获取服务器列表";
        case St::NeedSelectServer: return "待选择服务器";
        case St::WaitLogin: return "登录中";
        case St::AuthWs: return "认证连接中";
        case St::WaitRoles: return "等待角色列表";
        case St::NeedSelectRole: return "待选择角色";
        case St::EnterWs: return "进入游戏连接中";
        case St::Entering: return "进入游戏中";
        case St::Online: return "在线挂机";
        case St::Disconnected: return "已断开";
    }
    return "未知";
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// 导入分享码触发器的落库：归属到当前角色并原子写回 trigger.json。
// 返回 false 表示未导入（同角色重名 / 未进入游戏）。
bool import_accept_trigger(const wsmud::trigger::Trigger& cand) {
    const auto& a = *accounts[static_cast<std::size_t>(sel)];
    if (a.my_name.empty()) { out("[导入] 当前未进入游戏，无法确定归属角色"); return false; }
    const std::string my = a.my_name;
    for (const auto& t : g_trig_cfg)
        if (t.owner == my && t.name == cand.name) {
            out("[导入] 已存在同名触发器「" + cand.name + "」，已跳过");
            return false;
        }
    wsmud::trigger::Trigger t = cand;
    t.owner = my;
    g_trig_cfg.push_back(std::move(t));
    persist_triggers();
    return true;
}

// 重新加载触发器：读 exe 同级 trigger.json，校验后替换所有账号的引擎。
// 文件不存在时创建空模板（唯一写操作）。失败保留当前配置。
void reload_triggers() {
    std::string path = exe_dir() + "/trigger.json";
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        FILE* w = std::fopen(path.c_str(), "wb");
        if (w) {
            const char tmpl[] = "{\"version\":1,\"triggers\":[]}\n";
            std::fwrite(tmpl, 1, sizeof tmpl - 1, w);
            std::fclose(w);
        }
        out("trigger.json 不存在，已创建空模板，请编辑后输入 reloadTrigger 重载");
        return;
    }
    std::fclose(fp);
    std::vector<wsmud::trigger::Trigger> list;
    std::string err;
    if (!wsmud::trigger::Engine::load_from_file(path, list, err)) {
        out("[重载失败] " + err + "（保留当前配置）");
        return;
    }
    g_trig_cfg = list;
    for (auto& a : accounts) a->set_triggers_all(list);
    out("已重载 " + std::to_string(list.size()) + " 个触发器（trigger.json）");
}

// 把内存配置原子写回 trigger.json，并同步到所有账号引擎。
// 关键保障：先 dump 再 load_from_file 往返校验（同一套规则），dump 有 bug 也在写盘前暴露；
// 写盘只用原子写，失败时内存配置与磁盘旧文件均不被破坏。
void persist_triggers() {
    std::string path = exe_dir() + "/trigger.json";
    std::string text = wsmud::trigger::Engine::dump_triggers(g_trig_cfg);
    // 往返校验：写出的文本必须能被 load_from_file 原样读回
    std::string tmp = path + ".verify";
    FILE* fw = std::fopen(tmp.c_str(), "wb");
    std::vector<wsmud::trigger::Trigger> check;
    std::string err;
    bool ok = false;
    if (fw) {
        std::fwrite(text.data(), 1, text.size(), fw);
        std::fclose(fw);
        ok = wsmud::trigger::Engine::load_from_file(tmp, check, err);
        std::remove(tmp.c_str());
    }
    if (!ok) { out("[保存失败] " + (err.empty() ? "校验读回异常" : err) + "（未写盘，内存配置保留）"); return; }
    if (!write_atomic(path, text, err)) { out("[保存失败] " + err + "（未写盘，内存配置保留）"); return; }
    g_trig_cfg = std::move(check);   // 以读回的规范化配置作为唯一真相
    for (auto& a : accounts) a->set_triggers_all(g_trig_cfg);
    out("已保存 " + std::to_string(g_trig_cfg.size()) + " 个触发器（trigger.json）");
}

// ---------- settings.json 加载/写回（按玩家隔离的本地设置，长期存储） ----------
// 结构：{"version":1,"players":{"<角色名>":{"auto_perform":bool,"auto_marry":bool}}}
// 与 trigger.json 同目录（软件同级）。文件不存在时创建空模板；解析失败保留空表。

void load_settings() {
    std::string path = exe_dir() + "/settings.json";
    mud::g_settings.clear();
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        FILE* w = std::fopen(path.c_str(), "wb");
        if (w) {
            const char tmpl[] = "{\"version\":1,\"players\":{}}\n";
            std::fwrite(tmpl, 1, sizeof tmpl - 1, w);
            std::fclose(w);
        }
        out("settings.json 不存在，已创建空模板");
        return;
    }
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    std::fclose(fp);
    std::string err;
    json::Value root = json::parse(text, &err);
    if (!err.empty()) {
        out("[设置] settings.json 解析失败：" + err + "（使用默认设置）");
        return;
    }
    const auto& players = root.get("players");
    if (!players.is_object()) return;
    for (const auto& kv : players.as_object()) {
        mud::PlayerSettings ps;
        ps.auto_perform = kv.second.get("auto_perform").as_bool(false);
        ps.auto_marry = kv.second.get("auto_marry").as_bool(false);
        mud::g_settings[kv.first] = ps;
    }
    out("已加载 " + std::to_string(mud::g_settings.size()) + " 个玩家的设置（settings.json）");
}

// 把内存设置表原子写回 settings.json（临时文件 + rename，崩溃不损坏原文件）
void persist_settings() {
    json::Value::Object players;
    for (const auto& kv : mud::g_settings) {
        json::Value::Object po;
        po["auto_perform"] = json::Value(kv.second.auto_perform);
        po["auto_marry"] = json::Value(kv.second.auto_marry);
        players[kv.first] = json::Value(std::move(po));
    }
    json::Value::Object root;
    root["version"] = json::Value(1);
    root["players"] = json::Value(std::move(players));
    std::string text = json::dump_pretty(json::Value(std::move(root)));
    std::string err;
    if (!write_atomic(exe_dir() + "/settings.json", text, err))
        out("[设置] 保存失败：" + err);
}

// 处理程序命令（一行）
void process_line(const std::string& raw) {
    std::string line = trim(raw);
    if (line.empty()) return;

    // quit：退出当前选中槽位的账号并重置，回到账号/密码录入阶段，避免该标签页废掉
    //（退出程序请用 F10 或 Ctrl+C）
    if (line == "quit" || line == "exit") {
        auto& a = *accounts[static_cast<std::size_t>(sel)];
        if (a.account.empty()) {
            out("槽位" + std::to_string(sel + 1) + " 未配置账号");
            return;
        }
        a.disconnect();
        acc_logs[static_cast<std::size_t>(a.index - 1)].clear();  // 槽位重置时清空该标签页输出区
        acc_chat_logs[static_cast<std::size_t>(a.index - 1)].clear();  // 同步清空聊天区
        acc_pkt_logs[static_cast<std::size_t>(a.index - 1)].clear();   // 同步清空网络包栏
        // 清空凭据与状态，让该槽位循环回到录入阶段（start_login 会自行重置 servers/roles）
        a.account.clear();
        a.password.clear();
        a.need = Account::Need::None;
        a.stage = Account::Stage::None;
        a.last_error.clear();
        account_log(a.index, "已退出该账号，槽位已重置，请重新录入账号密码");
        if (sel == a.index - 1) {
            input_stage = InputStage::Account;
            cmd_buf.clear();
        }
        return;
    }

    // reloadTrigger：重新加载软件同级目录 trigger.json（程序只读，唯一写操作是首次创建空模板）
    if (line == "reloadTrigger") { reload_triggers(); return; }

    // 优先路由给等待输入的账号（选择服务器/角色）
    for (auto& a : accounts) {
        if (a->need != Account::Need::None) {
            int n;
            if (!parse_int(line, n)) {
                out("请输入序号：" + a->prompt);
                return;
            }
            if (a->need == Account::Need::Server) a->select_server(n - 1);
            else a->select_role(n - 1);
            return;
        }
    }

    if (line == "status") { cmd_status(); return; }
    if (line == "help" || line == "h" || line == "?") { cmd_help(); return; }
    if (line == "reconnect" || line.rfind("reconnect ", 0) == 0) {
        cmd_reconnect(trim(line.substr(9)));
        return;
    }
    if (line == "send" || line.rfind("send ", 0) == 0) {
        cmd_send(trim(line.substr(4)));
        return;
    }
    if (line == "trigger" || line.rfind("trigger ", 0) == 0) {
        cmd_trigger(trim(line.substr(7)));
        return;
    }
    if (line == "copyId" || line.rfind("copyId ", 0) == 0) {
        cmd_copy_id(trim(line.substr(6)));
        return;
    }
    out("未知命令：" + line + "（输入 help 查看帮助）");
}
