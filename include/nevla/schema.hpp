#pragma once
/* Schema: document ka shape + rules (Mongoose ke Schema jaisa).

     Schema userSchema({
         {"name",  String().required().trim().minLength(2)},
         {"email", String().required().unique().lowercase().match(R"(^\S+@\S+\.\S+$)")},
         {"age",   Int().min(0).max(150)},
         {"role",  String().enumOf({"user", "admin"}).def("user")},
     }, {.timestamps = true});                                                                    */

#include "value.hpp"

#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nevla {

class Schema;
class Document;

// Value galat type ka ho (e.g. Number field me "abc")
class CastError : public Error {
  public:
    std::string path;
    CastError(std::string p, const std::string &msg) : Error(msg), path(std::move(p)) {}
};

// Validation fail: errors[path] = message
class ValidationError : public Error {
  public:
    std::map<std::string, std::string> errors;
    ValidationError(const std::string &msg, std::map<std::string, std::string> errs)
        : Error(msg), errors(std::move(errs)) {}
};

// unique index toota (E11000)
class DuplicateKeyError : public Error {
  public:
    using Error::Error;
};

// orFail() par document nahi mila
class NotFoundError : public Error {
  public:
    using Error::Error;
};

enum class FieldType { String, Number, Int, Boolean, Date, ObjectId, Array, Object, Mixed };

struct Field {
    FieldType type = FieldType::Mixed;

    /* ---- options (chain karke lagao) ---- */
    Field &required(bool on = true, std::string message = "");
    Field &def(Value value);                  // default value
    template <std::invocable F>
    Field &def(F make) {  // default function (har naye document par chalta hai)
        return defFn(std::function<Value()>(std::move(make)));
    }
    Field &defFn(std::function<Value()> make);
    Field &enumOf(std::vector<Value> allowed);
    Field &min(double v);
    Field &max(double v);
    Field &minLength(size_t n);
    Field &maxLength(size_t n);
    Field &match(std::string regex, std::string message = "");
    Field &trim(bool on = true);
    Field &lowercase(bool on = true);
    Field &uppercase(bool on = true);
    Field &unique(bool on = true);     // unique index (validator nahi: DuplicateKeyError save par aata hai)
    Field &index(bool on = true);      // normal index
    Field &select(bool on);            // select(false): find me by default nahi aata ("+field" se mangwao)
    Field &immutable(bool on = true);  // ek baar save ke baad badal nahi sakta
    Field &validate(std::function<bool(const Value &)> fn, std::string message = "");
    Field &set(std::function<Value(const Value &)> fn);  // setter: value save hone se pehle badlo
    Field &ref(std::string modelName);                   // populate() ke liye

    /* ---- data (seedha padh sakte ho) ---- */
    bool required_ = false;
    std::string requiredMessage_;
    std::optional<Value> default_;
    std::function<Value()> defaultFn_;
    std::vector<Value> enum_;
    std::optional<double> min_, max_;
    std::optional<size_t> minLength_, maxLength_;
    std::string match_, matchMessage_;
    bool trim_ = false, lowercase_ = false, uppercase_ = false;
    bool unique_ = false, index_ = false, select_ = true, immutable_ = false;
    std::vector<std::pair<std::function<bool(const Value &)>, std::string>> validators_;
    std::function<Value(const Value &)> setter_;
    std::string ref_;
    std::shared_ptr<Field> of_;       // Array ke elements ka type
    std::shared_ptr<Schema> schema_;  // Object / Array-of-objects ka schema
};

/* ---- Field types ---- */
Field String();
Field Number();    // int ya double
Field Int();       // sirf poora number
Field Boolean();
Field DateTime();
Field Id();        // ObjectId (ref ke saath populate hota hai)
Field Mixed();     // kuch bhi (cast / validation nahi)
Field Array(Field of);              // Array(String())
Field Array(const Schema &schema);  // Array(Schema{...}) = sub-documents ki list
Field Object(const Schema &schema); // nested object

struct SchemaOptions {
    bool timestamps = false;                 // createdAt / updatedAt apne aap
    std::string collection;                  // khaali = model ke naam se ("User" -> "users")
    bool strict = true;                      // schema me na likhe fields chhod do
    std::string versionKey = "__v";          // "" = band
    std::string discriminatorKey = "__t";
    bool autoIndex = true;                   // pehle operation par indexes bana do
    bool virtuals = false;                   // toJSON() me virtuals + "id" bhi
};

// Query middleware ko ye milta hai (filter / update badal sakte ho)
struct QueryInfo {
    std::string op;     // "find", "findOne", "updateOne", "updateMany", "findOneAndUpdate", "deleteOne", "deleteMany", ...
    std::string model;
    Value &filter;
    Value *update;      // update wale ops me, warna nullptr
};

class Schema {
  public:
    using DocHook = std::function<void(Document &)>;
    using QueryHook = std::function<void(QueryInfo &)>;

    Schema() = default;
    Schema(std::initializer_list<std::pair<std::string, Field>> fields, SchemaOptions options = {});

    Schema &add(const std::string &name, Field field);
    Schema &add(const Schema &other);  // dusre schema ke fields jod do

    // Compound / custom index:  schema.index({{"userId", 1}, {"createdAt", -1}}, {{"unique", true}})
    Schema &index(Value keys, Value options = Value::object());

    /* Middleware (hooks)
       Document hooks : "validate", "save", "deleteOne"        -> [](Document &doc) {...}
       Query hooks    : "find", "findOne", "updateOne", ...    -> [](QueryInfo &q) {...}  */
    Schema &pre(const std::string &hook, DocHook fn);
    Schema &post(const std::string &hook, DocHook fn);
    Schema &pre(const std::string &hook, QueryHook fn);

    // Virtual: DB me save nahi hota, doc.get("fullName") se milta hai
    Schema &virtual_(const std::string &name, std::function<Value(const Document &)> getter);
    // Instance method: doc.call("name", args)
    Schema &method(const std::string &name, std::function<Value(Document &, const Value &)> fn);
    // toJSON() ka result badlo (e.g. password hatao)
    Schema &toJSON(std::function<void(const Document &, Value &)> transform);

    const Field *field(const std::string &name) const;  // top-level (+ _id, createdAt, updatedAt)
    const Field *path(std::string_view dotted, bool *underMixed = nullptr) const;
    const std::vector<std::pair<std::string, Field>> &fields() const { return fields_; }
    bool hasOwnId() const;

    SchemaOptions options;

    /* ---- internals (library use karti hai) ---- */
    struct IndexDef {
        Value keys, options;
    };
    std::vector<IndexDef> indexes_;
    std::map<std::string, std::vector<DocHook>> preDoc_, postDoc_;
    std::map<std::string, std::vector<QueryHook>> preQuery_;
    std::vector<std::pair<std::string, std::function<Value(const Document &)>>> virtuals_;
    std::map<std::string, std::function<Value(Document &, const Value &)>> methods_;
    std::function<void(const Document &, Value &)> jsonTransform_;

  private:
    std::vector<std::pair<std::string, Field>> fields_;
};

namespace detail {
// Ek value ko field ke type me badlo (+ trim / lowercase / setter). Galat ho to CastError.
Value castValue(const Field &f, const Value &v, const std::string &path);
// Poora object: cast, strict (anjaan keys hatao)
Value castObject(const Schema &s, const Value &obj, const std::string &prefix = "");
void applyDefaults(const Schema &s, Value &obj);
void validateValue(const Field &f, const Value *v, const std::string &path, std::map<std::string, std::string> &errors);
void validateObject(const Schema &s, const Value &obj, const std::string &prefix,
                    std::map<std::string, std::string> &errors);
// select(false) wale paths
void hiddenPaths(const Schema &s, const std::string &prefix, std::vector<std::string> &out);
}  // namespace detail

std::string uuid();  // random UUID v4 string (custom string _id ke liye)

}  // namespace nevla
