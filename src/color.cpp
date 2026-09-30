// 游戏 HTML 颜色标记 → ANSI 转义序列（实现，见 color.hpp）
#include "color.hpp"

#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace wsmud {

namespace {

// 样式表里的 23 个颜色标签 → 基础 16 色 SGR 前景码。
// 用基础 SGR（31-37/90-97）而非 truecolor（38;2;r;g;b）：不少终端对 truecolor 支持不完整，
// 会直接忽略那组序列导致文本退化成默认白色（UI 的基础色如蓝色条、绿色边框却能正常显示）。
// 此处按用户提供的 CSS hex 就近归入最接近的基础色。
struct ColorDef { const char* name; const char* code; };
const ColorDef kColors[] = {
    {"NOR", "32"}, {"GRE", "32"}, {"BLK", "90"}, {"BLU", "34"}, {"CYN", "36"},
    {"RED", "31"}, {"MAG", "35"}, {"YEL", "33"}, {"WHT", "37"}, {"ORA", "33"},
    {"HIK", "90"}, {"HIB", "94"}, {"HIG", "92"}, {"HIC", "96"}, {"HIR", "91"},
    {"HIM", "95"}, {"HIY", "93"}, {"HIW", "97"}, {"HIO", "33"}, {"HIJ", "93"},
    {"HIZ", "95"}, {"ORD", "91"},
};

// 颜色名 → 基础 SGR 前景序列；未知名返回空串
std::string color_seq(const std::string& name) {
    for (const auto& c : kColors) {
        if (name == c.name) {
            std::string s = "\x1b[";
            s += c.code;
            s += 'm';
            return s;
        }
    }
    return "";
}

void toupper_self(std::string& s) {
    for (char& ch : s)
        if (ch >= 'a' && ch <= 'z') ch -= static_cast<char>('a' - 'A');
}

// 码点 → UTF-8
void append_cp(std::string& out, unsigned cp) {
    if (cp < 0x80) { out += static_cast<char>(cp); return; }
    if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// 解析 <开标签> 的标签名（跳过前导 '/' 与空白）
std::string tag_name(const std::string& inner, bool closing) {
    std::size_t a = closing && !inner.empty() ? (inner[0] == '/' ? 1 : 0) : 0;
    while (a < inner.size() && (inner[a] == ' ' || inner[a] == '\t')) ++a;
    std::size_t b = a;
    while (b < inner.size() && !(inner[b] == ' ' || inner[b] == '\t' || inner[b] == '/' || inner[b] == '>'))
        ++b;
    return inner.substr(a, b - a);
}

// 开标签是否带 class="hide"（严格取引号内值比较）
bool has_hide_class(const std::string& inner) {
    std::size_t p = inner.find("class=");
    if (p == std::string::npos) return false;
    p += 6;
    while (p < inner.size() && (inner[p] == ' ' || inner[p] == '\t')) ++p;
    if (p >= inner.size()) return false;
    char q = inner[p];
    if (q != '"' && q != '\'') return false;
    std::size_t q2 = inner.find(q, p + 1);
    if (q2 == std::string::npos) return false;
    return inner.compare(p + 1, q2 - p - 1, "hide") == 0;
}

}  // namespace

std::string html_to_ansi(const std::string& html) {
    std::string out;
    std::vector<std::string> stack;   // 每层开启时父级样式快照（元素级，进出平衡）
    std::string active;               // 当前完整样式序列
    std::vector<bool> hs;             // 元素级隐藏栈：true=本元素或其父级 class="hide"
    auto hidden = [&] { return !hs.empty() && hs.back(); };
    auto emit = [&](const std::string& s) { if (!hidden()) out += s; };

    // 开启一个元素：记录父级快照；可视且有样式增量时应用并输出
    auto open = [&](bool isHidden, const std::string& delta) {
        bool parentHidden = !hs.empty() && hs.back();
        hs.push_back(parentHidden || isHidden);
        stack.push_back(active);
        if (!delta.empty() && !parentHidden) {
            active += delta;
            emit(delta);
        }
    };
    // 关闭一个元素：弹出快照，回到父级样式（父级可见时重发复位+父级样式）
    auto close = [&] {
        if (!hs.empty()) hs.pop_back();
        if (!stack.empty()) {
            std::string snap = stack.back();
            stack.pop_back();
            if (active != snap) {
                if (!hidden()) { emit("\x1b[0m"); if (!snap.empty()) emit(snap); }
                active = snap;
            }
        } else {
            emit("\x1b[0m");
            active.clear();
        }
    };

    std::size_t i = 0, n = html.size();
    while (i < n) {
        char c = html[i];

        // ---- HTML 实体 ----
        if (c == '&') {
            std::size_t sem = html.find(';', i + 1);
            if (sem == std::string::npos || sem - i > 12) { emit("&"); ++i; continue; }
            std::string ent = html.substr(i + 1, sem - i - 1);
            std::string rep;
            if (ent == "nbsp") rep = " ";
            else if (ent == "amp") rep = "&";
            else if (ent == "lt") rep = "<";
            else if (ent == "gt") rep = ">";
            else if (ent == "quot") rep = "\"";
            else if (ent == "apos") rep = "'";
            else if (!ent.empty() && ent[0] == '#') {
                char* endp = nullptr;
                unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                     ? std::strtoul(ent.c_str() + 2, &endp, 16)
                                     : std::strtoul(ent.c_str() + 1, &endp, 10);
                if (endp && *endp == '\0') append_cp(rep, static_cast<unsigned>(cp));
            }
            if (rep.empty()) rep = ent;
            emit(rep);
            i = sem + 1;
            continue;
        }

        // ---- 标签 ----
        if (c == '<') {
            std::size_t g = html.find('>', i + 1);
            if (g == std::string::npos) { emit("<"); ++i; continue; }
            std::string inner = html.substr(i + 1, g - i - 1);
            i = g + 1;
            bool closing = !inner.empty() && inner[0] == '/';
            std::string name = tag_name(inner, closing);
            if (name.empty()) { emit("<" + inner + ">"); continue; }
            toupper_self(name);

            if (name == "BR") { emit("\n"); continue; }   // 换行：不进元素栈

            if (closing) { close(); continue; }            // 全部按配对的元素关闭处理

            // ---- 开启标签 ----
            bool isHide = (name == "SPAN" || name == "DIV") && has_hide_class(inner);
            if (name == "P" || name == "DIV" || name == "SPAN" || name == "FONT" ||
                name == "B" || name == "I" || name == "U") {
                open(isHide, "");
                continue;
            }
            if (name == "OPT") { open(isHide, "\x1b[2m"); continue; }   // 半透明 → 暗色
            std::string seq = color_seq(name);
            if (!seq.empty()) {
                if (name == "NOR") {   // 普通：清掉旧样式，仅置绿
                    stack.clear();
                    active.clear();
                }
                open(isHide, seq);
                continue;
            }
            emit("<" + inner + ">");   // 未知标签：原样保留
            continue;
        }

        // ---- 普通字符 ----
        emit(std::string(1, c));
        ++i;
    }
    if (!active.empty()) out += "\x1b[0m";   // 行尾统一复位，避免颜色污染后续行
    return out;
}

}  // namespace wsmud