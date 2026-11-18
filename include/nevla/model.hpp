#pragma once
/* Model / Document / Query (a Mongoose-style API).

     nevla::connect("mongodb://127.0.0.1:27017/mydb");
     auto User = nevla::model("User", userSchema);
     auto u = User.create({{"name", "Asha"}, {"email", "asha@x.com"}});
     auto admins = User.find({{"role", "admin"}}).sort("-createdAt").limit(10).exec();                    */

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "schema.hpp"

namespace nevla {

struct ModelImpl;
struct CursorImpl;
struct ChangeStreamImpl;
class Aggregate;
class Model;

/* ---------------- Connection ---------------- */
// Empty dbName = database from the URI ("mongodb://host/mydb"), or "test".
// Safe to call again (reconnect): running operations keep the old pool alive until they finish.
void connect(const std::string &uri, const std::string &dbName = "");
void disconnect();
bool connected();
std::string databaseName();
void ping();  // throws nevla::Error if the server is not reachable
// Every nevla operation inside fn() on THIS thread runs in one transaction (needs a replica set / Atlas).
// If fn throws, everything is rolled back. fn may run more than once on transient errors (driver retry).
void transaction(const std::function<void()> &fn);
// Run blocking work on another thread:  auto fut = nevla::async([&] { return User.find().exec(); });
template <class F>
auto async(F &&fn) {
    return std::async(std::launch::async, std::forward<F>(fn));
}
// Like mongoose.set('debug', true): print every operation to stderr, or receive it in a callback
void setDebug(bool on);
void setDebug(std::function<void(const std::string &collection, const std::string &op, const Value &args)> fn);
// Automatic index creation failed (e.g. duplicates already in the collection). Operations keep working.
// Default: print to stderr. Model::createIndexes() throws instead.
void onIndexError(std::function<void(const std::string &model, const std::string &message)> fn);

/* ---------------- Results / options ---------------- */
struct UpdateOptions {
    bool upsert = false;
    bool runValidators = false;  // run schema validators on $set / $setOnInsert / $unset values
};
struct FindUpdateOptions {
    bool returnNew = false;  // Mongoose { new: true }: return the document AFTER the update
    bool upsert = false;
    bool runValidators = false;
    std::string select;  // projection for the returned document ("name email", "-password")
};
struct UpdateResult {
    int64_t matchedCount = 0, modifiedCount = 0;
    Value upsertedId;  // _id of the document created by an upsert
};
struct BulkWriteResult {
    int64_t insertedCount = 0, matchedCount = 0, modifiedCount = 0, deletedCount = 0, upsertedCount = 0;
};
struct InsertManyOptions {
    bool ordered = true;  // false: keep inserting after a failure (DuplicateKeyError is still thrown at the end)
};

// populate() with options. Nested: {.path = "author", .populate = {{.path = "company"}}}
struct PopulateOptions {
    std::string path;
    std::string select;                     // fields of the populated documents ("name email" / "-password")
    Value match;                            // extra filter for the populated documents
    Value sort;                             // "-createdAt" or {{"createdAt", -1}}
    int64_t limit = 0;                      // per document (arrays / virtual populate), 0 = all
    std::vector<PopulateOptions> populate;  // populate inside the populated documents
    std::string model;                      // override the field's ref
};

/* ---------------- Document ---------------- */
class Document {
  public:
    Document() = default;

    // Read (dotted path like "address.city", virtuals and aliases; field getters are applied)
    Value get(std::string_view path) const;
    template <class T>
    T get(std::string_view path) const {
        return get(path).template as<T>();
    }
    // doc["a.b"]: raw stored value by reference (null if missing): `for (auto &e : doc["tags"].elements())` is safe
    const Value &operator[](std::string_view path) const;
    bool has(std::string_view path) const;
    std::string id() const;  // _id as text
    Value _id() const;

    // Write (cast + setters, change tracking). A value that cannot be cast is reported by validate()/save().
    Document &set(std::string_view path, const Value &value);
    Document &set(const Value &object);  // many fields at once
    Document &unset(std::string_view path);
    Document &push(std::string_view arrayPath, const Value &item);
    Value &mut(std::string_view path);  // reference for in-place edits (marks the path modified)
    void markModified(std::string_view path);
    Document &overwrite(const Value &object);  // replace all fields (keeps _id)

    bool isNew() const { return isNew_; }
    bool isModified(std::string_view path = "") const;
    std::vector<std::string> modifiedPaths() const;

    void validate();   // throws ValidationError
    Document &save();  // new: insert, existing: only the changed paths ($set / $unset)
    void deleteOne();
    UpdateResult updateOne(const Value &update, UpdateOptions options = {});  // Model.updateOne({_id}, update)
    Document &increment();                                                    // next save() bumps the version key (__v)

    Document &populate(const std::string &path);
    Document &populate(const PopulateOptions &options);
    Document &depopulate(const std::string &path = "");  // populated docs -> back to ids ("" = all)
    bool populated(const std::string &path) const;

    Value call(const std::string &method, const Value &args = Value());  // schema.method()
    bool equals(const Document &other) const;                            // same _id

    Value toObject() const;  // raw data
    Value toJSON() const;    // + virtuals / getters (schema options) + toJSON transform
    std::string json(int indent = -1) const { return toJSON().toJson(indent); }
    template <class T>
    T as() const {  // into a NEVLA_STRUCT struct
        return data_.template as<T>();
    }
    const Value &data() const { return data_; }
    std::string modelName() const;
    explicit operator bool() const { return m_ != nullptr; }

  private:
    friend class Model;
    friend struct ModelImpl;
    std::shared_ptr<ModelImpl> m_;
    Value data_ = Value::object();
    std::set<std::string> modified_;
    std::map<std::string, std::string> castErrors_;  // set() values that could not be cast
    std::set<std::string> populated_;
    mutable std::map<std::string, Value> virtualCache_;
    bool isNew_ = true;
    bool bumpVersion_ = false;
};

/* ---------------- Query ---------------- */
namespace detail {
struct QuerySpec {
    Value filter = Value::object();
    Value sort;
    int64_t limit = 0, skip = 0, batchSize = 0, maxTimeMS = 0;
    std::string select, comment;
    Value hint, collation;
    std::vector<PopulateOptions> populate;
};
Value parseSort(const Value &spec);
}  // namespace detail

template <class D>
class QueryBase {
  public:
    // where("age").gte(18).lte(60)   |   where("role", "admin")   |   where({{"role", "admin"}})
    D &where(std::string path) {
        path_ = std::move(path);
        return self();
    }
    D &where(const char *path) { return where(std::string(path)); }
    D &where(const std::string &path, Value value) {
        spec_.filter[path] = std::move(value);
        return self();
    }
    D &where(const Value &conditions) {
        for (const auto &kv : conditions.items()) spec_.filter[kv.first] = kv.second;
        return self();
    }
    D &equals(Value v) {
        spec_.filter[path_] = std::move(v);
        return self();
    }
    D &gt(Value v) { return op("$gt", std::move(v)); }
    D &gte(Value v) { return op("$gte", std::move(v)); }
    D &lt(Value v) { return op("$lt", std::move(v)); }
    D &lte(Value v) { return op("$lte", std::move(v)); }
    D &ne(Value v) { return op("$ne", std::move(v)); }
    D &in(std::vector<Value> values) { return op("$in", Value(Value::Array(std::move(values)))); }
    D &nin(std::vector<Value> values) { return op("$nin", Value(Value::Array(std::move(values)))); }
    D &all(std::vector<Value> values) { return op("$all", Value(Value::Array(std::move(values)))); }
    D &size(int64_t n) { return op("$size", n); }
    D &mod(int64_t divisor, int64_t remainder) { return op("$mod", Value::array({divisor, remainder})); }
    D &exists(bool yes = true) { return op("$exists", yes); }
    D &elemMatch(Value conditions) { return op("$elemMatch", std::move(conditions)); }
    D &regex(std::string pattern, std::string options = "") {
        op("$regex", std::move(pattern));
        if (!options.empty()) op("$options", std::move(options));
        return self();
    }
    // or_({{{"role", "admin"}}, {{"age", {{"$gte", 18}}}}})   (or / and are C++ keywords)
    D &or_(std::vector<Value> conditions) { return logical("$or", std::move(conditions)); }
    D &and_(std::vector<Value> conditions) { return logical("$and", std::move(conditions)); }
    D &nor(std::vector<Value> conditions) { return logical("$nor", std::move(conditions)); }
    D &text(std::string search) {  // needs a text index
        spec_.filter["$text"] = Value{{"$search", std::move(search)}};
        return self();
    }
    // sort("-createdAt name")  or  sort({{"createdAt", -1}})
    D &sort(const Value &spec) {
        spec_.sort = detail::parseSort(spec);
        return self();
    }
    D &limit(int64_t n) {
        spec_.limit = n;
        return self();
    }
    D &skip(int64_t n) {
        spec_.skip = n;
        return self();
    }
    // select("name email") | select("-password") | select("+password") (also fetch a select(false) field)
    D &select(const std::string &fields) {
        spec_.select += " " + fields;
        return self();
    }
    D &populate(const std::string &path) {
        PopulateOptions o;
        o.path = path;
        spec_.populate.push_back(std::move(o));
        return self();
    }
    D &populate(PopulateOptions options) {
        spec_.populate.push_back(std::move(options));
        return self();
    }
    D &batchSize(int64_t n) {
        spec_.batchSize = n;
        return self();
    }
    D &maxTimeMS(int64_t ms) {
        spec_.maxTimeMS = ms;
        return self();
    }
    D &hint(Value index) {  // {{"email", 1}} or "email_1"
        spec_.hint = std::move(index);
        return self();
    }
    D &collation(Value c) {  // {{"locale", "en"}, {"strength", 2}} = case-insensitive
        spec_.collation = std::move(c);
        return self();
    }
    D &comment(std::string c) {
        spec_.comment = std::move(c);
        return self();
    }
    const Value &filter() const { return spec_.filter; }

  protected:
    QueryBase(std::shared_ptr<ModelImpl> m, Value filter) : m_(std::move(m)) {
        if (filter.isObject()) spec_.filter = std::move(filter);
    }
    D &self() { return static_cast<D &>(*this); }
    D &op(const char *name, Value v) {
        if (path_.empty()) throw Error(std::string("nevla: call where(\"path\") before ") + name);
        Value &cur = spec_.filter[path_];
        if (!cur.isObject()) cur = Value::object();
        cur[name] = std::move(v);
        return self();
    }
    D &logical(const char *name, std::vector<Value> conds) {
        Value &arr = spec_.filter[name];
        if (!arr.isArray()) arr = Value::array();
        for (auto &c : conds) arr.push_back(std::move(c));
        return self();
    }
    std::shared_ptr<ModelImpl> m_;
    detail::QuerySpec spec_;
    std::string path_;
};

// Streams documents from the server in batches (does not load everything into memory)
class Cursor {
  public:
    std::optional<Document> next();
    std::optional<Value> nextLean();

  private:
    friend class Query;
    std::shared_ptr<CursorImpl> impl_;
};

struct Page {
    std::vector<Document> docs;
    int64_t total = 0, page = 1, perPage = 0, pages = 0;
    bool hasNext = false, hasPrev = false;
};

class Query : public QueryBase<Query> {
  public:
    std::vector<Document> exec();  // Documents (you can set / save / populate them)
    std::vector<Value> lean();     // plain Values only (faster, for read-only work)
    int64_t count();               // countDocuments with this filter
    int64_t countDocuments() { return count(); }
    std::vector<Value> distinct(const std::string &field);
    Cursor cursor();                                          // for (auto d = c.next(); d; d = c.next())
    void forEach(const std::function<void(Document &)> &fn);  // stream every matching document
    Page paginate(int64_t page, int64_t perPage);             // page starts at 1

  private:
    friend class Model;
    using QueryBase::QueryBase;
};

class QueryOne : public QueryBase<QueryOne> {
  public:
    std::optional<Document> exec();
    std::optional<Value> lean();
    Document orFail();  // NotFoundError if nothing matches

  private:
    friend class Model;
    using QueryBase::QueryBase;
};

/* ---------------- Change streams ---------------- */
struct WatchOptions {
    bool fullDocument = false;  // include the whole document on updates ("updateLookup")
    int64_t maxAwaitMS = 1000;  // how long next() waits for an event
};
// stream = User.watch();  while (running) if (auto ev = stream.next()) { (*ev)["operationType"] ... }
class ChangeStream {
  public:
    std::optional<Value> next();  // nullopt = no event within maxAwaitMS (call again)

  private:
    friend class Model;
    std::shared_ptr<ChangeStreamImpl> impl_;
};

/* ---------------- Aggregation builder ----------------
   Point.aggregate()
       .match({{"device", id}, {"at", {{"$gte", "2026-11-17"}}}})   // cast with the schema (string id -> ObjectId ...)
       .group("device", {avg("value"), max("value"), count()})      // -> {_id, avgValue, maxValue, count}
       .sort("-avgValue")
       .limit(5)
       .exec();                                                     */

// One accumulator of group():  avg("value") -> output field "avgValue" = {$avg: "$value"}
struct Accumulator {
    std::string op;                            // "$avg", "$sum", ...
    Value expr;                                // "$value" or 1
    std::string name;                          // output field
    Accumulator &as(std::string outputName) {  // avg("value").as("meanTemp")
        name = std::move(outputName);
        return *this;
    }
};
Accumulator sum(const std::string &field);  // field: "value", "reading.temp" or an expression string "$value"
Accumulator avg(const std::string &field);
Accumulator min(const std::string &field);
Accumulator max(const std::string &field);
Accumulator first(const std::string &field);  // first value in the group (after sort())
Accumulator last(const std::string &field);
Accumulator push(const std::string &field);      // array of all values
Accumulator addToSet(const std::string &field);  // array of distinct values
Accumulator count();                             // number of documents -> "count"
Accumulator stdDevPop(const std::string &field);
Accumulator accumulator(const std::string &op, Value expr, std::string name);  // anything else: ("$top", {...}, "x")

struct AggregateOptions {
    bool allowDiskUse = false;  // big $group / $sort may use disk
    int64_t maxTimeMS = 0;
    Value collation;
    Value hint;
};

class Aggregate {
  public:
    // $match. Cast with the schema while the documents still have the schema's shape
    // (i.e. before group / project / addFields / replaceRoot / raw stage). Throws CastError on bad values.
    Aggregate &match(const Value &filter);
    // $group by "" (everything), "device", or several fields "device type" -> _id {device, type}
    Aggregate &group(const std::string &by, std::vector<Accumulator> accumulators = {});
    Aggregate &sort(const Value &spec);  // "-avgValue name" or {{"avgValue", -1}}
    Aggregate &limit(int64_t n);
    Aggregate &skip(int64_t n);
    Aggregate &project(const Value &spec);  // "name email -_id" or {{"total", {{"$multiply", ...}}}}
    Aggregate &project(const char *spec) { return project(Value(spec)); }
    Aggregate &addFields(const Value &fields);  // {{"total", {{"$multiply", Value::array({"$price", "$qty"})}}}}
    Aggregate &unwind(const std::string &path, bool keepEmpty = false);  // "tags" or "$tags"
    // $lookup: `from` may be a model name ("Author") or a collection name
    Aggregate &lookup(const std::string &from, const std::string &localField, const std::string &foreignField,
                      const std::string &as);
    Aggregate &count(const std::string &as = "count");  // $count: one document {as: n}
    Aggregate &sample(int64_t n);
    Aggregate &replaceRoot(const std::string &path);  // "$doc" or "doc"
    Aggregate &stage(const Value &rawStage);          // anything else: {{"$facet", {...}}}
    Aggregate &options(AggregateOptions o);

    std::vector<Value> exec() const;
    template <class T>
    std::vector<T> exec() const {  // results into NEVLA_STRUCT structs (group key "_id" -> member "id")
        std::vector<T> out;
        for (auto &v : exec()) out.push_back(v.template as<T>());
        return out;
    }
    std::optional<Value> first() const;  // only the first result (adds $limit 1)
    Value pipeline() const;              // the stages as they will be sent (incl. discriminator $match)
    std::string toJson(int indent = 2) const { return pipeline().toJson(indent); }

  private:
    friend class Model;
    explicit Aggregate(std::shared_ptr<ModelImpl> m) : m_(std::move(m)) {}
    std::shared_ptr<ModelImpl> m_;
    Value stages_ = Value::array();
    bool reshaped_ = false;  // after this the schema no longer describes the documents
    AggregateOptions opts_;
};

/* ---------------- Model ---------------- */
class Model {
  public:
    Model() = default;
    const std::string &name() const;
    const std::string &collectionName() const;
    const Schema &schema() const;

    // New document (not in the DB yet):  auto u = User({{"name", "Asha"}});  u.save();
    Document operator()(const Value &object = Value::object()) const;
    Document hydrate(const Value &object) const;  // data from the DB -> Document (isNew = false)

    Document create(const Value &object) const;  // build + validate + save
    std::vector<Document> insertMany(const std::vector<Value> &objects, InsertManyOptions options = {}) const;

    Query find(const Value &filter = Value::object()) const;
    QueryOne findOne(const Value &filter = Value::object()) const;
    QueryOne findById(const Value &id) const;
    Query where(const std::string &path) const;  // User.where("age").gte(18).exec()

    UpdateResult updateOne(const Value &filter, const Value &update, UpdateOptions options = {}) const;
    UpdateResult updateMany(const Value &filter, const Value &update, UpdateOptions options = {}) const;
    UpdateResult replaceOne(const Value &filter, const Value &replacement, UpdateOptions options = {}) const;
    std::optional<Document> findOneAndUpdate(const Value &filter, const Value &update,
                                             FindUpdateOptions options = {}) const;
    std::optional<Document> findByIdAndUpdate(const Value &id, const Value &update,
                                              FindUpdateOptions options = {}) const;
    std::optional<Document> findOneAndReplace(const Value &filter, const Value &replacement,
                                              FindUpdateOptions options = {}) const;
    std::optional<Document> findOneAndDelete(const Value &filter) const;
    std::optional<Document> findByIdAndDelete(const Value &id) const;

    int64_t deleteOne(const Value &filter) const;
    int64_t deleteMany(const Value &filter = Value::object()) const;

    /* Mongoose-format operations:
       {{"insertOne", {{"document", {...}}}}}, {{"updateOne", {{"filter", ...}, {"update", ...}, {"upsert", true}}}},
       updateMany, replaceOne ({"replacement"}), deleteOne, deleteMany */
    BulkWriteResult bulkWrite(const std::vector<Value> &operations, bool ordered = true) const;

    int64_t countDocuments(const Value &filter = Value::object()) const;
    int64_t estimatedDocumentCount() const;
    bool exists(const Value &filter) const;
    std::vector<Value> distinct(const std::string &field, const Value &filter = Value::object()) const;
    std::vector<Value> aggregate(const Value &pipeline) const;  // raw stages ($match stages are cast too)
    Aggregate aggregate() const;                                // chainable builder (see Aggregate)

    ChangeStream watch(const Value &pipeline = Value::array(), WatchOptions options = {}) const;  // replica set

    Value call(const std::string &staticName, const Value &args = Value()) const;  // schema.static_()

    void createIndexes() const;  // create the schema's indexes now (throws on failure)
    void syncIndexes() const;    // createIndexes() + drop indexes that are no longer in the schema
    std::vector<Value> listIndexes() const;
    void dropCollection() const;

    // Different document "types" in one collection (Mongoose discriminator)
    Model discriminator(const std::string &name, const Schema &extra) const;

    explicit operator bool() const { return m_ != nullptr; }

  private:
    friend Model model(const std::string &, const Schema &, const std::string &);
    friend Model model(const std::string &);
    explicit Model(std::shared_ptr<ModelImpl> m) : m_(std::move(m)) {}
    std::shared_ptr<ModelImpl> m_;
};

// Create a model (empty collection = from the name: "User" -> "users", "Person" -> "people")
Model model(const std::string &name, const Schema &schema, const std::string &collection = "");
// A model created earlier, by name
Model model(const std::string &name);
// All model names
std::vector<std::string> modelNames();

}  // namespace nevla
