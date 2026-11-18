#pragma once
/* Model / Document / Query (Mongoose jaisa API).

     nevla::connect("mongodb://127.0.0.1:27017/mydb");
     auto User = nevla::model("User", userSchema);
     auto u = User.create({{"name", "Asha"}, {"email", "asha@x.com"}});
     auto admins = User.find({{"role", "admin"}}).sort("-createdAt").limit(10).exec();                    */

#include "schema.hpp"

#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nevla {

struct ModelImpl;
class Model;

/* ---------------- Connection ---------------- */
// dbName khaali ho to URI ka database ("mongodb://host/mydb"), wo bhi na ho to "test"
void connect(const std::string &uri, const std::string &dbName = "");
void disconnect();
bool connected();
// Is thread par fn() ke andar ke saare nevla operations ek transaction me (replica set / Atlas chahiye).
// fn exception phenke to sab wapas (rollback).
void transaction(const std::function<void()> &fn);
// Blocking kaam ko alag thread par:  auto fut = nevla::async([&] { return User.find().exec(); });
template <class F>
auto async(F &&fn) {
    return std::async(std::launch::async, std::forward<F>(fn));
}

/* ---------------- Document ---------------- */
class Document {
  public:
    Document() = default;

    // Padhna ("address.city" jaisa dotted path, virtuals bhi)
    Value get(std::string_view path) const;
    template <class T>
    T get(std::string_view path) const {
        return get(path).template as<T>();
    }
    // doc["a.b"]: reference lautata hai (na ho to null), isliye `for (auto &e : doc["tags"].elements())` safe hai
    const Value &operator[](std::string_view path) const;
    bool has(std::string_view path) const;
    std::string id() const;  // _id text me
    Value _id() const;

    // Badalna (cast + setters lagte hain, change track hota hai)
    Document &set(std::string_view path, const Value &value);
    Document &set(const Value &object);  // kai fields ek saath
    Document &unset(std::string_view path);
    Document &push(std::string_view arrayPath, const Value &item);
    Value &mut(std::string_view path);          // seedha badalne ke liye reference (path modified mark ho jata hai)
    void markModified(std::string_view path);

    bool isNew() const { return isNew_; }
    bool isModified(std::string_view path = "") const;
    std::vector<std::string> modifiedPaths() const;

    void validate();          // fail -> ValidationError
    Document &save();         // naya: insert, purana: sirf badle hue paths ($set / $unset)
    void deleteOne();
    Document &populate(const std::string &path);
    Value call(const std::string &method, const Value &args = Value());  // schema.method()

    Value toObject() const;                // raw data
    Value toJSON() const;                  // + virtuals (option) + toJSON transform
    std::string json(int indent = -1) const { return toJSON().toJson(indent); }
    template <class T>
    T as() const {  // NEVLA_STRUCT wale struct me
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
    mutable std::map<std::string, Value> virtualCache_;
    bool isNew_ = true;
};

/* ---------------- Query ---------------- */
namespace detail {
struct QuerySpec {
    Value filter = Value::object();
    Value sort;
    int64_t limit = 0, skip = 0;
    std::string select;
    std::vector<std::string> populate;
};
Value parseSort(const Value &spec);
}  // namespace detail

template <class D>
class QueryBase {
  public:
    // where("age").gte(18).lte(60)   |   where("role", "admin")
    D &where(std::string path) {
        path_ = std::move(path);
        return self();
    }
    D &where(const std::string &path, Value value) {
        spec_.filter[path] = std::move(value);
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
    D &exists(bool yes = true) { return op("$exists", yes); }
    D &regex(std::string pattern, std::string options = "") {
        op("$regex", std::move(pattern));
        if (!options.empty()) op("$options", std::move(options));
        return self();
    }
    // sort("-createdAt name")  ya  sort({{"createdAt", -1}})
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
    // select("name email") | select("-password") | select("+password") (select(false) wala field bhi lao)
    D &select(const std::string &fields) {
        spec_.select += " " + fields;
        return self();
    }
    D &populate(const std::string &path) {
        spec_.populate.push_back(path);
        return self();
    }
    const Value &filter() const { return spec_.filter; }

  protected:
    QueryBase(std::shared_ptr<ModelImpl> m, Value filter) : m_(std::move(m)) {
        if (filter.isObject()) spec_.filter = std::move(filter);
    }
    D &self() { return static_cast<D &>(*this); }
    D &op(const char *name, Value v) {
        Value &cur = spec_.filter[path_];
        if (!cur.isObject()) cur = Value::object();
        cur[name] = std::move(v);
        return self();
    }
    std::shared_ptr<ModelImpl> m_;
    detail::QuerySpec spec_;
    std::string path_;
};

class Query : public QueryBase<Query> {
  public:
    std::vector<Document> exec();  // Documents (save / set / populate kar sakte ho)
    std::vector<Value> lean();     // sirf plain Values (tez, read-only kaam ke liye)
    int64_t count();

  private:
    friend class Model;
    using QueryBase::QueryBase;
};

class QueryOne : public QueryBase<QueryOne> {
  public:
    std::optional<Document> exec();
    std::optional<Value> lean();
    Document orFail();  // na mile to NotFoundError

  private:
    friend class Model;
    using QueryBase::QueryBase;
};

/* ---------------- Model ---------------- */
struct UpdateOptions {
    bool upsert = false;
    bool runValidators = false;  // $set ke values par schema validators
};
struct FindUpdateOptions {
    bool returnNew = false;  // Mongoose ka { new: true }: update ke BAAD wala document lautao
    bool upsert = false;
    bool runValidators = false;
};
struct UpdateResult {
    int64_t matchedCount = 0, modifiedCount = 0;
    Value upsertedId;  // upsert se naya bana ho to uski _id
};

class Model {
  public:
    Model() = default;
    const std::string &name() const;
    const std::string &collectionName() const;
    const Schema &schema() const;

    // Naya document (abhi DB me nahi):  auto u = User({{"name", "Asha"}});  u.save();
    Document operator()(const Value &object = Value::object()) const;
    Document hydrate(const Value &object) const;  // DB se aaya data -> Document (isNew = false)

    Document create(const Value &object) const;  // banao + validate + save
    std::vector<Document> insertMany(const std::vector<Value> &objects) const;

    Query find(const Value &filter = Value::object()) const;
    QueryOne findOne(const Value &filter = Value::object()) const;
    QueryOne findById(const Value &id) const;

    UpdateResult updateOne(const Value &filter, const Value &update, UpdateOptions options = {}) const;
    UpdateResult updateMany(const Value &filter, const Value &update, UpdateOptions options = {}) const;
    std::optional<Document> findOneAndUpdate(const Value &filter, const Value &update,
                                             FindUpdateOptions options = {}) const;
    std::optional<Document> findByIdAndUpdate(const Value &id, const Value &update,
                                              FindUpdateOptions options = {}) const;
    std::optional<Document> findOneAndDelete(const Value &filter) const;
    std::optional<Document> findByIdAndDelete(const Value &id) const;

    int64_t deleteOne(const Value &filter) const;
    int64_t deleteMany(const Value &filter = Value::object()) const;

    int64_t countDocuments(const Value &filter = Value::object()) const;
    int64_t estimatedDocumentCount() const;
    bool exists(const Value &filter) const;
    std::vector<Value> distinct(const std::string &field, const Value &filter = Value::object()) const;
    std::vector<Value> aggregate(const Value &pipeline) const;  // pipeline = array of stages

    void createIndexes() const;   // schema ke saare indexes abhi banao
    void dropCollection() const;

    // Ek hi collection me alag "type" ke documents (Mongoose discriminator)
    Model discriminator(const std::string &name, const Schema &extra) const;

    explicit operator bool() const { return m_ != nullptr; }

  private:
    friend Model model(const std::string &, const Schema &, const std::string &);
    friend Model model(const std::string &);
    explicit Model(std::shared_ptr<ModelImpl> m) : m_(std::move(m)) {}
    std::shared_ptr<ModelImpl> m_;
};

// Model banao (collection khaali ho to naam se: "User" -> "users", "Category" -> "categories")
Model model(const std::string &name, const Schema &schema, const std::string &collection = "");
// Pehle bana hua model naam se
Model model(const std::string &name);

}  // namespace nevla
