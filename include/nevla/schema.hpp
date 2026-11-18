#pragma once
/* Schema: the shape of a document plus its rules (like a Mongoose Schema).

     Schema userSchema({
         {"name",  String().required().trim().minLength(2)},
         {"email", String().required().unique().lowercase().match(R"(^\S+@\S+\.\S+$)")},
         {"age",   Int().min(0).max(150)},
         {"role",  String().enumOf({"user", "admin"}).def("user")},
     }, {.timestamps = true});                                                                    */

#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "value.hpp"

namespace nevla {

class Schema;
class Document;
class Model;

// A value could not be converted to the field's type (e.g. "abc" for a Number field).
// Thrown by queries / updates. On documents, cast problems are reported through ValidationError instead.
class CastError : public Error {
  public:
    std::string path;
    CastError(std::string p, const std::string &msg) : Error(msg), path(std::move(p)) {}
};

// Validation failed. errors[path] = message, kinds[path] = "required" | "enum" | "min" | "max" | "minlength" |
// "maxlength" | "regexp" | "user defined" | "cast"
class ValidationError : public Error {
  public:
    std::map<std::string, std::string> errors;
    std::map<std::string, std::string> kinds;
    ValidationError(const std::string &msg, std::map<std::string, std::string> errs,
                    std::map<std::string, std::string> k = {})
        : Error(msg), errors(std::move(errs)), kinds(std::move(k)) {}
};

// A unique index was violated (MongoDB E11000)
class DuplicateKeyError : public Error {
  public:
    using Error::Error;
};

// orFail() / save() could not find the document
class NotFoundError : public Error {
  public:
    using Error::Error;
};

// optimisticConcurrency: somebody else saved this document after you loaded it
class VersionError : public Error {
  public:
    using Error::Error;
};

enum class FieldType { String, Number, Int, Boolean, Date, ObjectId, Array, Object, Mixed, Map };

struct Field {
    FieldType type = FieldType::Mixed;

    /* ---- options (chainable) ----
       Messages may contain {PATH} and {VALUE}, e.g. .min(18, "{PATH} must be 18+, got {VALUE}") */
    Field &required(bool on = true, std::string message = "");
    Field &def(Value value);  // default value
    template <std::invocable F>
    Field &def(F make) {  // default function (runs for every new document)
        return defFn(std::function<Value()>(std::move(make)));
    }
    Field &defFn(std::function<Value()> make);
    Field &enumOf(std::vector<Value> allowed, std::string message = "");
    Field &min(double v, std::string message = "");
    Field &max(double v, std::string message = "");
    Field &min(Date d, std::string message = "");  // DateTime().min(*Date::parse("2020-01-01"))
    Field &max(Date d, std::string message = "");
    Field &minLength(size_t n, std::string message = "");
    Field &maxLength(size_t n, std::string message = "");
    Field &match(std::string regex, std::string message = "");  // invalid regex throws nevla::Error right here
    Field &trim(bool on = true);
    Field &lowercase(bool on = true);
    Field &uppercase(bool on = true);
    Field &unique(bool on = true);     // unique index (not a validator: DuplicateKeyError comes from save)
    Field &index(bool on = true);      // normal index
    Field &sparse(bool on = true);     // sparse index (with unique/index)
    Field &expires(int64_t seconds);   // TTL index on a DateTime field
    Field &select(bool on);            // select(false): hidden from find() unless asked with "+field"
    Field &immutable(bool on = true);  // cannot change after the first save (also stripped from updates)
    Field &validate(std::function<bool(const Value &)> fn, std::string message = "");
    Field &set(std::function<Value(const Value &)> fn);  // setter: transform the value before it is stored
    Field &get(std::function<Value(const Value &)> fn);  // getter: transform on doc.get() / toJSON({getters})
    Field &alias(std::string name);                      // doc.get("n") / doc.set("n", v) for field "name"
    Field &ref(std::string modelName);                   // for populate()
    // Stored AES-256-GCM encrypted (String fields; needs nevla::setEncryptionKey). deterministic = true:
    // the same text always encrypts the same way, so equality filters work ({{"token", t}}) but equal values
    // can be recognised; false (default) is safer but the field cannot be filtered on.
    Field &encrypted(bool deterministic = false);

    /* ---- data (readable) ---- */
    bool required_ = false;
    std::string requiredMessage_;
    std::optional<Value> default_;
    std::function<Value()> defaultFn_;
    std::vector<Value> enum_;
    std::string enumMessage_;
    std::optional<double> min_, max_;
    std::string minMessage_, maxMessage_;
    bool minIsDate_ = false, maxIsDate_ = false;
    std::optional<size_t> minLength_, maxLength_;
    std::string minLengthMessage_, maxLengthMessage_;
    std::string match_, matchMessage_;
    std::shared_ptr<const std::regex> regex_;  // compiled once by match()
    bool trim_ = false, lowercase_ = false, uppercase_ = false;
    bool unique_ = false, index_ = false, sparse_ = false, select_ = true, immutable_ = false;
    std::optional<int64_t> expires_;
    std::vector<std::pair<std::function<bool(const Value &)>, std::string>> validators_;
    std::function<Value(const Value &)> setter_, getter_;
    std::string alias_;
    std::string ref_;
    bool encrypted_ = false, deterministic_ = false;
    std::shared_ptr<Field> of_;       // element type of Array / value type of Map
    std::shared_ptr<Schema> schema_;  // schema of Object / Array-of-sub-documents
};

/* ---- Field types ---- */
Field String();
Field Number();  // int or double
Field Int();     // whole numbers only
Field Boolean();
Field DateTime();
Field Id();                          // ObjectId (populates with .ref())
Field Mixed();                       // anything (no cast / validation)
Field Array(Field of);               // Array(String())
Field Array(const Schema &schema);   // Array(Schema{...}) = list of sub-documents
Field Object(const Schema &schema);  // nested object (single sub-document)
Field Map(Field of);                 // object with any keys, every value cast to `of`:  Map(Number())

// MongoDB time-series collection (MongoDB 5.0+): far less storage and fast time-range queries for sensor data
struct TimeSeriesOptions {
    std::string timeField;           // required: the DateTime field, e.g. "at"
    std::string metaField;           // optional: what the measurement belongs to, e.g. "device"
    std::string granularity;         // optional: "seconds" | "minutes" | "hours"
    int64_t expireAfterSeconds = 0;  // optional: delete measurements older than this (0 = keep)
};

struct SchemaOptions {
    bool timestamps = false;         // createdAt / updatedAt automatically
    std::string collection;          // empty = from the model name ("User" -> "users")
    bool strict = true;              // drop fields not in the schema (documents AND updates)
    std::string versionKey = "__v";  // "" = off
    std::string discriminatorKey = "__t";
    bool autoIndex = true;                // create indexes before the first operation
    bool virtuals = false;                // toJSON() includes virtuals + "id"
    std::string createdAt = "createdAt";  // timestamp field names
    std::string updatedAt = "updatedAt";
    bool optimisticConcurrency = false;  // save() fails with VersionError if the doc changed in the DB meanwhile
    bool getters = false;                // toJSON() applies field getters
    bool strictQuery = false;            // a filter path that is not in the schema throws (typo protection)
    TimeSeriesOptions timeseries;        // set timeField to make the collection a time-series collection
};

// What query middleware receives (may change filter / update)
struct QueryInfo {
    std::string op;  // "find", "findOne", "countDocuments", "updateOne", "updateMany", "findOneAndUpdate",
                     // "findOneAndDelete", "findOneAndReplace", "replaceOne", "deleteOne", "deleteMany", "aggregate"
    std::string model;
    Value &filter;  // for "aggregate": the pipeline (array)
    Value *update;  // update ops only, otherwise nullptr
};

// Virtual populate: fill a field from another collection where foreignField == this doc's localField
struct VirtualPopulate {
    std::string ref;           // model name
    std::string localField;    // e.g. "_id"
    std::string foreignField;  // e.g. "author"
    bool justOne = false;      // single document instead of an array
};

class Schema {
  public:
    using DocHook = std::function<void(Document &)>;
    using QueryHook = std::function<void(QueryInfo &)>;
    // result: array of docs (find), doc or null (findOne*), {matchedCount, modifiedCount, deletedCount...} (writes)
    using QueryPostHook = std::function<void(QueryInfo &, Value &result)>;
    using StaticFn = std::function<Value(const Model &, const Value &)>;

    Schema() = default;
    Schema(std::initializer_list<std::pair<std::string, Field>> fields, SchemaOptions options = {});

    Schema &add(const std::string &name, Field field);
    Schema &add(const Schema &other);  // merge another schema (fields, hooks, methods, statics, ...)
    Schema &remove(const std::string &name);

    // Compound / custom index:  schema.index({{"userId", 1}, {"createdAt", -1}}, {{"unique", true}})
    Schema &index(Value keys, Value options = Value::object());

    /* Middleware (hooks)
       Document hooks : "validate", "save", "deleteOne"   -> [](Document &doc) {...}   (pre + post)
       Query hooks    : "find", "findOne", "updateOne", "aggregate", ... (see QueryInfo::op)
                        pre  -> [](QueryInfo &q) {...}
                        post -> [](QueryInfo &q, Value &result) {...}                                  */
    Schema &pre(const std::string &hook, DocHook fn);
    Schema &post(const std::string &hook, DocHook fn);
    Schema &pre(const std::string &hook, QueryHook fn);
    Schema &post(const std::string &hook, QueryPostHook fn);

    // Virtual: not stored, read with doc.get("fullName")
    Schema &virtual_(const std::string &name, std::function<Value(const Document &)> getter);
    // Virtual populate: doc.populate("posts") fills it from another model
    Schema &virtualPopulate(const std::string &name, VirtualPopulate options);
    // Instance method: doc.call("name", args)
    Schema &method(const std::string &name, std::function<Value(Document &, const Value &)> fn);
    // Static (model-level) method: User.call("findByEmail", "a@b.c")
    Schema &static_(const std::string &name, StaticFn fn);
    // Reusable plugin:  schema.plugin(softDelete)   /   schema.plugin(paginate, {{"perPage", 20}})
    Schema &plugin(const std::function<void(Schema &)> &fn);
    Schema &plugin(const std::function<void(Schema &, const Value &)> &fn, const Value &options);
    // Change the result of toJSON() (e.g. remove password)
    Schema &toJSON(std::function<void(const Document &, Value &)> transform);

    /* Export the schema
         toJsonSchema()      MongoDB $jsonSchema (server-side validation, see Model::syncValidator)
         toTypeScript("User")  "export interface User { ... }" for a TypeScript / React frontend
         toOpenAPI()         OpenAPI 3 schema object for API docs                                 */
    Value toJsonSchema() const;
    std::string toTypeScript(const std::string &interfaceName) const;
    Value toOpenAPI() const;

    const Field *field(const std::string &name) const;  // top-level (+ _id and timestamps)
    // Dotted path: "address.city", "items.0.qty", "items.$.qty", "items.$[i].qty", "prices.inr" (Map)
    const Field *path(std::string_view dotted, bool *underMixed = nullptr) const;
    const std::vector<std::pair<std::string, Field>> &fields() const { return fields_; }
    bool hasOwnId() const;
    std::string resolveAlias(std::string_view path) const;  // "n.x" -> "name.x" if "n" is an alias

    SchemaOptions options;

    /* ---- internals (used by the library) ---- */
    struct IndexDef {
        Value keys, options;
    };
    std::vector<IndexDef> indexes_;
    std::map<std::string, std::vector<DocHook>> preDoc_, postDoc_;
    std::map<std::string, std::vector<QueryHook>> preQuery_;
    std::map<std::string, std::vector<QueryPostHook>> postQuery_;
    std::vector<std::pair<std::string, std::function<Value(const Document &)>>> virtuals_;
    std::vector<std::pair<std::string, VirtualPopulate>> virtualPopulates_;
    std::map<std::string, std::function<Value(Document &, const Value &)>> methods_;
    std::map<std::string, StaticFn> statics_;
    std::function<void(const Document &, Value &)> jsonTransform_;

  private:
    std::vector<std::pair<std::string, Field>> fields_;
};

namespace detail {
// Convert one value to the field's type (+ trim / lowercase / setter). Throws CastError.
Value castValue(const Field &f, const Value &v, const std::string &path);
// Whole object: cast + strict (drop unknown keys).
// castErrors == nullptr: throw CastError. Otherwise: record errors[path] = message, keep going.
Value castObject(const Schema &s, const Value &obj, const std::string &prefix = "",
                 std::map<std::string, std::string> *castErrors = nullptr);
void applyDefaults(const Schema &s, Value &obj);
void validateValue(const Field &f, const Value *v, const std::string &path, std::map<std::string, std::string> &errors,
                   std::map<std::string, std::string> *kinds = nullptr);
void validateObject(const Schema &s, const Value &obj, const std::string &prefix,
                    std::map<std::string, std::string> &errors, std::map<std::string, std::string> *kinds = nullptr);
// select(false) paths
void hiddenPaths(const Schema &s, const std::string &prefix, std::vector<std::string> &out);
// "{PATH} is bad ({VALUE})" -> filled in
std::string formatMessage(std::string msg, const std::string &path, const Value *value);
// Apply getters to a whole object (toJSON({getters}))
void applyGetters(const Schema &s, Value &obj);
}  // namespace detail

std::string uuid();  // random UUID v4 string (for custom string _id)

}  // namespace nevla
