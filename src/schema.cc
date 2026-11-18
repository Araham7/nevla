#include "nevla/schema.hpp"

#include <algorithm>
#include <cmath>
#include <random>

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
Field &Field::enumOf(std::vector<Value> allowed, std::string message) {
    enum_ = std::move(allowed);
    enumMessage_ = std::move(message);
    return *this;
}
Field &Field::min(double v, std::string message) {
    min_ = v;
    minIsDate_ = false;
    minMessage_ = std::move(message);
    return *this;
}
Field &Field::max(double v, std::string message) {
    max_ = v;
    maxIsDate_ = false;
    maxMessage_ = std::move(message);
    return *this;
}
Field &Field::min(Date d, std::string message) {
    min_ = static_cast<double>(d.ms);
    minIsDate_ = true;
    minMessage_ = std::move(message);
    return *this;
}
Field &Field::max(Date d, std::string message) {
    max_ = static_cast<double>(d.ms);
    maxIsDate_ = true;
    maxMessage_ = std::move(message);
    return *this;
}
Field &Field::minLength(size_t n, std::string message) {
    minLength_ = n;
    minLengthMessage_ = std::move(message);
    return *this;
}
Field &Field::maxLength(size_t n, std::string message) {
    maxLength_ = n;
    maxLengthMessage_ = std::move(message);
    return *this;
}
Field &Field::match(std::string regex, std::string message) {
    try {
        regex_ = std::make_shared<const std::regex>(regex, std::regex::ECMAScript);
    } catch (const std::regex_error &e) {
        throw Error("nevla: invalid regex in match(\"" + regex + "\"): " + e.what());
    }
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
Field &Field::sparse(bool on) {
    sparse_ = on;
    return *this;
}
Field &Field::expires(int64_t seconds) {
    expires_ = seconds;
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
Field &Field::get(std::function<Value(const Value &)> fn) {
    getter_ = std::move(fn);
    return *this;
}
Field &Field::alias(std::string name) {
    alias_ = std::move(name);
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
Field Map(Field of) {
    Field f = make(FieldType::Map);
    f.of_ = std::make_shared<Field>(std::move(of));
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

template <class M>
static void mergeHooks(M &into, const M &from) {
    for (const auto &h : from) into[h.first].insert(into[h.first].end(), h.second.begin(), h.second.end());
}

Schema &Schema::add(const Schema &other) {
    for (const auto &f : other.fields_) add(f.first, f.second);
    for (const auto &i : other.indexes_) indexes_.push_back(i);
    mergeHooks(preDoc_, other.preDoc_);
    mergeHooks(postDoc_, other.postDoc_);
    mergeHooks(preQuery_, other.preQuery_);
    mergeHooks(postQuery_, other.postQuery_);
    for (const auto &v : other.virtuals_) virtuals_.push_back(v);
    for (const auto &v : other.virtualPopulates_) virtualPopulates_.push_back(v);
    for (const auto &m : other.methods_) methods_[m.first] = m.second;
    for (const auto &m : other.statics_) statics_[m.first] = m.second;
    if (!jsonTransform_ && other.jsonTransform_) jsonTransform_ = other.jsonTransform_;
    return *this;
}

Schema &Schema::remove(const std::string &name) {
    fields_.erase(std::remove_if(fields_.begin(), fields_.end(), [&](const auto &f) { return f.first == name; }),
                  fields_.end());
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
Schema &Schema::post(const std::string &hook, QueryPostHook fn) {
    postQuery_[hook].push_back(std::move(fn));
    return *this;
}
Schema &Schema::virtual_(const std::string &name, std::function<Value(const Document &)> getter) {
    virtuals_.emplace_back(name, std::move(getter));
    return *this;
}
Schema &Schema::virtualPopulate(const std::string &name, VirtualPopulate opts) {
    if (opts.ref.empty() || opts.localField.empty() || opts.foreignField.empty())
        throw Error("nevla: virtualPopulate(\"" + name + "\") needs ref, localField and foreignField");
    virtualPopulates_.emplace_back(name, std::move(opts));
    return *this;
}
Schema &Schema::method(const std::string &name, std::function<Value(Document &, const Value &)> fn) {
    methods_[name] = std::move(fn);
    return *this;
}
Schema &Schema::static_(const std::string &name, StaticFn fn) {
    statics_[name] = std::move(fn);
    return *this;
}
Schema &Schema::plugin(const std::function<void(Schema &)> &fn) {
    fn(*this);
    return *this;
}
Schema &Schema::plugin(const std::function<void(Schema &, const Value &)> &fn, const Value &opts) {
    fn(*this, opts);
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
    if (options.timestamps && (name == options.createdAt || name == options.updatedAt)) return &dateField;
    return nullptr;
}

std::string Schema::resolveAlias(std::string_view p) const {
    auto dot = p.find('.');
    std::string_view head = p.substr(0, dot);
    for (const auto &f : fields_)
        if (!f.second.alias_.empty() && f.second.alias_ == head)
            return f.first + (dot == std::string_view::npos ? "" : std::string(p.substr(dot)));
    return std::string(p);
}

namespace {
bool isNumeric(const std::string &seg) {
    return !seg.empty() && std::all_of(seg.begin(), seg.end(), [](char c) { return c >= '0' && c <= '9'; });
}
// Array position in a path: "0", "$", "$[]", "$[elem]"
bool isPosition(const std::string &seg) {
    return isNumeric(seg) || seg == "$" || (seg.size() >= 3 && seg[0] == '$' && seg[1] == '[' && seg.back() == ']');
}
}  // namespace

const Field *Schema::path(std::string_view dotted, bool *underMixed) const {
    static const Field mixed = Mixed();
    if (underMixed) *underMixed = false;
    const Schema *sc = this;  // inside which schema we are (nullptr = inside field f)
    const Field *f = nullptr;
    for (;;) {
        auto dot = dotted.find('.');
        std::string seg(dotted.substr(0, dot));
        bool last = dot == std::string_view::npos;
        if (sc) {
            f = sc->field(seg);
            if (!f) {
                std::string real = sc->resolveAlias(seg);
                if (real != seg) f = sc->field(real);
            }
            if (!f) return nullptr;
            sc = nullptr;
        } else if (f->type == FieldType::Mixed) {
            if (underMixed) *underMixed = true;
            return f;
        } else if (f->type == FieldType::Map) {
            f = f->of_ ? f->of_.get() : &mixed;  // "prices.inr" -> value type
        } else if (f->type == FieldType::Array && isPosition(seg)) {
            if (f->schema_) {  // "items.0" = a whole sub-document
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
        case FieldType::String:
            return "String";
        case FieldType::Number:
            return "Number";
        case FieldType::Int:
            return "Int";
        case FieldType::Boolean:
            return "Boolean";
        case FieldType::Date:
            return "Date";
        case FieldType::ObjectId:
            return "ObjectId";
        case FieldType::Array:
            return "Array";
        case FieldType::Object:
            return "Object";
        case FieldType::Map:
            return "Map";
        default:
            return "Mixed";
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
    // strtod would also accept "0x10", "inf", "nan": MongoDB / Mongoose do not
    if (t.find_first_of("xXnNiI") != std::string::npos) return false;
    char *end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

void addError(std::map<std::string, std::string> &errors, std::map<std::string, std::string> *kinds,
              const std::string &path, const std::string &kind, const std::string &msg) {
    errors[path] = msg;
    if (kinds) (*kinds)[path] = kind;
}
}  // namespace

std::string formatMessage(std::string msg, const std::string &path, const Value *value) {
    auto replaceAll = [&](const std::string &from, const std::string &to) {
        for (size_t pos = 0; (pos = msg.find(from, pos)) != std::string::npos; pos += to.size())
            msg.replace(pos, from.size(), to);
    };
    replaceAll("{PATH}", path);
    replaceAll("{VALUE}", value ? value->str() : "");
    return msg;
}

Value castValue(const Field &f, const Value &v, const std::string &path) {
    if (v.isNull()) return v;
    Value out;
    switch (f.type) {
        case FieldType::Mixed:
            out = v;
            break;
        case FieldType::String: {
            if (v.isArray() || v.isObject()) castFail(f, v, path);
            std::string s = v.str();
            if (f.trim_) s = trimmed(s);
            if (f.lowercase_)
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            if (f.uppercase_)
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
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
            if (!std::isfinite(d)) castFail(f, v, path);
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
                out = castValue(f, v["_id"], path);  // populated document -> its id
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
                one(v, 0);  // single value -> [value] (like Mongoose)
            }
            out = std::move(arr);
            break;
        }
        case FieldType::Object: {
            if (!v.isObject()) castFail(f, v, path);
            out = f.schema_ ? castObject(*f.schema_, v, path + ".") : v;
            break;
        }
        case FieldType::Map: {
            if (!v.isObject()) castFail(f, v, path);
            Value m = Value::object();
            for (const auto &kv : v.items()) {
                if (kv.first.empty() || kv.first[0] == '$' || kv.first.find('.') != std::string::npos)
                    throw CastError(path, "Map keys cannot be empty, start with '$' or contain '.' (got \"" + kv.first +
                                              "\" at path `" + path + "`)");
                m[kv.first] = f.of_ ? castValue(*f.of_, kv.second, path + "." + kv.first) : kv.second;
            }
            out = std::move(m);
            break;
        }
    }
    if (f.setter_) out = f.setter_(out);
    return out;
}

Value castObject(const Schema &s, const Value &obj, const std::string &prefix,
                 std::map<std::string, std::string> *castErrors) {
    Value out = Value::object();
    if (!obj.isObject()) return out;
    for (const auto &m : obj.items()) {
        if (m.first == "_id" && !s.hasOwnId()) {
            // _id not declared in the schema: 24-hex string -> ObjectId, anything else as is
            bool hex = m.second.isString() && ObjectId::isValid(m.second.asString());
            out["_id"] = hex ? Value(ObjectId(m.second.asString())) : m.second;
            continue;
        }
        std::string key = m.first;
        const Field *f = s.field(key);
        if (!f) {  // alias?
            std::string real = s.resolveAlias(key);
            if (real != key && (f = s.field(real))) key = real;
        }
        if (f) {
            if (!castErrors) {
                out[key] = castValue(*f, m.second, prefix + key);
                continue;
            }
            try {
                out[key] = castValue(*f, m.second, prefix + key);
            } catch (const CastError &e) {
                (*castErrors)[e.path.empty() ? prefix + key : e.path] = e.what();
            }
        } else if (!s.options.strict || key == s.options.versionKey || key == s.options.discriminatorKey) {
            out[key] = m.second;
        }
        // strict: unknown key dropped
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

void validateValue(const Field &f, const Value *v, const std::string &path, std::map<std::string, std::string> &errors,
                   std::map<std::string, std::string> *kinds) {
    auto fail = [&](const std::string &kind, const std::string &custom, const std::string &fallback) {
        addError(errors, kinds, path, kind, custom.empty() ? fallback : formatMessage(custom, path, v));
    };
    bool missing = !v || v->isNull() || (f.type == FieldType::String && v->isString() && v->asString().empty()) ||
                   (f.type == FieldType::Array && v->isArray() && v->elements().empty() && f.required_);
    if (missing) {
        if (f.required_) fail("required", f.requiredMessage_, "Path `" + path + "` is required.");
        return;
    }
    if (!f.enum_.empty() && std::find(f.enum_.begin(), f.enum_.end(), *v) == f.enum_.end()) {
        fail("enum", f.enumMessage_, "`" + v->str() + "` is not a valid enum value for path `" + path + "`.");
        return;
    }
    if (v->isDate() && (f.minIsDate_ || f.maxIsDate_)) {
        double ms = static_cast<double>(v->asDate().ms);
        if (f.min_ && f.minIsDate_ && ms < *f.min_)
            fail("min", f.minMessage_,
                 "Path `" + path + "` (" + v->str() + ") is before minimum allowed value (" +
                     Date{static_cast<int64_t>(*f.min_)}.iso() + ").");
        if (f.max_ && f.maxIsDate_ && ms > *f.max_)
            fail("max", f.maxMessage_,
                 "Path `" + path + "` (" + v->str() + ") is after maximum allowed value (" +
                     Date{static_cast<int64_t>(*f.max_)}.iso() + ").");
    }
    if (v->isNumber()) {
        if (f.min_ && !f.minIsDate_ && v->asDouble() < *f.min_)
            fail("min", f.minMessage_,
                 "Path `" + path + "` (" + v->str() + ") is less than minimum allowed value (" + Value(*f.min_).str() +
                     ").");
        if (f.max_ && !f.maxIsDate_ && v->asDouble() > *f.max_)
            fail("max", f.maxMessage_,
                 "Path `" + path + "` (" + v->str() + ") is more than maximum allowed value (" + Value(*f.max_).str() +
                     ").");
    }
    if (v->isString()) {
        const std::string &s = v->asString();
        if (f.minLength_ && s.size() < *f.minLength_)
            fail("minlength", f.minLengthMessage_,
                 "Path `" + path + "` (`" + s + "`) is shorter than the minimum allowed length (" +
                     std::to_string(*f.minLength_) + ").");
        if (f.maxLength_ && s.size() > *f.maxLength_)
            fail("maxlength", f.maxLengthMessage_,
                 "Path `" + path + "` (`" + s + "`) is longer than the maximum allowed length (" +
                     std::to_string(*f.maxLength_) + ").");
        if (f.regex_ && !std::regex_search(s, *f.regex_))
            fail("regexp", f.matchMessage_, "Path `" + path + "` is invalid (" + s + ").");
    }
    for (const auto &val : f.validators_) {
        bool ok = false;
        try {
            ok = val.first(*v);
        } catch (const std::exception &e) {  // validator threw: treat as failed with its message
            fail("user defined", val.second, e.what());
            continue;
        }
        if (!ok)
            fail("user defined", val.second, "Validator failed for path `" + path + "` with value `" + v->str() + "`");
    }
    if (f.type == FieldType::Array && v->isArray()) {
        size_t i = 0;
        for (const auto &e : v->elements()) {
            std::string p = path + "." + std::to_string(i++);
            if (f.schema_)
                validateObject(*f.schema_, e, p + ".", errors, kinds);
            else if (f.of_)
                validateValue(*f.of_, &e, p, errors, kinds);
        }
    }
    if (f.type == FieldType::Map && v->isObject() && f.of_)
        for (const auto &kv : v->items()) validateValue(*f.of_, &kv.second, path + "." + kv.first, errors, kinds);
    if (f.type == FieldType::Object && f.schema_ && v->isObject())
        validateObject(*f.schema_, *v, path + ".", errors, kinds);
}

void validateObject(const Schema &s, const Value &obj, const std::string &prefix,
                    std::map<std::string, std::string> &errors, std::map<std::string, std::string> *kinds) {
    for (const auto &kv : s.fields()) {
        const Value *v = obj.find(kv.first);
        if (kv.second.type == FieldType::Object && kv.second.schema_ && !v) {
            // nested object missing: still check its required fields
            validateObject(*kv.second.schema_, Value::object(), prefix + kv.first + ".", errors, kinds);
            if (kv.second.required_)
                addError(errors, kinds, prefix + kv.first, "required",
                         kv.second.requiredMessage_.empty()
                             ? "Path `" + prefix + kv.first + "` is required."
                             : formatMessage(kv.second.requiredMessage_, prefix + kv.first, nullptr));
            continue;
        }
        validateValue(kv.second, v, prefix + kv.first, errors, kinds);
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

void applyGetters(const Schema &s, Value &obj) {
    if (!obj.isObject()) return;
    for (const auto &kv : s.fields()) {
        Value *v = obj.find(kv.first);
        if (!v) continue;
        const Field &f = kv.second;
        if (f.type == FieldType::Object && f.schema_) applyGetters(*f.schema_, *v);
        if (f.type == FieldType::Array && f.schema_)
            for (auto &e : v->elements()) applyGetters(*f.schema_, e);
        if ((f.type == FieldType::Array || f.type == FieldType::Map) && f.of_ && f.of_->getter_) {
            if (v->isArray())
                for (auto &e : v->elements()) e = f.of_->getter_(e);
            if (v->isObject())
                for (auto &m : v->items()) m.second = f.of_->getter_(m.second);
        }
        if (f.getter_) *v = f.getter_(*v);
    }
}

}  // namespace detail
}  // namespace nevla

/* ======================= schema export ======================= */
namespace nevla {
namespace {

enum class Flavor { Json, OpenApi };

Value jsonSchemaOf(const Schema &s, bool topLevel, Flavor fl);

Value fieldSchema(const Field &f, Flavor fl) {
    Value out = Value::object();
    bool json = fl == Flavor::Json;
    switch (f.type) {
        case FieldType::String:
            if (json)
                out["bsonType"] = "string";
            else
                out["type"] = "string";
            if (f.minLength_) out["minLength"] = static_cast<int64_t>(*f.minLength_);
            if (f.maxLength_) out["maxLength"] = static_cast<int64_t>(*f.maxLength_);
            if (!f.match_.empty()) out["pattern"] = f.match_;
            break;
        case FieldType::Number:
        case FieldType::Int:
            if (json)
                out["bsonType"] = f.type == FieldType::Int ? Value::array({"int", "long"})
                                                           : Value::array({"double", "int", "long", "decimal"});
            else
                out["type"] = f.type == FieldType::Int ? "integer" : "number";
            if (f.min_ && !f.minIsDate_) out["minimum"] = *f.min_;
            if (f.max_ && !f.maxIsDate_) out["maximum"] = *f.max_;
            break;
        case FieldType::Boolean:
            if (json)
                out["bsonType"] = "bool";
            else
                out["type"] = "boolean";
            break;
        case FieldType::Date:
            if (json)
                out["bsonType"] = "date";
            else {
                out["type"] = "string";
                out["format"] = "date-time";
            }
            break;
        case FieldType::ObjectId:
            if (json)
                out["bsonType"] = "objectId";
            else {
                out["type"] = "string";
                out["pattern"] = "^[0-9a-fA-F]{24}$";
            }
            break;
        case FieldType::Array: {
            if (json)
                out["bsonType"] = "array";
            else
                out["type"] = "array";
            if (f.schema_)
                out["items"] = jsonSchemaOf(*f.schema_, false, fl);
            else if (f.of_)
                out["items"] = fieldSchema(*f.of_, fl);
            if (f.minLength_) out["minItems"] = static_cast<int64_t>(*f.minLength_);
            break;
        }
        case FieldType::Object:
            out = f.schema_ ? jsonSchemaOf(*f.schema_, false, fl) : Value::object();
            if (!f.schema_) {
                if (json)
                    out["bsonType"] = "object";
                else
                    out["type"] = "object";
            }
            break;
        case FieldType::Map:
            if (json)
                out["bsonType"] = "object";
            else
                out["type"] = "object";
            out["additionalProperties"] = f.of_ ? fieldSchema(*f.of_, fl) : Value(true);
            break;
        case FieldType::Mixed:
            break;  // anything
    }
    if (!f.enum_.empty()) {
        Value e = Value::array();
        for (const auto &v : f.enum_) e.push_back(v);
        if (json && !f.required_) e.push_back(Value());  // optional: may be null
        out["enum"] = e;
    }
    // optional fields may hold null (MongoDB only; OpenAPI clients usually omit them instead)
    if (json && !f.required_ && f.type != FieldType::Mixed && out.contains("bsonType")) {
        Value t = out["bsonType"];
        Value arr = t.isArray() ? t : Value::array({t});
        arr.push_back("null");
        out["bsonType"] = arr;
    }
    if (!json && f.type != FieldType::Mixed && !f.required_ && f.default_) out["default"] = *f.default_;
    return out;
}

Value jsonSchemaOf(const Schema &s, bool topLevel, Flavor fl) {
    bool json = fl == Flavor::Json;
    Value out = Value::object();
    if (json)
        out["bsonType"] = "object";
    else
        out["type"] = "object";
    Value props = Value::object(), required = Value::array();
    if (topLevel) {
        if (!s.hasOwnId()) {
            Field id = Id();
            id.required_ = true;
            props["_id"] = fieldSchema(id, fl);
            if (!json) props["_id"]["readOnly"] = true;
        }
        if (s.options.timestamps) {
            for (const auto &n : {s.options.createdAt, s.options.updatedAt}) {
                Field d = DateTime();
                props[n] = fieldSchema(d, fl);
                if (!json) props[n]["readOnly"] = true;
            }
        }
        if (!s.options.versionKey.empty()) {
            Field v = Int();
            props[s.options.versionKey] = fieldSchema(v, fl);
            if (!json) props[s.options.versionKey]["readOnly"] = true;
        }
    }
    for (const auto &kv : s.fields()) {
        props[kv.first] = fieldSchema(kv.second, fl);
        if (kv.second.required_) required.push_back(kv.first);
    }
    if (topLevel && json && !s.hasOwnId()) required.push_back("_id");
    out["properties"] = props;
    if (!required.empty()) out["required"] = required;
    return out;
}

std::string tsType(const Field &f, int indent);

std::string tsObject(const Schema &s, int indent, bool topLevel) {
    std::string pad(indent + 2, ' '), out = "{\n";
    auto line = [&](const std::string &name, const std::string &type, bool optional) {
        bool plain =
            !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_' || name[0] == '$');
        for (char c : name) plain &= std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        out += pad + (plain ? name : Value(name).toJson()) + (optional ? "?: " : ": ") + type + ";\n";
    };
    if (topLevel) {
        if (!s.hasOwnId()) line("_id", "string", false);
        if (s.options.virtuals) line("id", "string", true);
    }
    for (const auto &kv : s.fields()) {
        const Field &f = kv.second;
        bool always = f.required_ || f.default_ || f.defaultFn_;
        line(kv.first, tsType(f, indent + 2), !always);
    }
    if (topLevel) {
        if (s.options.timestamps) {
            line(s.options.createdAt, "string", false);
            line(s.options.updatedAt, "string", false);
        }
        if (!s.options.versionKey.empty()) line(s.options.versionKey, "number", true);
    }
    return out + std::string(indent, ' ') + "}";
}

std::string tsType(const Field &f, int indent) {
    if (!f.enum_.empty()) {
        std::string out;
        for (const auto &v : f.enum_) out += (out.empty() ? "" : " | ") + v.toJson();
        return out;
    }
    switch (f.type) {
        case FieldType::String:
        case FieldType::Date:  // JSON carries dates as ISO strings
        case FieldType::ObjectId:
            return "string";
        case FieldType::Number:
        case FieldType::Int:
            return "number";
        case FieldType::Boolean:
            return "boolean";
        case FieldType::Mixed:
            return "unknown";
        case FieldType::Array: {
            std::string inner = f.schema_ ? tsObject(*f.schema_, indent, false)
                                : f.of_   ? tsType(*f.of_, indent)
                                          : "unknown";
            bool simple = inner.find_first_of(" |") == std::string::npos;
            return simple ? inner + "[]" : "Array<" + inner + ">";
        }
        case FieldType::Object:
            return f.schema_ ? tsObject(*f.schema_, indent, false) : "Record<string, unknown>";
        case FieldType::Map:
            return "Record<string, " + (f.of_ ? tsType(*f.of_, indent) : std::string("unknown")) + ">";
    }
    return "unknown";
}
}  // namespace

Value Schema::toJsonSchema() const { return jsonSchemaOf(*this, true, Flavor::Json); }
Value Schema::toOpenAPI() const { return jsonSchemaOf(*this, true, Flavor::OpenApi); }

std::string Schema::toTypeScript(const std::string &interfaceName) const {
    return "export interface " + interfaceName + " " + tsObject(*this, 0, true) + "\n";
}

}  // namespace nevla
