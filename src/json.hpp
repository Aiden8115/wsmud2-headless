// 轻量 JSON 解析/序列化库
// 容错能力：字符串允许单引号 '...'（游戏服务器会返回单引号 JSON），对象键允许无引号裸词。
#pragma once

#include <string>
#include <vector>
#include <map>
#include <variant>
#include <cstdint>
#include <cmath>

namespace wsmud {
namespace json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Storage v{nullptr};

    Value() {}
    Value(std::nullptr_t) : v(nullptr) {}
    Value(bool b) : v(b) {}
    Value(int i) : v(static_cast<double>(i)) {}
    Value(double d) : v(d) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(Array a) : v(std::move(a)) {}
    Value(Object o) : v(std::move(o)) {}

    bool is_null() const  { return std::holds_alternative<std::nullptr_t>(v); }
    bool is_bool() const  { return std::holds_alternative<bool>(v); }
    bool is_number() const{ return std::holds_alternative<double>(v); }
    bool is_string() const{ return std::holds_alternative<std::string>(v); }
    bool is_array() const { return std::holds_alternative<Array>(v); }
    bool is_object() const{ return std::holds_alternative<Object>(v); }

    bool as_bool(bool def = false) const { return is_bool() ? std::get<bool>(v) : def; }
    double as_number(double def = 0.0) const { return is_number() ? std::get<double>(v) : def; }
    int64_t as_int(int64_t def = 0) const {
        if (!is_number()) return def;
        double d = std::get<double>(v);
        // double→int64 仅在可表示范围内有定义（[conv.fpint]）；超范围（如 1e30）或非有限值直接回退 def，避免 UB
        if (!std::isfinite(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0) return def;
        return static_cast<int64_t>(d);
    }
    // 无参版本返回内部或空字符串的引用；带默认值版本按值返回，避免悬挂
    const std::string& as_string() const {
        static const std::string empty;
        return is_string() ? std::get<std::string>(v) : empty;
    }
    std::string as_string(const char* def) const {
        return is_string() ? std::get<std::string>(v) : std::string(def ? def : "");
    }
    const Array& as_array() const {
        static const Array empty;
        return is_array() ? std::get<Array>(v) : empty;
    }
    const Object& as_object() const {
        static const Object empty;
        return is_object() ? std::get<Object>(v) : empty;
    }

    // 对象取键；非对象或缺键时返回空 Value（null）
    const Value& get(const std::string& key) const {
        static const Value null;
        if (!is_object()) return null;
        const Object& o = std::get<Object>(v);
        auto it = o.find(key);
        return it == o.end() ? null : it->second;
    }
    // 对象取键并回退默认
    const Value& get(const std::string& key, const Value& fallback) const {
        if (!is_object()) return fallback;
        const Object& o = std::get<Object>(v);
        auto it = o.find(key);
        return it == o.end() ? fallback : it->second;
    }
};

// 解析文本；失败时返回 null 并设置 err
Value parse(const std::string& text, std::string* err = nullptr);
// 序列化为紧凑 JSON（字符串一律用双引号）
std::string dump(const Value& value);
// 序列化为带缩进的美化 JSON（2 空格缩进），供写盘/配置等需人工可读的场景
std::string dump_pretty(const Value& value);

}  // namespace json
}  // namespace wsmud
