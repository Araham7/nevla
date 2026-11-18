#pragma once
/* Typed models: your struct in, your struct out, field names checked by the compiler.

     struct User { std::string id, name, email; int age = 0; std::vector<std::string> tags; };
     NEVLA_STRUCT(User, id, name, email, age, tags)

     nevla::TypedModel<User> Users("User");                        // schema generated from the struct
     User u = Users.create({.name = "Asha", .email = "a@x.in", .age = 21});
     std::vector<User> adults = Users.find().where(&User::age).gte(18).sort(&User::age, -1).exec();
     Users.update(u.id).set(&User::age, 22).push(&User::tags, "pro").exec();

   &User::agee  -> compile error (no such member)
   .where(&User::age).gte("x") -> compile error (age is an int)                                          */

#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "model.hpp"
#include "typed.hpp"

namespace nevla {

namespace detail {
template <class T>
concept NevlaStruct = requires { nevla_visit(static_cast<const T *>(nullptr), [](const char *, auto) {}); };

template <class T>
struct Unwrap {
    using type = T;
};
template <class T>
struct Unwrap<std::optional<T>> {
    using type = T;
};
template <class T>
struct IsVector : std::false_type {};
template <class T>
struct IsVector<std::vector<T>> : std::true_type {
    using elem = T;
};
template <class T>
struct IsMap : std::false_type {};
template <class T>
struct IsMap<std::map<std::string, T>> : std::true_type {
    using elem = T;
};

// MongoDB field name of a member pointer ("id" -> "_id")
template <class T, class M>
std::string memberName(M T::*ptr) {
    std::string out;
    nevla_visit(static_cast<const T *>(nullptr), [&](const char *name, auto p) {
        if constexpr (std::is_same_v<decltype(p), M T::*>)
            if (out.empty() && p == ptr) out = structKey(name);
    });
    if (out.empty()) throw Error("nevla: this member is not listed in NEVLA_STRUCT");
    return out;
}
}  // namespace detail

template <class T>
Schema schemaOf(SchemaOptions options = {});

namespace detail {
template <class M>
Field fieldFor() {
    using U = typename Unwrap<M>::type;
    if constexpr (std::is_same_v<U, std::string>)
        return String();
    else if constexpr (std::is_same_v<U, bool>)
        return Boolean();
    else if constexpr (std::is_integral_v<U>)
        return Int();
    else if constexpr (std::is_floating_point_v<U>)
        return Number();
    else if constexpr (std::is_same_v<U, Date>)
        return DateTime();
    else if constexpr (std::is_same_v<U, ObjectId>)
        return Id();
    else if constexpr (std::is_same_v<U, Value>)
        return Mixed();
    else if constexpr (IsVector<U>::value) {
        using E = typename IsVector<U>::elem;
        if constexpr (NevlaStruct<E>)
            return Array(schemaOf<E>());
        else
            return Array(fieldFor<E>());
    } else if constexpr (IsMap<U>::value) {
        return Map(fieldFor<typename IsMap<U>::elem>());
    } else if constexpr (NevlaStruct<U>) {
        return Object(schemaOf<U>());
    } else {
        static_assert(sizeof(U) == 0, "nevla::schemaOf: unsupported member type (add a NEVLA_STRUCT or use Value)");
    }
}
}  // namespace detail

// A schema with one field per NEVLA_STRUCT member (types from the C++ types). Change any field afterwards:
//   auto s = nevla::schemaOf<User>({.timestamps = true});
//   s.add("email", String().required().lowercase().unique());      // replaces the generated field
template <class T>
Schema schemaOf(SchemaOptions options) {
    Schema s;
    s.options = options;
    nevla_visit(static_cast<const T *>(nullptr), [&](const char *name, auto ptr) {
        using M = std::remove_cvref_t<decltype(static_cast<const T *>(nullptr)->*ptr)>;
        if (std::string(name) == "id") return;  // the document _id
        s.add(name, detail::fieldFor<M>());
    });
    return s;
}

template <class T>
class TypedQuery;

// where(&T::member) -> conditions typed like the member. Chain more conditions on the same field
// (.gte(18).lt(60)), or go on with the query (.sort(...), .where(...), .exec()).
template <class T, class M>
class FieldCond {
  public:
    using V = typename detail::Unwrap<M>::type;
    FieldCond(TypedQuery<T> q, std::string path) : q_(std::move(q)), path_(std::move(path)) {}
    FieldCond &equals(const V &v) { return op("$eq", toValue(v)); }
    FieldCond &ne(const V &v) { return op("$ne", toValue(v)); }
    FieldCond &gt(const V &v) { return op("$gt", toValue(v)); }
    FieldCond &gte(const V &v) { return op("$gte", toValue(v)); }
    FieldCond &lt(const V &v) { return op("$lt", toValue(v)); }
    FieldCond &lte(const V &v) { return op("$lte", toValue(v)); }
    FieldCond &in(const std::vector<V> &vs) { return op("$in", toValue(vs)); }
    FieldCond &nin(const std::vector<V> &vs) { return op("$nin", toValue(vs)); }
    FieldCond &exists(bool yes = true) { return op("$exists", yes); }
    // for std::vector members: the array contains this element
    template <class E = V>
        requires detail::IsVector<E>::value
    FieldCond &contains(const typename detail::IsVector<E>::elem &e) {
        return op("$eq", toValue(e));
    }
    template <class E = V>
        requires std::is_same_v<E, std::string>
    FieldCond &regex(const std::string &pattern, const std::string &options = "") {
        op("$regex", pattern);
        if (!options.empty()) op("$options", options);
        return *this;
    }

    // continue with the query
    template <class M2>
    FieldCond<T, M2> where(M2 T::*member) {
        return q_.where(member);
    }
    template <class M2, class N>
    FieldCond<T, N> where(M2 T::*outer, N M2::*inner) {
        return q_.where(outer, inner);
    }
    TypedQuery<T> &where(const Value &conditions) { return q_.where(conditions); }
    template <class M2>
    TypedQuery<T> &sort(M2 T::*member, int direction = 1) {
        return q_.sort(member, direction);
    }
    TypedQuery<T> &sort(const Value &spec) { return q_.sort(spec); }
    TypedQuery<T> &limit(int64_t n) { return q_.limit(n); }
    TypedQuery<T> &skip(int64_t n) { return q_.skip(n); }
    std::vector<T> exec() { return q_.exec(); }
    std::optional<T> first() { return q_.first(); }
    int64_t count() { return q_.count(); }
    Async<std::vector<T>> execAsync() const { return q_.execAsync(); }
    TypedQuery<T> &query() { return q_; }
    operator TypedQuery<T>() const { return q_; }

  private:
    FieldCond &op(const char *name, Value v) {
        Value cur = q_.raw().filter()[path_];
        Value cond = cur.isObject() && !cur.empty() && cur.items()[0].first[0] == '$' ? cur : Value::object();
        if (!cur.isObject() && !cur.isNull()) cond["$eq"] = cur;  // keep an earlier plain equality
        cond[name] = std::move(v);
        q_.raw().where(Value{{path_, cond}});
        return *this;
    }
    TypedQuery<T> q_;
    std::string path_;
};

template <class T>
class TypedQuery {
  public:
    explicit TypedQuery(Query q) : q_(std::move(q)) {}

    template <class M>
    FieldCond<T, M> where(M T::*member) {
        return FieldCond<T, M>(*this, detail::memberName(member));
    }
    // nested struct member:  where(&User::address, &Address::city)
    template <class M, class N>
    FieldCond<T, N> where(M T::*outer, N M::*inner) {
        return FieldCond<T, N>(*this, detail::memberName(outer) + "." + detail::memberName(inner));
    }
    TypedQuery &where(const Value &conditions) {  // object or formula string
        q_.where(conditions.isString() ? formulaFilter(conditions.asString()) : conditions);
        return *this;
    }
    template <class M>
    TypedQuery &sort(M T::*member, int direction = 1) {
        Value spec = Value::object();
        spec[detail::memberName(member)] = direction < 0 ? -1 : 1;
        sortSpec_.push_back(std::move(spec));
        applySort();
        return *this;
    }
    TypedQuery &sort(const Value &spec) {
        q_.sort(spec);
        sortSpec_.clear();
        return *this;
    }
    TypedQuery &limit(int64_t n) {
        q_.limit(n);
        return *this;
    }
    TypedQuery &skip(int64_t n) {
        q_.skip(n);
        return *this;
    }

    std::vector<T> exec() {
        std::vector<T> out;
        for (auto &v : q_.lean()) out.push_back(v.template as<T>());
        return out;
    }
    std::optional<T> first() {
        q_.limit(1);
        auto rows = exec();
        if (rows.empty()) return std::nullopt;
        return std::move(rows[0]);
    }
    int64_t count() { return q_.count(); }
    Async<std::vector<T>> execAsync() const {
        TypedQuery q = *this;
        return Async<std::vector<T>>([q]() mutable { return q.exec(); });
    }
    Query &raw() { return q_; }  // the underlying untyped query (populate, select, ...)

  private:
    void applySort() {
        Value all = Value::object();
        for (const auto &s : sortSpec_)
            for (const auto &kv : s.items()) all[kv.first] = kv.second;
        q_.sort(all);
    }
    Query q_;
    std::vector<Value> sortSpec_;
};

// Users.update(id).set(&User::age, 22).inc(&User::visits, 1).push(&User::tags, "x").exec()
template <class T>
class TypedUpdate {
  public:
    TypedUpdate(Model m, Value filter, bool many) : m_(std::move(m)), filter_(std::move(filter)), many_(many) {}

    template <class M>
    TypedUpdate &set(M T::*member, const typename detail::Unwrap<M>::type &v) {
        update_["$set"][detail::memberName(member)] = toValue(v);
        return *this;
    }
    template <class M>
        requires std::is_arithmetic_v<M> && (!std::is_same_v<M, bool>)
    TypedUpdate &inc(M T::*member, M by = 1) {
        update_["$inc"][detail::memberName(member)] = toValue(by);
        return *this;
    }
    template <class M>
    TypedUpdate &unset(M T::*member) {
        update_["$unset"][detail::memberName(member)] = "";
        return *this;
    }
    template <class E>
    TypedUpdate &push(std::vector<E> T::*member, const std::type_identity_t<E> &item) {  // "literal" ok
        update_["$push"][detail::memberName(member)] = toValue(item);
        return *this;
    }
    template <class E>
    TypedUpdate &pull(std::vector<E> T::*member, const std::type_identity_t<E> &item) {  // "literal" ok
        update_["$pull"][detail::memberName(member)] = toValue(item);
        return *this;
    }
    UpdateResult exec(UpdateOptions o = {}) {
        return many_ ? m_.updateMany(filter_, update_, o) : m_.updateOne(filter_, update_, o);
    }

  private:
    Model m_;
    Value filter_, update_ = Value::object();
    bool many_;
};

template <class T>
class TypedModel {
  public:
    // Schema generated from the struct
    explicit TypedModel(const std::string &name, SchemaOptions options = {}, const std::string &collection = "")
        : m_(nevla::model(name, schemaOf<T>(options), collection)) {}
    // Your own schema (e.g. schemaOf<T>() with validators added)
    TypedModel(const std::string &name, const Schema &schema, const std::string &collection = "")
        : m_(nevla::model(name, schema, collection)) {}
    explicit TypedModel(Model m) : m_(std::move(m)) {}

    T create(const T &obj) const { return m_.create(toValue(obj)).template as<T>(); }
    std::vector<T> insertMany(const std::vector<T> &objs) const {
        std::vector<Value> vs;
        for (const auto &o : objs) vs.push_back(toValue(o));
        std::vector<T> out;
        for (auto &d : m_.insertMany(vs)) out.push_back(d.template as<T>());
        return out;
    }
    TypedQuery<T> find(const Value &filter = Value::object()) const { return TypedQuery<T>(m_.find(filter)); }
    std::optional<T> findOne(const Value &filter = Value::object()) const {
        auto v = m_.findOne(filter).lean();
        if (!v) return std::nullopt;
        return v->template as<T>();
    }
    std::optional<T> findById(const Value &id) const {
        auto v = m_.findById(id).lean();
        if (!v) return std::nullopt;
        return v->template as<T>();
    }
    // Insert when it has no id yet, otherwise save the changed fields (validated). Returns the stored state.
    T save(const T &obj) const {
        Value v = toValue(obj);
        if (!v.contains("_id")) return create(obj);
        auto doc = m_.findById(v["_id"]).orFail();
        v.erase("_id");
        doc.set(v);
        doc.save();
        return doc.template as<T>();
    }
    bool deleteById(const Value &id) const { return m_.deleteOne(Value{{"_id", id}}) > 0; }
    int64_t deleteMany(const Value &filter) const { return m_.deleteMany(filter); }
    int64_t count(const Value &filter = Value::object()) const { return m_.countDocuments(filter); }
    TypedUpdate<T> update(const Value &id) const { return TypedUpdate<T>(m_, Value{{"_id", id}}, false); }
    TypedUpdate<T> updateMany(const Value &filter) const { return TypedUpdate<T>(m_, filter, true); }
    Aggregate aggregate() const { return m_.aggregate(); }

    template <class M>
    static std::string path(M T::*member) {
        return detail::memberName(member);
    }
    const Model &model() const { return m_; }  // the untyped model (hooks, populate, ...)

  private:
    Model m_;
};

}  // namespace nevla
