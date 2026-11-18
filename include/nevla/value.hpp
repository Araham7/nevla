#pragma once
/* nevla::Value — JSON/BSON jaisa dynamic value (null, bool, int, double, string, Date, ObjectId, array, object).
   Object me keys ka order wahi rehta hai jis order me daale gaye.

     Value user = {{"name", "Asha"}, {"age", 21}, {"tags", {"a", "b"}}};   // object
     Value list = {1, 2, 3};                                              // array
     user["age"] = 22;   user.path("address.city");   user.toJson();                                       */

#include <compare>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nevla {

// Sab nevla errors ka base
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
    explicit ObjectId(std::string hex);  // galat ho to nevla::Error
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
    Value(T *) = delete;  // pointer galti se bool na ban jaye
    /* {{"key", value}, ...} = object;  {a, b, c} = array.
       (Har element 2-item array ho jiska pehla item string ho to object mana jata hai.
        Aise pairs ka ARRAY chahiye to Value::array({...}) use karo.) */
    Value(std::initializer_list<Value> init);

    static Value object();                                   // {}
    static Value array(std::initializer_list<Value> items = {});  // [] ya [..] (hamesha array)
    static Value parse(std::string_view json);               // JSON text -> Value (galat ho to Error)

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

    // Lenient getters (type alag ho to jitna ho sake convert, warna fallback)
    bool asBool(bool fallback = false) const;
    int64_t asInt(int64_t fallback = 0) const;
    double asDouble(double fallback = 0) const;
    const std::string &asString() const;  // string na ho to ""
    std::string str() const;              // kisi bhi scalar ka text ("42", "true", ObjectId hex, ISO date)
    Date asDate() const;
    ObjectId asObjectId() const;
    template <class T>
    T as() const;  // as<int>(), as<std::string>(), as<std::vector<std::string>>(), as<MyStruct>() (NEVLA_STRUCT)

    // Object
    Value &operator[](const std::string &key);              // na ho to bana deta hai (null -> object)
    const Value &operator[](const std::string &key) const;  // na ho to null
    Value *find(std::string_view key);
    const Value *find(std::string_view key) const;
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    bool erase(std::string_view key);
    Object &items() &;              // object ke (key, value) pairs
    const Object &items() const &;  // object na ho to khaali
    Object items() &&;              // temporary par: copy (taaki `for (auto &kv : doc.get("x").items())` safe rahe)

    // Array
    void push_back(Value v);  // null -> array
    Value &at(size_t i);
    const Value &at(size_t i) const;
    Array &elements() &;
    const Array &elements() const &;  // array na ho to khaali
    Array elements() &&;              // temporary par: copy (range-for me safe)

    size_t size() const;  // array / object ke items, string ki length, baaki 0
    bool empty() const { return size() == 0; }

    // Dotted path: "address.city", "tags.0"
    const Value *path(std::string_view dotted) const;  // na mile to nullptr
    Value &pathRef(std::string_view dotted);           // raste me objects bana deta hai
    bool erasePath(std::string_view dotted);

    bool operator==(const Value &other) const;
    std::string toJson(int indent = -1) const;  // Date -> ISO string, ObjectId -> hex string

  private:
    std::variant<std::monostate, bool, int64_t, double, std::string, Date, ObjectId, Array, Object> v_;
};

/* ---- C++ types <-> Value (as<T>() / toValue() / NEVLA_STRUCT isi se chalte hain) ---- */
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

}  // namespace nevla
