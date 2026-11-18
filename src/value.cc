#include "nevla/value.hpp"

#include <bsoncxx/builder/core.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "bson.hpp"

namespace nevla {

/* ---------------- Date ---------------- */
Date Date::now() {
    return {std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count()};
}

std::string Date::iso() const {
    time_t secs = static_cast<time_t>(ms / 1000);
    int milli = static_cast<int>(ms % 1000);
    if (milli < 0) {
        milli += 1000;
        secs -= 1;
    }
    struct tm t;
    gmtime_r(&secs, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec, milli);
    return buf;
}

std::optional<Date> Date::parse(std::string_view s) {
    size_t i = 0;
    auto digits = [&](int n, int &out) {
        if (i + n > s.size()) return false;
        out = 0;
        for (int k = 0; k < n; k++) {
            char c = s[i + k];
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        i += n;
        return true;
    };
    int Y, M, D, h = 0, m = 0, sec = 0, ms = 0, offsetMin = 0;
    if (!digits(4, Y) || i >= s.size() || s[i++] != '-' || !digits(2, M) || i >= s.size() || s[i++] != '-' ||
        !digits(2, D))
        return std::nullopt;
    if (i < s.size()) {
        if (s[i] != 'T' && s[i] != ' ') return std::nullopt;
        i++;
        if (!digits(2, h) || i >= s.size() || s[i++] != ':' || !digits(2, m)) return std::nullopt;
        if (i < s.size() && s[i] == ':') {
            i++;
            if (!digits(2, sec)) return std::nullopt;
            if (i < s.size() && s[i] == '.') {
                i++;
                int n = 0;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                    if (n < 3) ms = ms * 10 + (s[i] - '0');
                    n++;
                    i++;
                }
                for (; n < 3; n++) ms *= 10;
            }
        }
        if (i < s.size()) {
            if (s[i] == 'Z') {
                i++;
            } else if (s[i] == '+' || s[i] == '-') {
                int sign = s[i++] == '-' ? -1 : 1, oh = 0, om = 0;
                if (!digits(2, oh)) return std::nullopt;
                if (i < s.size() && s[i] == ':') i++;
                if (!digits(2, om)) return std::nullopt;
                offsetMin = sign * (oh * 60 + om);
            }
        }
        if (i != s.size()) return std::nullopt;
    }
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0;
    int maxDay = M >= 1 && M <= 12 ? kDays[M - 1] + (M == 2 && leap ? 1 : 0) : 0;
    // "2026-02-31" is rejected (instead of silently becoming March 3); 24:00:00 only as end of day
    if (M < 1 || M > 12 || D < 1 || D > maxDay || h > 24 || m > 59 || sec > 60 || (h == 24 && (m || sec || ms)))
        return std::nullopt;
    struct tm t {};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = sec;
    return Date{static_cast<int64_t>(timegm(&t)) * 1000 + ms - static_cast<int64_t>(offsetMin) * 60000};
}

/* ---------------- ObjectId ---------------- */
bool ObjectId::isValid(std::string_view hex) {
    if (hex.size() != 24) return false;
    for (char c : hex)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

ObjectId::ObjectId(std::string hex) : hex_(std::move(hex)) {
    if (!isValid(hex_)) throw Error("Invalid ObjectId: \"" + hex_ + "\" (expected 24 hex characters)");
    for (auto &c : hex_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

ObjectId ObjectId::generate() { return ObjectId(bsoncxx::oid{}.to_string()); }

/* ---------------- Value ---------------- */
namespace {
const Value kNull;
const std::string kEmptyString;
const Value::Array kEmptyArray;
const Value::Object kEmptyObject;

std::string numberText(double d) {
    if (!std::isfinite(d)) return "null";
    if (std::floor(d) == d && std::fabs(d) < 9.007199254740992e15) return std::to_string(static_cast<int64_t>(d));
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), d);
    return std::string(buf, r.ptr);
}

void escape(std::string &out, std::string_view s) {
    static const char *hex = "0123456789abcdef";
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void write(std::string &out, const Value &v, int indent, int depth) {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * d), ' ');
    };
    switch (v.type()) {
        case Value::Type::Null:
            out += "null";
            break;
        case Value::Type::Bool:
            out += v.asBool() ? "true" : "false";
            break;
        case Value::Type::Int:
            out += std::to_string(v.asInt());
            break;
        case Value::Type::Double:
            out += numberText(v.asDouble());
            break;
        case Value::Type::String:
            escape(out, v.asString());
            break;
        case Value::Type::Date:
            escape(out, v.asDate().iso());
            break;
        case Value::Type::ObjectId:
            escape(out, v.asObjectId().str());
            break;
        case Value::Type::Array: {
            if (v.elements().empty()) {
                out += "[]";
                break;
            }
            out += '[';
            bool first = true;
            for (const auto &e : v.elements()) {
                if (!first) out += ',';
                first = false;
                newline(depth + 1);
                write(out, e, indent, depth + 1);
            }
            newline(depth);
            out += ']';
            break;
        }
        case Value::Type::Object: {
            if (v.items().empty()) {
                out += "{}";
                break;
            }
            out += '{';
            bool first = true;
            for (const auto &m : v.items()) {
                if (!first) out += ',';
                first = false;
                newline(depth + 1);
                escape(out, m.first);
                out += indent < 0 ? ":" : ": ";
                write(out, m.second, indent, depth + 1);
            }
            newline(depth);
            out += '}';
            break;
        }
    }
}

bool isIndex(std::string_view s, size_t &out) {
    if (s.empty() || s.size() > 9) return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<size_t>(c - '0');
    }
    return true;
}
}  // namespace

Value::Value(std::initializer_list<Value> init) {
    bool isObj = init.size() > 0;
    for (const auto &e : init)
        if (!(e.isArray() && e.elements().size() == 2 && e.elements()[0].isString())) {
            isObj = false;
            break;
        }
    if (isObj) {
        Object o;
        for (const auto &e : init) {
            const std::string &key = e.elements()[0].asString();
            bool replaced = false;
            for (auto &m : o)
                if (m.first == key) {
                    m.second = e.elements()[1];
                    replaced = true;
                }
            if (!replaced) o.emplace_back(key, e.elements()[1]);
        }
        v_ = std::move(o);
    } else {
        v_ = Array(init.begin(), init.end());
    }
}

Value Value::object() {
    Value v;
    v.v_ = Object{};
    return v;
}

Value Value::array(std::initializer_list<Value> list) {
    Value v;
    v.v_ = Array(list.begin(), list.end());
    return v;
}

Value Value::parse(std::string_view json) {
    // bsoncxx only parses an object at the top level: wrap scalars
    try {
        std::string wrapped = "{\"v\":" + std::string(json) + "}";
        auto doc = bsoncxx::from_json(wrapped);
        return detail::fromBson(doc.view()["v"].get_value());
    } catch (const bsoncxx::exception &e) {
        throw Error(std::string("Invalid JSON: ") + e.what());
    }
}

bool Value::asBool(bool fallback) const {
    switch (type()) {
        case Type::Bool:
            return std::get<bool>(v_);
        case Type::Int:
            return std::get<int64_t>(v_) != 0;
        case Type::Double:
            return std::get<double>(v_) != 0;
        case Type::String: {
            const auto &s = std::get<std::string>(v_);
            if (s == "true" || s == "1" || s == "yes") return true;
            if (s == "false" || s == "0" || s == "no") return false;
            return fallback;
        }
        default:
            return fallback;
    }
}

int64_t Value::asInt(int64_t fallback) const {
    switch (type()) {
        case Type::Int:
            return std::get<int64_t>(v_);
        case Type::Double:
            return static_cast<int64_t>(std::get<double>(v_));
        case Type::Bool:
            return std::get<bool>(v_) ? 1 : 0;
        case Type::Date:
            return std::get<Date>(v_).ms;
        case Type::String: {
            const auto &s = std::get<std::string>(v_);
            char *end = nullptr;
            double d = std::strtod(s.c_str(), &end);
            return (end && end != s.c_str() && *end == '\0') ? static_cast<int64_t>(d) : fallback;
        }
        default:
            return fallback;
    }
}

double Value::asDouble(double fallback) const {
    switch (type()) {
        case Type::Int:
            return static_cast<double>(std::get<int64_t>(v_));
        case Type::Double:
            return std::get<double>(v_);
        case Type::Bool:
            return std::get<bool>(v_) ? 1 : 0;
        case Type::Date:
            return static_cast<double>(std::get<Date>(v_).ms);
        case Type::String: {
            const auto &s = std::get<std::string>(v_);
            char *end = nullptr;
            double d = std::strtod(s.c_str(), &end);
            return (end && end != s.c_str() && *end == '\0') ? d : fallback;
        }
        default:
            return fallback;
    }
}

const std::string &Value::asString() const { return isString() ? std::get<std::string>(v_) : kEmptyString; }

std::string Value::str() const {
    switch (type()) {
        case Type::String:
            return std::get<std::string>(v_);
        case Type::Bool:
            return std::get<bool>(v_) ? "true" : "false";
        case Type::Int:
            return std::to_string(std::get<int64_t>(v_));
        case Type::Double:
            return numberText(std::get<double>(v_));
        case Type::Date:
            return std::get<Date>(v_).iso();
        case Type::ObjectId:
            return std::get<ObjectId>(v_).str();
        case Type::Null:
            return "";
        default:
            return toJson();
    }
}

Date Value::asDate() const {
    if (isDate()) return std::get<Date>(v_);
    if (isNumber()) return Date{asInt()};
    if (isString())
        if (auto d = Date::parse(asString())) return *d;
    return Date{};
}

ObjectId Value::asObjectId() const {
    if (isObjectId()) return std::get<ObjectId>(v_);
    if (isString() && ObjectId::isValid(asString())) return ObjectId(asString());
    return ObjectId{};
}

Value &Value::operator[](const std::string &key) {
    if (isNull()) v_ = Object{};
    if (!isObject()) throw Error("Value[\"" + key + "\"]: not an object");
    auto &o = std::get<Object>(v_);
    for (auto &m : o)
        if (m.first == key) return m.second;
    o.emplace_back(key, Value());
    return o.back().second;
}

const Value &Value::operator[](const std::string &key) const {
    const Value *p = find(key);
    return p ? *p : kNull;
}

Value *Value::find(std::string_view key) {
    if (!isObject()) return nullptr;
    for (auto &m : std::get<Object>(v_))
        if (m.first == key) return &m.second;
    return nullptr;
}

const Value *Value::find(std::string_view key) const { return const_cast<Value *>(this)->find(key); }

bool Value::erase(std::string_view key) {
    if (!isObject()) return false;
    auto &o = std::get<Object>(v_);
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it->first == key) {
            o.erase(it);
            return true;
        }
    return false;
}

Value::Object &Value::items() & {
    if (isNull()) v_ = Object{};
    if (!isObject()) throw Error("Value::items(): not an object");
    return std::get<Object>(v_);
}
const Value::Object &Value::items() const & { return isObject() ? std::get<Object>(v_) : kEmptyObject; }
Value::Object Value::items() && { return isObject() ? std::move(std::get<Object>(v_)) : Object{}; }

void Value::push_back(Value v) {
    if (isNull()) v_ = Array{};
    if (!isArray()) throw Error("Value::push_back(): not an array");
    std::get<Array>(v_).push_back(std::move(v));
}

Value &Value::at(size_t i) {
    if (!isArray() || i >= std::get<Array>(v_).size())
        throw Error("Value::at(" + std::to_string(i) + "): index out of range");
    return std::get<Array>(v_)[i];
}
const Value &Value::at(size_t i) const { return const_cast<Value *>(this)->at(i); }

Value::Array &Value::elements() & {
    if (isNull()) v_ = Array{};
    if (!isArray()) throw Error("Value::elements(): not an array");
    return std::get<Array>(v_);
}
const Value::Array &Value::elements() const & { return isArray() ? std::get<Array>(v_) : kEmptyArray; }
Value::Array Value::elements() && { return isArray() ? std::move(std::get<Array>(v_)) : Array{}; }

size_t Value::size() const {
    if (isArray()) return std::get<Array>(v_).size();
    if (isObject()) return std::get<Object>(v_).size();
    if (isString()) return std::get<std::string>(v_).size();
    return 0;
}

const Value *Value::path(std::string_view dotted) const {
    const Value *cur = this;
    while (cur) {
        auto dot = dotted.find('.');
        std::string_view seg = dotted.substr(0, dot);
        size_t idx = 0;
        if (cur->isArray() && isIndex(seg, idx))
            cur = idx < cur->elements().size() ? &cur->elements()[idx] : nullptr;
        else
            cur = cur->find(seg);
        if (dot == std::string_view::npos) break;
        dotted.remove_prefix(dot + 1);
    }
    return cur;
}

Value &Value::pathRef(std::string_view dotted) {
    Value *cur = this;
    for (;;) {
        auto dot = dotted.find('.');
        std::string_view seg = dotted.substr(0, dot);
        size_t idx = 0;
        if (cur->isArray() && isIndex(seg, idx)) {
            auto &arr = cur->elements();
            if (idx > arr.size()) throw Error("path \"" + std::string(dotted) + "\": array index out of range");
            if (idx == arr.size()) arr.emplace_back();
            cur = &arr[idx];
        } else {
            if (!cur->isObject() && !cur->isNull()) *cur = Value::object();
            cur = &(*cur)[std::string(seg)];
        }
        if (dot == std::string_view::npos) return *cur;
        dotted.remove_prefix(dot + 1);
    }
}

bool Value::erasePath(std::string_view dotted) {
    auto dot = dotted.rfind('.');
    if (dot == std::string_view::npos) return erase(dotted);
    Value *parent = const_cast<Value *>(path(dotted.substr(0, dot)));
    return parent && parent->erase(dotted.substr(dot + 1));
}

bool Value::operator==(const Value &o) const {
    if (isNumber() && o.isNumber()) return asDouble() == o.asDouble();
    return v_ == o.v_;
}

std::string Value::toJson(int indent) const {
    std::string out;
    write(out, *this, indent, 0);
    return out;
}

/* ---------------- BSON ---------------- */
namespace detail {
namespace {
void append(bsoncxx::builder::core &b, const Value &v) {
    switch (v.type()) {
        case Value::Type::Null:
            b.append(bsoncxx::types::b_null{});
            break;
        case Value::Type::Bool:
            b.append(v.asBool());
            break;
        case Value::Type::Int: {
            int64_t n = v.asInt();
            if (n >= INT32_MIN && n <= INT32_MAX)
                b.append(static_cast<int32_t>(n));
            else
                b.append(n);
            break;
        }
        case Value::Type::Double:
            b.append(v.asDouble());
            break;
        case Value::Type::String:
            b.append(v.asString());
            break;
        case Value::Type::Date:
            b.append(bsoncxx::types::b_date{std::chrono::milliseconds{v.asDate().ms}});
            break;
        case Value::Type::ObjectId:
            b.append(bsoncxx::oid{v.asObjectId().str()});
            break;
        case Value::Type::Array:
            b.open_array();
            for (const auto &e : v.elements()) append(b, e);
            b.close_array();
            break;
        case Value::Type::Object:
            b.open_document();
            for (const auto &m : v.items()) {
                b.key_owned(m.first);
                append(b, m.second);
            }
            b.close_document();
            break;
    }
}
}  // namespace

bsoncxx::document::value toBson(const Value &object) {
    bsoncxx::builder::core b(false);
    for (const auto &m : object.items()) {
        b.key_owned(m.first);
        append(b, m.second);
    }
    return b.extract_document();
}

Value fromBson(const bsoncxx::types::bson_value::view &v) {
    using bsoncxx::type;
    switch (v.type()) {
        case type::k_double:
            return v.get_double().value;
        case type::k_string:
            return std::string(v.get_string().value);
        case type::k_bool:
            return v.get_bool().value;
        case type::k_int32:
            return v.get_int32().value;
        case type::k_int64:
            return v.get_int64().value;
        case type::k_date:
            return Date{v.get_date().value.count()};
        case type::k_oid:
            return ObjectId(v.get_oid().value.to_string());
        case type::k_document:
            return fromBson(v.get_document().value);
        case type::k_array: {
            Value arr = Value::array();
            for (const auto &e : v.get_array().value) arr.push_back(fromBson(e.get_value()));
            return arr;
        }
        case type::k_decimal128:
            return v.get_decimal128().value.to_string();
        case type::k_regex:
            return std::string(v.get_regex().regex);
        default:
            return Value();
    }
}

Value fromBson(bsoncxx::document::view doc) {
    Value out = Value::object();
    auto &items = out.items();
    for (const auto &e : doc) items.emplace_back(std::string(e.key()), fromBson(e.get_value()));
    return out;
}
}  // namespace detail

}  // namespace nevla
