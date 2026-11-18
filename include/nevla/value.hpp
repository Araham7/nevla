#pragma once
/* nevla::Value — a dynamic JSON/BSON-like value (null, bool, int, double, string, Date, ObjectId, array, object).
   Objects keep their keys in insertion order (like BSON documents).

     Value user = {{"name", "Asha"}, {"age", 21}, {"tags", {"a", "b"}}};   // object
     Value list = {1, 2, 3};                                              // array
     user["age"] = 22;   user.path("address.city");   user.toJson();                                       */

#include <compare>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nevla {

// Base class of every nevla error
class Error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// UTC time, milliseconds since epoch (MongoDB Date)
struct Date {
    int64_t ms = 0;
    static Date now();
    static std::optional<Date> parse(std::string_view iso);  // "2026-01-31", "2026-01-31T10:20:30.123Z", "+05:30"
    std::string iso() const;                                 // "2026-01-31T10:20:30.123Z"
    auto operator<=>(const Date &) const = default;
};

// MongoDB ObjectId (24 hex characters)
class ObjectId {
  public:
    ObjectId() = default;
    explicit ObjectId(std::string hex);  // throws nevla::Error if not 24 hex chars
    static ObjectId generate();
    static bool isValid(std::string_view hex);
    const std::string &str() const { return hex_; }
    bool empty() const { return hex_.empty(); }
    auto operator<=>(const ObjectId &) const = default;

  private:
    std::string hex_;
};

class Value {
  public:
    enum class Type { Null, Bool, Int, Double, String, Date, ObjectId, Array, Object };
    using Array = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    Value(T n) : v_(static_cast<int64_t>(n)) {}
    template <std::floating_point T>
    Value(T d) : v_(static_cast<double>(d)) {}
    Value(const char *s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(Date d) : v_(d) {}
    Value(ObjectId id) : v_(std::move(id)) {}
    Value(Array a) : v_(std::move(a)) {}
    Value(Object o) : v_(std::move(o)) {}
    template <class T>
    Value(const std::vector<T> &items);
    template <class T>
    Value(T *) = delete;  // so a pointer never silently becomes a bool
    /* {{"key", value}, ...} = object;  {a, b, c} = array.
       (If every element is a 2-item array whose first item is a string, it is treated as an object.
        For an ARRAY of such pairs use Value::array({...}).) */
    Value(std::initializer_list<Value> init);

    static Value object();                                        // {}
    static Value array(std::initializer_list<Value> items = {});  // [] ya [..] (hamesha array)
    static Value parse(std::string_view json);                    // JSON text (Extended JSON ok) -> Value; throws Error

    Type type() const { return static_cast<Type>(v_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isInt() const { return type() == Type::Int; }
    bool isDouble() const { return type() == Type::Double; }
    bool isNumber() const { return isInt() || isDouble(); }
    bool isString() const { return type() == Type::String; }
    bool isDate() const { return type() == Type::Date; }
    bool isObjectId() const { return type() == Type::ObjectId; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    // Lenient getters (convert when possible, otherwise return the fallback)
    bool asBool(bool fallback = false) const;
    int64_t asInt(int64_t fallback = 0) const;
    double asDouble(double fallback = 0) const;
    const std::string &asString() const;  // "" if not a string
    std::string str() const;              // text of any scalar ("42", "true", ObjectId hex, ISO date)
    Date asDate() const;
    ObjectId asObjectId() const;
    template <class T>
    T as() const;  // as<int>(), as<std::string>(), as<std::vector<std::string>>(), as<MyStruct>() (NEVLA_STRUCT)

    // Object
    Value &operator[](const std::string &key);              // creates the key if missing (null -> object)
    const Value &operator[](const std::string &key) const;  // null if missing
    Value *find(std::string_view key);
    const Value *find(std::string_view key) const;
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    bool erase(std::string_view key);
    Object &items() &;              // (key, value) pairs of an object
    const Object &items() const &;  // empty if not an object
    Object items() &&;              // on a temporary: a copy (so `for (auto &kv : doc.get("x").items())` is safe)

    // Array
    void push_back(Value v);  // null becomes an array
    Value &at(size_t i);
    const Value &at(size_t i) const;
    Array &elements() &;
    const Array &elements() const &;  // empty if not an array
    Array elements() &&;              // on a temporary: a copy (safe in range-for)

    size_t size() const;  // items of array / object, length of string, otherwise 0
    bool empty() const { return size() == 0; }

    // Dotted path: "address.city", "tags.0"
    const Value *path(std::string_view dotted) const;  // nullptr if missing
    Value &pathRef(std::string_view dotted);           // creates objects on the way
    bool erasePath(std::string_view dotted);

    bool operator==(const Value &other) const;
    std::string toJson(int indent = -1) const;  // Date -> ISO string, ObjectId -> hex string

  private:
    std::variant<std::monostate, bool, int64_t, double, std::string, Date, ObjectId, Array, Object> v_;
};

/* ---- C++ types <-> Value (as<T>() / toValue() / NEVLA_STRUCT are built on these) ---- */
inline void from_value(const Value &v, Value &out) { out = v; }
inline void from_value(const Value &v, std::string &out) { out = v.str(); }
inline void from_value(const Value &v, bool &out) { out = v.asBool(); }
inline void from_value(const Value &v, Date &out) { out = v.asDate(); }
inline void from_value(const Value &v, ObjectId &out) { out = v.asObjectId(); }
template <std::integral T>
    requires(!std::same_as<T, bool>)
void from_value(const Value &v, T &out) {
    out = static_cast<T>(v.asInt());
}
template <std::floating_point T>
void from_value(const Value &v, T &out) {
    out = static_cast<T>(v.asDouble());
}
template <class T>
void from_value(const Value &v, std::optional<T> &out);
template <class T>
void from_value(const Value &v, std::vector<T> &out) {
    out.clear();
    for (const auto &e : v.elements()) {
        T item{};
        from_value(e, item);
        out.push_back(std::move(item));
    }
}
template <class T>
void from_value(const Value &v, std::map<std::string, T> &out) {
    out.clear();
    for (const auto &kv : v.items()) {
        T item{};
        from_value(kv.second, item);
        out.emplace(kv.first, std::move(item));
    }
}
template <class T>
void from_value(const Value &v, std::optional<T> &out) {
    if (v.isNull()) {
        out.reset();
        return;
    }
    T item{};
    from_value(v, item);
    out = std::move(item);
}

template <class T>
    requires std::constructible_from<Value, const T &>
void to_value(Value &out, const T &in) {
    out = Value(in);
}
template <class T>
void to_value(Value &out, const std::optional<T> &in);
template <class T>
    requires(!std::constructible_from<Value, const std::vector<T> &>)
void to_value(Value &out, const std::vector<T> &in) {
    out = Value::array();
    for (const auto &e : in) {
        Value item;
        to_value(item, e);
        out.push_back(std::move(item));
    }
}
template <class T>
void to_value(Value &out, const std::map<std::string, T> &in) {
    out = Value::object();
    for (const auto &kv : in) {
        Value item;
        to_value(item, kv.second);
        out[kv.first] = std::move(item);
    }
}
template <class T>
void to_value(Value &out, const std::optional<T> &in) {
    if (in)
        to_value(out, *in);
    else
        out = Value();
}

template <class T>
Value toValue(const T &in) {
    Value out;
    to_value(out, in);
    return out;
}

template <class T>
Value::Value(const std::vector<T> &list) : v_(Array{}) {
    auto &arr = std::get<Array>(v_);
    for (const auto &e : list) arr.push_back(toValue(e));
}

template <class T>
T Value::as() const {
    T out{};
    from_value(*this, out);
    return out;
}

/* obj("name", "Ravi", "age", 30)  ==  Value{{"name", "Ravi"}, {"age", 30}}
   arr(1, 2, 3)                     ==  Value::array({1, 2, 3})
   Same result as braces, but no braced list: use these inside co_await on GCC 13, which crashes
   (internal compiler error) on {{...}} lists in a co_await expression. Any GCC 14+/Clang is fine with braces. */
namespace detail {
inline void objPut(Value &) {}
template <class K, class V, class... R>
void objPut(Value &o, K &&k, V &&v, R &&...rest) {
    o[std::string(std::forward<K>(k))] = Value(std::forward<V>(v));
    objPut(o, std::forward<R>(rest)...);
}
}  // namespace detail
template <class... A>
Value obj(A &&...kv) {
    static_assert(sizeof...(A) % 2 == 0, "nevla::obj needs key, value pairs");
    Value o = Value::object();
    detail::objPut(o, std::forward<A>(kv)...);
    return o;
}
template <class... A>
Value arr(A &&...items) {
    Value a = Value::array();
    (a.push_back(Value(std::forward<A>(items))), ...);
    return a;
}

}  // namespace nevla
