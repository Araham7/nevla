#include "nevla/schema.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <regex>

namespace nevla {

/* ---------------- Field builders ---------------- */
Field &Field::required(bool on, std::string message) {
    required_ = on;
    requiredMessage_ = std::move(message);
    return *this;
}
Field &Field::def(Value value) {
    default_ = std::move(value);
    return *this;
}
Field &Field::defFn(std::function<Value()> make) {
    defaultFn_ = std::move(make);
    return *this;
}
Field &Field::enumOf(std::vector<Value> allowed) {
    enum_ = std::move(allowed);
    return *this;
}
Field &Field::min(double v) {
    min_ = v;
    return *this;
}
Field &Field::max(double v) {
    max_ = v;
    return *this;
}
Field &Field::minLength(size_t n) {
    minLength_ = n;
    return *this;
}
Field &Field::maxLength(size_t n) {
    maxLength_ = n;
    return *this;
}
Field &Field::match(std::string regex, std::string message) {
    match_ = std::move(regex);
    matchMessage_ = std::move(message);
    return *this;
}
Field &Field::trim(bool on) {
    trim_ = on;
    return *this;
}
Field &Field::lowercase(bool on) {
    lowercase_ = on;
    return *this;
}
Field &Field::uppercase(bool on) {
    uppercase_ = on;
    return *this;
}
Field &Field::unique(bool on) {
    unique_ = on;
    return *this;
}
Field &Field::index(bool on) {
    index_ = on;
    return *this;
}
Field &Field::select(bool on) {
    select_ = on;
    return *this;
}
Field &Field::immutable(bool on) {
    immutable_ = on;
    return *this;
}
Field &Field::validate(std::function<bool(const Value &)> fn, std::string message) {
    validators_.emplace_back(std::move(fn), std::move(message));
    return *this;
}
Field &Field::set(std::function<Value(const Value &)> fn) {
    setter_ = std::move(fn);
    return *this;
}
Field &Field::ref(std::string modelName) {
    ref_ = std::move(modelName);
    return *this;
}

static Field make(FieldType t) {
    Field f;
    f.type = t;
    return f;
}
Field String() { return make(FieldType::String); }
Field Number() { return make(FieldType::Number); }
Field Int() { return make(FieldType::Int); }
Field Boolean() { return make(FieldType::Boolean); }
Field DateTime() { return make(FieldType::Date); }
Field Id() { return make(FieldType::ObjectId); }
Field Mixed() { return make(FieldType::Mixed); }
Field Array(Field of) {
    Field f = make(FieldType::Array);
    f.of_ = std::make_shared<Field>(std::move(of));
    return f;
}
Field Array(const Schema &schema) {
    Field f = make(FieldType::Array);
    f.schema_ = std::make_shared<Schema>(schema);
    return f;
}
Field Object(const Schema &schema) {
    Field f = make(FieldType::Object);
    f.schema_ = std::make_shared<Schema>(schema);
    return f;
}

/* ---------------- Schema ---------------- */
Schema::Schema(std::initializer_list<std::pair<std::string, Field>> fields, SchemaOptions opts)
    : options(std::move(opts)) {
    for (const auto &f : fields) add(f.first, f.second);
}

Schema &Schema::add(const std::string &name, Field field) {
    for (auto &f : fields_)
        if (f.first == name) {
            f.second = std::move(field);
            return *this;
        }
    fields_.emplace_back(name, std::move(field));
    return *this;
}

Schema &Schema::add(const Schema &other) {
    for (const auto &f : other.fields_) add(f.first, f.second);
    for (const auto &i : other.indexes_) indexes_.push_back(i);
    for (const auto &h : other.preDoc_) preDoc_[h.first].insert(preDoc_[h.first].end(), h.second.begin(), h.second.end());
    for (const auto &h : other.postDoc_)
        postDoc_[h.first].insert(postDoc_[h.first].end(), h.second.begin(), h.second.end());
    for (const auto &h : other.preQuery_)
        preQuery_[h.first].insert(preQuery_[h.first].end(), h.second.begin(), h.second.end());
    for (const auto &v : other.virtuals_) virtuals_.push_back(v);
    for (const auto &m : other.methods_) methods_[m.first] = m.second;
    return *this;
}

Schema &Schema::index(Value keys, Value opts) {
    indexes_.push_back({std::move(keys), opts.isObject() ? std::move(opts) : Value::object()});
    return *this;
}
Schema &Schema::pre(const std::string &hook, DocHook fn) {
    preDoc_[hook].push_back(std::move(fn));
    return *this;
}
Schema &Schema::post(const std::string &hook, DocHook fn) {
    postDoc_[hook].push_back(std::move(fn));
    return *this;
}
Schema &Schema::pre(const std::string &hook, QueryHook fn) {
    preQuery_[hook].push_back(std::move(fn));
    return *this;
}
Schema &Schema::virtual_(const std::string &name, std::function<Value(const Document &)> getter) {
    virtuals_.emplace_back(name, std::move(getter));
    return *this;
}
Schema &Schema::method(const std::string &name, std::function<Value(Document &, const Value &)> fn) {
    methods_[name] = std::move(fn);
    return *this;
}
Schema &Schema::toJSON(std::function<void(const Document &, Value &)> transform) {
    jsonTransform_ = std::move(transform);
    return *this;
}

bool Schema::hasOwnId() const {
    for (const auto &f : fields_)
        if (f.first == "_id") return true;
    return false;
}

const Field *Schema::field(const std::string &name) const {
    for (const auto &f : fields_)
        if (f.first == name) return &f.second;
    static const Field idField = Id();
    static const Field dateField = DateTime();
    if (name == "_id") return &idField;
    if (options.timestamps && (name == "createdAt" || name == "updatedAt")) return &dateField;
    return nullptr;
}

const Field *Schema::path(std::string_view dotted, bool *underMixed) const {
    static const Field mixed = Mixed();
    if (underMixed) *underMixed = false;
    const Schema *sc = this;   // abhi kis schema ke andar hain (ya nullptr = field "f" ke andar)
    const Field *f = nullptr;
    for (;;) {
        auto dot = dotted.find('.');
        std::string seg(dotted.substr(0, dot));
        bool last = dot == std::string_view::npos;
        bool numeric = !seg.empty() && std::all_of(seg.begin(), seg.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (sc) {
            f = sc->field(seg);
            if (!f) return nullptr;
            sc = nullptr;
        } else if (f->type == FieldType::Mixed) {
            if (underMixed) *underMixed = true;
            return f;
        } else if (f->type == FieldType::Array && numeric) {
            if (f->schema_) {  // "items.0" = poora sub-document
                if (last) {
                    if (underMixed) *underMixed = true;
                    return &mixed;
                }
                sc = f->schema_.get();
            } else {
                f = f->of_ ? f->of_.get() : &mixed;
            }
        } else if ((f->type == FieldType::Object || f->type == FieldType::Array) && f->schema_) {
            f = f->schema_->field(seg);  // "address.city", "steps.deviceId"
            if (!f) return nullptr;
        } else {
            return nullptr;
        }
        if (last) return f;
        dotted.remove_prefix(dot + 1);
    }
}

std::string uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xFFFF), static_cast<unsigned>(a & 0xFFFF),
                  static_cast<unsigned>(b >> 48), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

/* ---------------- cast / defaults / validate ---------------- */
namespace detail {

namespace {
const char *typeName(FieldType t) {
    switch (t) {
        case FieldType::String: return "String";
        case FieldType::Number: return "Number";
        case FieldType::Int: return "Int";
        case FieldType::Boolean: return "Boolean";
        case FieldType::Date: return "Date";
        case FieldType::ObjectId: return "ObjectId";
        case FieldType::Array: return "Array";
        case FieldType::Object: return "Object";
        default: return "Mixed";
    }
}

[[noreturn]] void castFail(const Field &f, const Value &v, const std::string &path) {
    throw CastError(path, std::string("Cast to ") + typeName(f.type) + " failed for value " + v.toJson() +
                              " at path `" + path + "`");
}

std::string trimmed(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

bool parseNumber(const std::string &s, double &out) {
    std::string t = trimmed(s);
    if (t.empty()) return false;
    char *end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}
}  // namespace

Value castValue(const Field &f, const Value &v, const std::string &path) {
    if (v.isNull()) return v;
    Value out;
    switch (f.type) {
        case FieldType::Mixed: out = v; break;
        case FieldType::String: {
            if (v.isArray() || v.isObject()) castFail(f, v, path);
            std::string s = v.str();
            if (f.trim_) s = trimmed(s);
            if (f.lowercase_) std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            if (f.uppercase_) std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
            out = std::move(s);
            break;
        }
        case FieldType::Number:
        case FieldType::Int: {
            double d = 0;
            if (v.isInt()) {
                out = v;
                break;
            }
            if (v.isDouble())
                d = v.asDouble();
            else if (v.isBool())
                d = v.asBool() ? 1 : 0;
            else if (!v.isString() || !parseNumber(v.asString(), d))
                castFail(f, v, path);
            bool whole = std::floor(d) == d && std::fabs(d) < 9.007199254740992e15;
            if (f.type == FieldType::Int && !whole) castFail(f, v, path);
            out = (whole && !v.isDouble()) || f.type == FieldType::Int ? Value(static_cast<int64_t>(d)) : Value(d);
            break;
        }
        case FieldType::Boolean: {
            if (v.isBool())
                out = v;
            else if (v.isNumber() && (v.asDouble() == 0 || v.asDouble() == 1))
                out = v.asDouble() == 1;
            else if (v.isString() && (v.asString() == "true" || v.asString() == "1" || v.asString() == "yes"))
                out = true;
            else if (v.isString() && (v.asString() == "false" || v.asString() == "0" || v.asString() == "no"))
                out = false;
            else
                castFail(f, v, path);
            break;
        }
        case FieldType::Date: {
            if (v.isDate())
                out = v;
            else if (v.isNumber())
                out = Date{v.asInt()};
            else if (v.isString()) {
                auto d = Date::parse(v.asString());
                if (!d) castFail(f, v, path);
                out = *d;
            } else
                castFail(f, v, path);
            break;
        }
        case FieldType::ObjectId: {
            if (v.isObjectId())
                out = v;
            else if (v.isString() && ObjectId::isValid(v.asString()))
                out = ObjectId(v.asString());
            else if (v.isObject() && v.contains("_id"))
                out = castValue(f, v["_id"], path);  // populated document -> uski id
            else
                castFail(f, v, path);
            break;
        }
        case FieldType::Array: {
            Value arr = Value::array();
            auto one = [&](const Value &e, size_t i) {
                std::string p = path + "." + std::to_string(i);
                if (f.schema_) {
                    if (!e.isObject()) castFail(f, e, p);
                    Value sub = castObject(*f.schema_, e, p + ".");
                    applyDefaults(*f.schema_, sub);
                    arr.push_back(std::move(sub));
                } else if (f.of_) {
                    arr.push_back(castValue(*f.of_, e, p));
                } else {
                    arr.push_back(e);
                }
            };
            if (v.isArray()) {
                size_t i = 0;
                for (const auto &e : v.elements()) one(e, i++);
            } else {
                one(v, 0);  // akeli value -> [value] (Mongoose jaisa)
            }
            out = std::move(arr);
            break;
        }
        case FieldType::Object: {
            if (!v.isObject()) castFail(f, v, path);
            out = f.schema_ ? castObject(*f.schema_, v, path + ".") : v;
            break;
        }
    }
    if (f.setter_) out = f.setter_(out);
    return out;
}

Value castObject(const Schema &s, const Value &obj, const std::string &prefix) {
    Value out = Value::object();
    if (!obj.isObject()) return out;
    for (const auto &m : obj.items()) {
        if (m.first == "_id" && !s.hasOwnId()) {
            // schema me _id nahi likha: 24-hex string -> ObjectId, baaki jaisa hai waisa
            bool hex = m.second.isString() && ObjectId::isValid(m.second.asString());
            out["_id"] = hex ? Value(ObjectId(m.second.asString())) : m.second;
            continue;
        }
        if (const Field *f = s.field(m.first))
            out[m.first] = castValue(*f, m.second, prefix + m.first);
        else if (!s.options.strict || m.first == s.options.versionKey || m.first == s.options.discriminatorKey)
            out[m.first] = m.second;
        // strict: anjaan key chhod di
    }
    return out;
}

void applyDefaults(const Schema &s, Value &obj) {
    if (!obj.isObject()) obj = Value::object();
    for (const auto &kv : s.fields()) {
        const Field &f = kv.second;
        Value *cur = obj.find(kv.first);
        if (!cur || cur->isNull()) {
            if (f.defaultFn_)
                obj[kv.first] = castValue(f, f.defaultFn_(), kv.first);
            else if (f.default_)
                obj[kv.first] = castValue(f, *f.default_, kv.first);
            else if (f.type == FieldType::Array && !cur)
                obj[kv.first] = Value::array();
            else if (f.type == FieldType::Object && f.schema_ && !cur) {
                Value sub = Value::object();
                applyDefaults(*f.schema_, sub);
                if (!sub.empty()) obj[kv.first] = std::move(sub);
            }
        } else if (f.type == FieldType::Object && f.schema_ && cur->isObject()) {
            applyDefaults(*f.schema_, *cur);
        }
    }
}

void validateValue(const Field &f, const Value *v, const std::string &path, std::map<std::string, std::string> &errors) {
    bool missing = !v || v->isNull() || (f.type == FieldType::String && v->isString() && v->asString().empty()) ||
                   (f.type == FieldType::Array && v->isArray() && v->elements().empty() && f.required_);
    if (missing) {
        if (f.required_)
            errors[path] = f.requiredMessage_.empty() ? "Path `" + path + "` is required." : f.requiredMessage_;
        return;
    }
    if (!f.enum_.empty() && std::find(f.enum_.begin(), f.enum_.end(), *v) == f.enum_.end()) {
        errors[path] = "`" + v->str() + "` is not a valid enum value for path `" + path + "`.";
        return;
    }
    if (v->isNumber()) {
        if (f.min_ && v->asDouble() < *f.min_)
            errors[path] = "Path `" + path + "` (" + v->str() + ") is less than minimum allowed value (" +
                           Value(*f.min_).str() + ").";
        if (f.max_ && v->asDouble() > *f.max_)
            errors[path] = "Path `" + path + "` (" + v->str() + ") is more than maximum allowed value (" +
                           Value(*f.max_).str() + ").";
    }
    if (v->isString()) {
        const std::string &s = v->asString();
        if (f.minLength_ && s.size() < *f.minLength_)
            errors[path] = "Path `" + path + "` (`" + s + "`) is shorter than the minimum allowed length (" +
                           std::to_string(*f.minLength_) + ").";
        if (f.maxLength_ && s.size() > *f.maxLength_)
            errors[path] = "Path `" + path + "` (`" + s + "`) is longer than the maximum allowed length (" +
                           std::to_string(*f.maxLength_) + ").";
        if (!f.match_.empty()) {
            bool ok = false;
            try {
                ok = std::regex_search(s, std::regex(f.match_));
            } catch (const std::regex_error &) {
                errors[path] = "Schema ka regex galat hai for path `" + path + "`: " + f.match_;
                return;
            }
            if (!ok) errors[path] = f.matchMessage_.empty() ? "Path `" + path + "` is invalid (" + s + ")." : f.matchMessage_;
        }
    }
    for (const auto &val : f.validators_)
        if (!val.first(*v))
            errors[path] = val.second.empty() ? "Validator failed for path `" + path + "` with value `" + v->str() + "`"
                                              : val.second;
    if (f.type == FieldType::Array && v->isArray()) {
        size_t i = 0;
        for (const auto &e : v->elements()) {
            std::string p = path + "." + std::to_string(i++);
            if (f.schema_)
                validateObject(*f.schema_, e, p + ".", errors);
            else if (f.of_)
                validateValue(*f.of_, &e, p, errors);
        }
    }
    if (f.type == FieldType::Object && f.schema_ && v->isObject()) validateObject(*f.schema_, *v, path + ".", errors);
}

void validateObject(const Schema &s, const Value &obj, const std::string &prefix,
                    std::map<std::string, std::string> &errors) {
    for (const auto &kv : s.fields()) {
        const Value *v = obj.find(kv.first);
        if (kv.second.type == FieldType::Object && kv.second.schema_ && !v) {
            // nested object hi nahi hai: uske required fields check karo
            validateObject(*kv.second.schema_, Value::object(), prefix + kv.first + ".", errors);
            if (kv.second.required_) errors[prefix + kv.first] = "Path `" + prefix + kv.first + "` is required.";
            continue;
        }
        validateValue(kv.second, v, prefix + kv.first, errors);
    }
}

void hiddenPaths(const Schema &s, const std::string &prefix, std::vector<std::string> &out) {
    for (const auto &kv : s.fields()) {
        if (!kv.second.select_)
            out.push_back(prefix + kv.first);
        else if (kv.second.type == FieldType::Object && kv.second.schema_)
            hiddenPaths(*kv.second.schema_, prefix + kv.first + ".", out);
    }
}

}  // namespace detail
}  // namespace nevla
