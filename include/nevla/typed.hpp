#pragma once
/* NEVLA_STRUCT: connect your own C++ struct to Value / Document (compile time, no reflection needed).

     struct User { std::string id, name; int age = 0; std::vector<std::string> tags; };
     NEVLA_STRUCT(User, id, name, age, tags)        // write it in the struct's namespace

     User u = doc.as<User>();                        // Document -> struct
     auto doc = UserModel.create(nevla::toValue(u)); // struct -> Document
   A member named "id" maps to MongoDB's "_id" (left out when empty, so the DB / model generates one).
   Nested structs, std::vector, std::optional, std::map<std::string, T> all work. */

#include "value.hpp"

namespace nevla::detail {
inline std::string structKey(const char *member) { return std::string(member) == "id" ? "_id" : member; }
inline bool skipEmptyId(const std::string &key, const Value &v) {
    return key == "_id" &&
           (v.isNull() || (v.isString() && v.asString().empty()) || (v.isObjectId() && v.asObjectId().empty()));
}
}  // namespace nevla::detail

#define NEVLA_PARENS ()
#define NEVLA_EXPAND(...) NEVLA_EXPAND4(NEVLA_EXPAND4(NEVLA_EXPAND4(NEVLA_EXPAND4(__VA_ARGS__))))
#define NEVLA_EXPAND4(...) NEVLA_EXPAND3(NEVLA_EXPAND3(NEVLA_EXPAND3(NEVLA_EXPAND3(__VA_ARGS__))))
#define NEVLA_EXPAND3(...) NEVLA_EXPAND2(NEVLA_EXPAND2(NEVLA_EXPAND2(NEVLA_EXPAND2(__VA_ARGS__))))
#define NEVLA_EXPAND2(...) NEVLA_EXPAND1(NEVLA_EXPAND1(NEVLA_EXPAND1(NEVLA_EXPAND1(__VA_ARGS__))))
#define NEVLA_EXPAND1(...) __VA_ARGS__
#define NEVLA_FOR_EACH(macro, ...) __VA_OPT__(NEVLA_EXPAND(NEVLA_FOR_EACH_HELPER(macro, __VA_ARGS__)))
#define NEVLA_FOR_EACH_HELPER(macro, a1, ...) \
    macro(a1) __VA_OPT__(NEVLA_FOR_EACH_AGAIN NEVLA_PARENS(macro, __VA_ARGS__))
#define NEVLA_FOR_EACH_AGAIN() NEVLA_FOR_EACH_HELPER

#define NEVLA_TO_MEMBER(f)                                                                       \
    {                                                                                            \
        ::nevla::Value nv_item;                                                                  \
        to_value(nv_item, nv_in.f);                                                              \
        std::string nv_key = ::nevla::detail::structKey(#f);                                     \
        if (!::nevla::detail::skipEmptyId(nv_key, nv_item)) nv_out[nv_key] = std::move(nv_item); \
    }
#define NEVLA_FROM_MEMBER(f) \
    if (const ::nevla::Value *nv_p = nv_in.find(::nevla::detail::structKey(#f))) from_value(*nv_p, nv_out.f);

#define NEVLA_VISIT_MEMBER(f) nv_fn(#f, &NvSelf::f);

/* Also generates nevla_visit(const Type *, fn): calls fn("member", &Type::member) for every member.
   TypedModel / schemaOf / member-pointer queries are built on it. */
#define NEVLA_STRUCT(Type, ...)                                         \
    inline void to_value(::nevla::Value &nv_out, const Type &nv_in) {   \
        nv_out = ::nevla::Value::object();                              \
        NEVLA_FOR_EACH(NEVLA_TO_MEMBER, __VA_ARGS__)                    \
    }                                                                   \
    inline void from_value(const ::nevla::Value &nv_in, Type &nv_out) { \
        NEVLA_FOR_EACH(NEVLA_FROM_MEMBER, __VA_ARGS__)                  \
    }                                                                   \
    template <class NvFn>                                               \
    inline void nevla_visit(const Type *, NvFn &&nv_fn) {               \
        using NvSelf = Type;                                            \
        NEVLA_FOR_EACH(NEVLA_VISIT_MEMBER, __VA_ARGS__)                 \
    }
