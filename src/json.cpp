#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace wsmud {
namespace json {

namespace {

struct Parser {
    const char* p;
    const char* end;
    std::string err;

    explicit Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    bool eof() const { return p >= end; }
    char peek() const { return p < end ? *p : '\0'; }

    bool expect(char c) {
        skip_ws();
        if (eof() || *p != c) {
            fail(std::string("期望 '") + c + "'");
            return false;
        }
        ++p;
        return true;
    }

    void fail(const std::string& msg) {
        if (err.empty()) err = msg;
        p = end;  // 短路后续解析
    }

    Value parse_value() {
        skip_ws();
        if (eof()) { fail("意外结束"); return Value(); }
        switch (*p) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': case '\'': return parse_string();
            case 't': return parse_lit("true", Value(true));
            case 'f': return parse_lit("false", Value(false));
            case 'n': return parse_lit("null", Value());
            default:
                if (*p == '-' || std::isdigit(static_cast<unsigned char>(*p))) return parse_number();
                fail(std::string("意外的字符 '") + *p + "'");
                return Value();
        }
    }

    Value parse_lit(const char* word, Value v) {
        for (const char* w = word; *w; ++w, ++p) {
            if (p >= end || *p != *w) { fail(std::string("期望 ") + word); return Value(); }
        }
        return v;
    }

    Value parse_number() {
        const char* start = p;
        if (peek() == '-') ++p;
        while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
        if (p < end && *p == '.') { ++p; while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p; }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
        }
        if (p == start || (p - start == 1 && *start == '-')) { fail("无效数字"); return Value(); }
        char buf[64];
        std::size_t len = static_cast<std::size_t>(p - start);
        if (len >= sizeof(buf)) { fail("数字过长"); return Value(); }
        std::memcpy(buf, start, len);
        buf[len] = '\0';
        return Value(std::strtod(buf, nullptr));
    }

    Value parse_string() {
        char quote = *p++;
        std::string out;
        while (true) {
            if (eof()) { fail("字符串未闭合"); return Value(); }
            char c = *p++;
            if (c == quote) break;
            if (c == '\\') {
                if (eof()) { fail("转义未完成"); return Value(); }
                char e = *p++;
                switch (e) {
                    case '"': out += '"'; break;
                    case '\'': out += '\''; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (p + 4 > end) { fail("unicode 转义不完整"); return Value(); }
                        unsigned int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= h - '0';
                            else if (h >= 'a' && h <= 'f') code |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') code |= h - 'A' + 10;
                            else { fail("unicode 转义非法"); return Value(); }
                        }
                        // 简单 UTF-8 编码（仅 BMP；代理对直接按码点输出，够用）
                        if (code < 0x80) out += static_cast<char>(code);
                        else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: fail("未知转义"); return Value();
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                fail("字符串包含控制字符");
                return Value();
            } else {
                out += c;
            }
        }
        return Value(std::move(out));
    }

    Value parse_object() {
        if (!expect('{')) return Value();
        Value::Object obj;
        skip_ws();
        if (peek() == '}') { ++p; return Value(std::move(obj)); }
        while (true) {
            skip_ws();
            std::string key;
            if (peek() == '"' || peek() == '\'') {
                Value k = parse_string();
                if (!k.is_string()) return Value();
                key = std::get<std::string>(std::move(k.v));
            } else {
                // 容错：裸词键
                const char* ks = p;
                while (p < end && !std::isspace(static_cast<unsigned char>(*p)) && *p != ':' && *p != ',' && *p != '}') ++p;
                if (p == ks) { fail("对象键缺失"); return Value(); }
                key.assign(ks, static_cast<std::size_t>(p - ks));
            }
            if (!expect(':')) return Value();
            Value val = parse_value();
            if (!err.empty()) return Value();
            obj[std::move(key)] = std::move(val);
            skip_ws();
            if (peek() == ',') { ++p; continue; }
            if (peek() == '}') { ++p; return Value(std::move(obj)); }
            fail("对象缺少 ',' 或 '}'");
            return Value();
        }
    }

    Value parse_array() {
        if (!expect('[')) return Value();
        Value::Array arr;
        skip_ws();
        if (peek() == ']') { ++p; return Value(std::move(arr)); }
        while (true) {
            Value v = parse_value();
            if (!err.empty()) return Value();
            arr.push_back(std::move(v));
            skip_ws();
            if (peek() == ',') { ++p; continue; }
            if (peek() == ']') { ++p; return Value(std::move(arr)); }
            fail("数组缺少 ',' 或 ']'");
            return Value();
        }
    }
};

std::string dump_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

std::string dump_impl(const Value& value) {
    switch (value.v.index()) {
        case 0: return "null";
        case 1: return std::get<bool>(value.v) ? "true" : "false";
        case 2: {
            double d = std::get<double>(value.v);
            if (std::isfinite(d)) {
                if (d == static_cast<double>(static_cast<long long>(d)))
                    return std::to_string(static_cast<long long>(d));
                return std::to_string(d);
            }
            return "null";
        }
        case 3: return dump_string(std::get<std::string>(value.v));
        case 4: {
            const auto& arr = std::get<Value::Array>(value.v);
            std::string out = "[";
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (i) out += ",";
                out += dump_impl(arr[i]);
            }
            out += "]";
            return out;
        }
        default: {
            const auto& obj = std::get<Value::Object>(value.v);
            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : obj) {
                if (!first) out += ",";
                first = false;
                out += dump_string(k);
                out += ":";
                out += dump_impl(v);
            }
            out += "}";
            return out;
        }
    }
}

}  // namespace

Value parse(const std::string& text, std::string* err) {
    Parser parser(text);
    parser.skip_ws();
    if (parser.eof()) {
        if (err) *err = "空输入";
        return Value();
    }
    Value v = parser.parse_value();
    parser.skip_ws();
    if (!parser.err.empty()) {
        if (err) *err = parser.err;
        return Value();
    }
    if (!parser.eof()) {
        if (err) *err = "尾部存在多余字符";
        return Value();
    }
    return v;
}

std::string dump(const Value& value) {
    return dump_impl(value);
}

namespace {

std::string indent_str(int depth) {
    std::string s;
    for (int i = 0; i < depth; ++i) s += "  ";
    return s;
}

void dump_pretty_impl(const Value& value, int depth, std::string& out) {
    switch (value.v.index()) {
        case 0: out += "null"; break;
        case 1: out += std::get<bool>(value.v) ? "true" : "false"; break;
        case 2: {
            double d = std::get<double>(value.v);
            if (std::isfinite(d)) {
                if (d == static_cast<double>(static_cast<long long>(d)))
                    out += std::to_string(static_cast<long long>(d));
                else
                    out += std::to_string(d);
            } else {
                out += "null";
            }
            break;
        }
        case 3: out += dump_string(std::get<std::string>(value.v)); break;
        case 4: {
            const auto& arr = std::get<Value::Array>(value.v);
            out += "[";
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (i) out += ",";
                out += "\n" + indent_str(depth + 1);
                dump_pretty_impl(arr[i], depth + 1, out);
            }
            if (!arr.empty()) out += "\n" + indent_str(depth);
            out += "]";
            break;
        }
        default: {
            const auto& obj = std::get<Value::Object>(value.v);
            out += "{";
            bool first = true;
            for (const auto& [k, v] : obj) {
                if (!first) out += ",";
                first = false;
                out += "\n" + indent_str(depth + 1);
                out += dump_string(k);
                out += ": ";
                dump_pretty_impl(v, depth + 1, out);
            }
            if (!obj.empty()) out += "\n" + indent_str(depth);
            out += "}";
            break;
        }
    }
}

}  // namespace

std::string dump_pretty(const Value& value) {
    std::string out;
    dump_pretty_impl(value, 0, out);
    out += "\n";
    return out;
}

}  // namespace json
}  // namespace wsmud
