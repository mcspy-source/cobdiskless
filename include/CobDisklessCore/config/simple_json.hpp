// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <sstream>
#include <optional>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace cobdiskless {

enum class JsonType {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

class JsonValue {
public:
    JsonType type_{JsonType::Null};
    bool bool_val_{false};
    double num_val_{0.0};
    std::string str_val_{};
    std::vector<JsonValue> arr_val_{};
    std::vector<std::pair<std::string, JsonValue>> obj_val_{};

    JsonValue() : type_(JsonType::Null) {}
    JsonValue(std::nullptr_t) : type_(JsonType::Null) {}
    JsonValue(bool b) : type_(JsonType::Boolean), bool_val_(b) {}
    JsonValue(int n) : type_(JsonType::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(unsigned int n) : type_(JsonType::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(long n) : type_(JsonType::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(unsigned long n) : type_(JsonType::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(long long n) : type_(JsonType::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(unsigned long long n) : type_(JsonType::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(double d) : type_(JsonType::Number), num_val_(d) {}
    JsonValue(const char* s) : type_(JsonType::String), str_val_(s ? s : "") {}
    JsonValue(std::string s) : type_(JsonType::String), str_val_(std::move(s)) {}
    JsonValue(std::string_view s) : type_(JsonType::String), str_val_(s) {}

    [[nodiscard]] JsonType type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == JsonType::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == JsonType::Boolean; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == JsonType::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == JsonType::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == JsonType::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == JsonType::Object; }

    [[nodiscard]] bool as_bool(bool def = false) const noexcept {
        return (type_ == JsonType::Boolean) ? bool_val_ : def;
    }
    [[nodiscard]] double as_double(double def = 0.0) const noexcept {
        return (type_ == JsonType::Number) ? num_val_ : def;
    }
    [[nodiscard]] int64_t as_int64(int64_t def = 0) const noexcept {
        return (type_ == JsonType::Number) ? static_cast<int64_t>(num_val_) : def;
    }
    [[nodiscard]] uint64_t as_uint64(uint64_t def = 0) const noexcept {
        return (type_ == JsonType::Number) ? static_cast<uint64_t>(num_val_) : def;
    }
    [[nodiscard]] const std::string& as_string(const std::string& def = "") const noexcept {
        return (type_ == JsonType::String) ? str_val_ : def;
    }

    [[nodiscard]] const std::vector<JsonValue>& as_array() const { return arr_val_; }
    std::vector<JsonValue>& as_array() {
        if (type_ != JsonType::Array) {
            type_ = JsonType::Array;
            arr_val_.clear();
        }
        return arr_val_;
    }

    [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& as_object() const { return obj_val_; }
    std::vector<std::pair<std::string, JsonValue>>& as_object() {
        if (type_ != JsonType::Object) {
            type_ = JsonType::Object;
            obj_val_.clear();
        }
        return obj_val_;
    }

    static JsonValue make_array() {
        JsonValue val;
        val.type_ = JsonType::Array;
        return val;
    }

    static JsonValue make_object() {
        JsonValue val;
        val.type_ = JsonType::Object;
        return val;
    }

    void push_back(JsonValue val) {
        if (type_ != JsonType::Array) {
            type_ = JsonType::Array;
            arr_val_.clear();
        }
        arr_val_.push_back(std::move(val));
    }

    [[nodiscard]] bool contains(const std::string& key) const noexcept {
        if (type_ != JsonType::Object) return false;
        for (const auto& [k, _] : obj_val_) {
            if (k == key) return true;
        }
        return false;
    }

    [[nodiscard]] const JsonValue* find(const std::string& key) const noexcept {
        if (type_ != JsonType::Object) return nullptr;
        for (const auto& [k, v] : obj_val_) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    JsonValue* find(const std::string& key) noexcept {
        if (type_ != JsonType::Object) return nullptr;
        for (auto& [k, v] : obj_val_) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    JsonValue& operator[](const std::string& key) {
        if (type_ != JsonType::Object) {
            type_ = JsonType::Object;
            obj_val_.clear();
        }
        for (auto& [k, v] : obj_val_) {
            if (k == key) return v;
        }
        obj_val_.emplace_back(key, JsonValue());
        return obj_val_.back().second;
    }

    const JsonValue& operator[](const std::string& key) const {
        static const JsonValue null_val;
        const auto* ptr = find(key);
        return ptr ? *ptr : null_val;
    }

    JsonValue& operator[](size_t idx) {
        if (type_ != JsonType::Array) {
            type_ = JsonType::Array;
            arr_val_.clear();
        }
        if (idx >= arr_val_.size()) {
            arr_val_.resize(idx + 1);
        }
        return arr_val_[idx];
    }

    const JsonValue& operator[](size_t idx) const {
        static const JsonValue null_val;
        if (type_ != JsonType::Array || idx >= arr_val_.size()) return null_val;
        return arr_val_[idx];
    }

    [[nodiscard]] size_t size() const noexcept {
        if (type_ == JsonType::Array) return arr_val_.size();
        if (type_ == JsonType::Object) return obj_val_.size();
        return 0;
    }

    static std::optional<JsonValue> parse(std::string_view input) {
        size_t idx = 0;
        skip_ws(input, idx);
        if (idx >= input.size()) return std::nullopt;
        auto val = parse_value(input, idx);
        if (!val) return std::nullopt;
        skip_ws(input, idx);
        return val;
    }

    [[nodiscard]] std::string dump(int indent = 2, int current_level = 0) const {
        std::string ind(indent > 0 ? (current_level * indent) : 0, ' ');
        std::string next_ind(indent > 0 ? ((current_level + 1) * indent) : 0, ' ');
        std::string newline = indent > 0 ? "\n" : "";
        std::string space = indent > 0 ? " " : "";

        switch (type_) {
            case JsonType::Null: return "null";
            case JsonType::Boolean: return bool_val_ ? "true" : "false";
            case JsonType::Number: {
                if (std::floor(num_val_) == num_val_ && !std::isinf(num_val_) && !std::isnan(num_val_) &&
                    num_val_ >= -9223372036854775808.0 && num_val_ <= 18446744073709551615.0) {
                    if (num_val_ < 0) {
                        return std::to_string(static_cast<int64_t>(num_val_));
                    } else {
                        return std::to_string(static_cast<uint64_t>(num_val_));
                    }
                }
                std::ostringstream ss;
                ss << std::setprecision(15) << num_val_;
                return ss.str();
            }
            case JsonType::String: {
                std::string out = "\"";
                for (char c : str_val_) {
                    switch (c) {
                        case '"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\b': out += "\\b"; break;
                        case '\f': out += "\\f"; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default:
                            if (static_cast<unsigned char>(c) < 0x20) {
                                char buf[8];
                                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                                out += buf;
                            } else {
                                out += c;
                            }
                            break;
                    }
                }
                out += "\"";
                return out;
            }
            case JsonType::Array: {
                if (arr_val_.empty()) return "[]";
                std::string out = "[" + newline;
                for (size_t i = 0; i < arr_val_.size(); ++i) {
                    out += next_ind + arr_val_[i].dump(indent, current_level + 1);
                    if (i + 1 < arr_val_.size()) out += ",";
                    out += newline;
                }
                out += ind + "]";
                return out;
            }
            case JsonType::Object: {
                if (obj_val_.empty()) return "{}";
                std::string out = "{" + newline;
                for (size_t i = 0; i < obj_val_.size(); ++i) {
                    out += next_ind + "\"" + obj_val_[i].first + "\":" + space +
                           obj_val_[i].second.dump(indent, current_level + 1);
                    if (i + 1 < obj_val_.size()) out += ",";
                    out += newline;
                }
                out += ind + "}";
                return out;
            }
        }
        return "null";
    }

private:
    static void skip_ws(std::string_view s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
    }

    static std::optional<std::string> parse_string(std::string_view s, size_t& i) {
        if (i >= s.size() || s[i] != '"') return std::nullopt;
        ++i;
        std::string res;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') {
                return res;
            }
            if (c == '\\') {
                if (i >= s.size()) return std::nullopt;
                char esc = s[i++];
                switch (esc) {
                    case '"': res += '"'; break;
                    case '\\': res += '\\'; break;
                    case '/': res += '/'; break;
                    case 'b': res += '\b'; break;
                    case 'f': res += '\f'; break;
                    case 'n': res += '\n'; break;
                    case 'r': res += '\r'; break;
                    case 't': res += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return std::nullopt;
                        std::string hex_str(s.substr(i, 4));
                        i += 4;
                        try {
                            unsigned long code = std::stoul(hex_str, nullptr, 16);
                            if (code < 128) {
                                res += static_cast<char>(code);
                            } else {
                                res += '?';
                            }
                        } catch (...) {
                            return std::nullopt;
                        }
                        break;
                    }
                    default: res += esc; break;
                }
            } else {
                res += c;
            }
        }
        return std::nullopt;
    }

    static std::optional<JsonValue> parse_number(std::string_view s, size_t& i) {
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        std::string num_str(s.substr(start, i - start));
        try {
            double val = std::stod(num_str);
            return JsonValue(val);
        } catch (...) {
            return std::nullopt;
        }
    }

    static std::optional<JsonValue> parse_array(std::string_view s, size_t& i) {
        if (i >= s.size() || s[i] != '[') return std::nullopt;
        ++i;
        JsonValue arr = JsonValue::make_array();
        skip_ws(s, i);
        if (i < s.size() && s[i] == ']') {
            ++i;
            return arr;
        }
        while (i < s.size()) {
            auto elem = parse_value(s, i);
            if (!elem) return std::nullopt;
            arr.push_back(std::move(*elem));
            skip_ws(s, i);
            if (i < s.size() && s[i] == ']') {
                ++i;
                return arr;
            }
            if (i >= s.size() || s[i] != ',') return std::nullopt;
            ++i;
            skip_ws(s, i);
        }
        return std::nullopt;
    }

    static std::optional<JsonValue> parse_object(std::string_view s, size_t& i) {
        if (i >= s.size() || s[i] != '{') return std::nullopt;
        ++i;
        JsonValue obj = JsonValue::make_object();
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            return obj;
        }
        while (i < s.size()) {
            skip_ws(s, i);
            auto key = parse_string(s, i);
            if (!key) return std::nullopt;
            skip_ws(s, i);
            if (i >= s.size() || s[i] != ':') return std::nullopt;
            ++i;
            skip_ws(s, i);
            auto val = parse_value(s, i);
            if (!val) return std::nullopt;
            obj[*key] = std::move(*val);
            skip_ws(s, i);
            if (i < s.size() && s[i] == '}') {
                ++i;
                return obj;
            }
            if (i >= s.size() || s[i] != ',') return std::nullopt;
            ++i;
            skip_ws(s, i);
        }
        return std::nullopt;
    }

    static std::optional<JsonValue> parse_value(std::string_view s, size_t& i) {
        skip_ws(s, i);
        if (i >= s.size()) return std::nullopt;

        char c = s[i];
        if (c == '"') {
            auto str = parse_string(s, i);
            if (str) return JsonValue(std::move(*str));
            return std::nullopt;
        }
        if (c == '{') return parse_object(s, i);
        if (c == '[') return parse_array(s, i);
        if (c == 't' && s.substr(i, 4) == "true") {
            i += 4;
            return JsonValue(true);
        }
        if (c == 'f' && s.substr(i, 5) == "false") {
            i += 5;
            return JsonValue(false);
        }
        if (c == 'n' && s.substr(i, 4) == "null") {
            i += 4;
            return JsonValue();
        }
        if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number(s, i);
        }

        return std::nullopt;
    }
};

} // namespace cobdiskless
