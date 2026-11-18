#include "nevla/model.hpp"

#include <algorithm>
#include <atomic>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <cmath>
#include <iostream>
#include <map>
#include <mongocxx/bulk_write.hpp>
#include <mongocxx/change_stream.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/hint.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/model/delete_many.hpp>
#include <mongocxx/model/delete_one.hpp>
#include <mongocxx/model/insert_one.hpp>
#include <mongocxx/model/replace_one.hpp>
#include <mongocxx/model/update_many.hpp>
#include <mongocxx/model/update_one.hpp>
#include <mongocxx/options/change_stream.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_replace.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/insert.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>
#include <mutex>
#include <sstream>

#include "bson.hpp"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

namespace nevla {

using detail::fromBson;
using detail::toBson;

struct ModelImpl {
    std::string name, collection;
    Schema schema;
    std::string discKey, discValue;  // set on a discriminator child
    std::mutex childMu;
    std::map<std::string, std::shared_ptr<ModelImpl>> children;  // base model: type -> child
    std::atomic<uint64_t> indexGen{0};
    std::mutex indexMu;

    static Document make(const std::shared_ptr<ModelImpl> &m, Value data, bool isNew) {
        Document d;
        d.m_ = m;
        d.data_ = std::move(data);
        d.isNew_ = isNew;
        return d;
    }
    static void markPopulated(Document &d, const std::string &path) { d.populated_.insert(path); }
};

namespace {

/* ======================= connection ======================= */
// A connection is shared: every running operation holds a reference, so connect() / disconnect() on another
// thread never pulls the pool out from under it.
struct Conn {
    std::unique_ptr<mongocxx::pool> pool;
    std::string db;
    uint64_t gen = 0;
};
std::mutex gConnMu;
std::shared_ptr<Conn> gConn;
std::atomic<uint64_t> gGeneration{0};

std::mutex gModelsMu;
std::map<std::string, std::shared_ptr<ModelImpl>> gModels;

std::mutex gHookMu;
std::atomic<bool> gDebug{false};
std::function<void(const std::string &, const std::string &, const Value &)> gDebugFn;
std::function<void(const std::string &, const std::string &)> gIndexErrorFn;
std::atomic<int64_t> gSlowMs{0};
std::function<void(const SlowQuery &)> gSlowFn;
std::atomic<bool> gStrictQuery{false};

std::shared_ptr<Conn> currentConn() {
    std::lock_guard<std::mutex> lk(gConnMu);
    if (!gConn) throw Error("nevla: not connected (call nevla::connect(uri) first)");
    return gConn;
}

struct Tx {
    std::shared_ptr<Conn> conn;
    mongocxx::client *client = nullptr;
    mongocxx::client_session *session = nullptr;
};
thread_local Tx tTx;

// mongocxx / bsoncxx exceptions -> nevla::Error
template <class F>
auto guard(F &&f) -> decltype(f()) {
    try {
        return f();
    } catch (const Error &) {
        throw;
    } catch (const mongocxx::operation_exception &e) {
        std::string what = e.what();
        if (e.code().value() == 11000 || what.find("E11000") != std::string::npos) throw DuplicateKeyError(what);
        throw Error("MongoDB: " + what);
    } catch (const mongocxx::exception &e) {
        throw Error(std::string("MongoDB: ") + e.what());
    } catch (const bsoncxx::exception &e) {
        throw Error(std::string("BSON: ") + e.what());
    }
}

// A read whose plan the server killed (collection created / dropped / index built mid-query) is safe to run again.
// Not inside a transaction (the driver retries the whole transaction there).
template <class F>
void retryRead(F &&fn) {
    for (int attempt = 1;; ++attempt) {
        try {
            fn();
            return;
        } catch (const mongocxx::operation_exception &e) {
            int code = e.code().value();
            bool transient = code == 175 /* QueryPlanKilled */ || code == 237 /* CursorKilled */;
            if (!transient || attempt >= 3 || tTx.client) throw;
        }
    }
}

void debugLog(const std::string &coll, const std::string &op, const Value &args) {
    if (!gDebug.load()) return;
    std::function<void(const std::string &, const std::string &, const Value &)> fn;
    {
        std::lock_guard<std::mutex> lk(gHookMu);
        fn = gDebugFn;
    }
    if (fn)
        fn(coll, op, args);
    else
        std::cerr << "nevla: " << coll << "." << op << "(" << args.toJson() << ")\n";
}

void reportIndexError(const std::string &model, const std::string &msg) {
    std::function<void(const std::string &, const std::string &)> fn;
    {
        std::lock_guard<std::mutex> lk(gHookMu);
        fn = gIndexErrorFn;
    }
    if (fn)
        fn(model, msg);
    else
        std::cerr << "nevla: could not create indexes for model \"" << model << "\": " << msg << "\n";
}

// Index suggestion with the ESR rule: Equality fields, then Sort fields, then Range fields
std::string suggestIndex(const Value &args) {
    Value filter = args["filter"];
    if (args["pipeline"].isArray())
        for (const auto &st : args["pipeline"].elements()) {
            if (st.contains("$match")) filter = st["$match"];
            break;
        }
    std::vector<std::string> eq, range, sortKeys;
    for (const auto &kv : filter.items()) {
        if (kv.first.empty() || kv.first[0] == '$' || kv.first == "_id") continue;
        bool isRange = false;
        if (kv.second.isObject())
            for (const auto &op : kv.second.items())
                isRange |= op.first == "$gt" || op.first == "$gte" || op.first == "$lt" || op.first == "$lte" ||
                           op.first == "$ne" || op.first == "$nin" || op.first == "$regex" || op.first == "$exists";
        (isRange ? range : eq).push_back(kv.first);
    }
    Value sort = args["sort"];
    std::string out;
    std::set<std::string> used;
    auto add = [&](const std::string &f, const Value &dir) {
        if (!used.insert(f).second) return;
        out += (out.empty() ? "" : ", ") + f + ": " + dir.str();
    };
    for (const auto &f : eq) add(f, 1);
    for (const auto &kv : sort.items()) add(kv.first, kv.second);
    for (const auto &f : range) add(f, 1);
    return out.empty() ? "" : "consider an index {" + out + "}";
}

// Logs an operation (setDebug) and reports it when it is slow (setSlowQueryThreshold)
class OpTrace {
  public:
    OpTrace(const std::string &coll, const std::string &op, Value args) {
        debugLog(coll, op, args);
        int64_t limit = gSlowMs.load();
        if (limit <= 0) return;
        active_ = true;
        limit_ = limit;
        coll_ = coll;
        op_ = op;
        args_ = std::move(args);
        t0_ = std::chrono::steady_clock::now();
    }
    ~OpTrace() {
        if (!active_) return;
        int64_t ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0_).count();
        if (ms < limit_) return;
        try {
            SlowQuery q{coll_, op_, ms, args_, suggestIndex(args_)};
            std::function<void(const SlowQuery &)> fn;
            {
                std::lock_guard<std::mutex> lk(gHookMu);
                fn = gSlowFn;
            }
            if (fn)
                fn(q);
            else
                std::cerr << "nevla: slow " << coll_ << "." << op_ << " took " << ms << " ms: " << args_.toJson()
                          << (q.suggestion.empty() ? "" : " -> " + q.suggestion) << "\n";
        } catch (...) {  // a logger must never break the operation
        }
    }
    OpTrace(const OpTrace &) = delete;
    OpTrace &operator=(const OpTrace &) = delete;

  private:
    bool active_ = false;
    int64_t limit_ = 0;
    std::string coll_, op_;
    Value args_;
    std::chrono::steady_clock::time_point t0_;
};

// Members are destroyed in reverse order: coll -> entry -> conn (pool outlives the client)
struct Op {
    std::shared_ptr<Conn> conn;
    std::optional<mongocxx::pool::entry> entry;
    mongocxx::client_session *session = nullptr;
    mongocxx::collection coll;
};

// Inside nevla::transaction() use the session, otherwise call directly
#define NV(o, call, ...) \
    ((o).session ? (o).coll.call(*(o).session __VA_OPT__(, ) __VA_ARGS__) : (o).coll.call(__VA_ARGS__))

/* ---------------- indexes ---------------- */
struct IndexDef {
    Value keys, options;
};

void collectFieldIndexes(const Schema &s, const std::string &prefix, std::vector<IndexDef> &out) {
    for (const auto &kv : s.fields()) {
        const Field &f = kv.second;
        std::string path = prefix + kv.first;
        if (f.unique_ || f.index_ || f.expires_) {
            IndexDef d{Value::object(), Value::object()};
            d.keys[path] = 1;
            if (f.unique_) d.options["unique"] = true;
            if (f.sparse_) d.options["sparse"] = true;
            if (f.expires_) d.options["expireAfterSeconds"] = *f.expires_;
            out.push_back(std::move(d));
        }
        if ((f.type == FieldType::Object || f.type == FieldType::Array) && f.schema_)
            collectFieldIndexes(*f.schema_, path + ".", out);
    }
}

std::vector<IndexDef> wantedIndexes(ModelImpl &m) {
    std::vector<IndexDef> out;
    collectFieldIndexes(m.schema, "", out);
    for (const auto &i : m.schema.indexes_) out.push_back({i.keys, i.options});
    return out;
}

void createIndexesNow(ModelImpl &m, Conn &c) {
    guard([&] {
        auto entry = c.pool->acquire();
        auto coll = (*entry)[c.db][m.collection];
        for (const auto &i : wantedIndexes(m)) {
            OpTrace nv_trace(m.collection, "createIndex", Value{{"keys", i.keys}, {"options", i.options}});
            coll.create_index(toBson(i.keys), toBson(i.options));
        }
    });
}

void ensureIndexes(ModelImpl &m, Conn &c) {
    if (m.indexGen.load() == c.gen) return;
    std::lock_guard<std::mutex> lk(m.indexMu);
    if (m.indexGen.load() == c.gen) return;
    if (m.schema.options.autoIndex) {
        // A failing index (e.g. duplicates already stored) must not block every operation of the model
        try {
            createIndexesNow(m, c);
        } catch (const std::exception &e) {
            reportIndexError(m.name, e.what());
        }
    }
    m.indexGen = c.gen;
}

Op open(ModelImpl &m) {
    Op o;
    if (tTx.client) {
        o.conn = tTx.conn;
        ensureIndexes(m, *o.conn);
        o.session = tTx.session;
        o.coll = (*tTx.client)[o.conn->db][m.collection];
    } else {
        o.conn = currentConn();
        ensureIndexes(m, *o.conn);
        o.entry.emplace(o.conn->pool->acquire());
        o.coll = (**o.entry)[o.conn->db][m.collection];
    }
    return o;
}

/* ---------------- helpers ---------------- */
bool endsWith(const std::string &s, const std::string &x) {
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

// Same idea as Mongoose's pluralize(): "User" -> "users", "Category" -> "categories", "Person" -> "people"
std::string pluralize(const std::string &name) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    if (s.empty()) return s;
    static const char *uncountable[] = {"advice", "energy", "equipment", "information", "money",   "species", "series",
                                        "rice",   "fish",   "sheep",     "moose",       "deer",    "news",    "media",
                                        "data",   "status", "metadata",  "feedback",    "software"};
    for (const char *u : uncountable)
        if (endsWith(s, u)) return s;
    static const std::pair<const char *, const char *> irregular[] = {
        {"person", "people"}, {"woman", "women"}, {"man", "men"},   {"child", "children"}, {"mouse", "mice"},
        {"goose", "geese"},   {"tooth", "teeth"}, {"foot", "feet"}, {"ox", "oxen"},        {"quiz", "quizzes"}};
    for (const auto &ir : irregular)
        if (std::string(ir.first).size() <= 2 ? s == ir.first : endsWith(s, ir.first))
            return s.substr(0, s.size() - std::string(ir.first).size()) + ir.second;
    if (endsWith(s, "matrix") || endsWith(s, "vertex") || endsWith(s, "index"))
        return s.substr(0, s.size() - 2) + "ices";
    if (endsWith(s, "y") && s.size() > 1 && std::string("aeiou").find(s[s.size() - 2]) == std::string::npos)
        return s.substr(0, s.size() - 1) + "ies";
    if (endsWith(s, "fe")) return s.substr(0, s.size() - 2) + "ves";
    if ((endsWith(s, "lf") || endsWith(s, "rf"))) return s.substr(0, s.size() - 1) + "ves";
    if (endsWith(s, "s") || endsWith(s, "x") || endsWith(s, "z") || endsWith(s, "ch") || endsWith(s, "sh"))
        return s + "es";
    return s + "s";
}

std::shared_ptr<ModelImpl> lookup(const std::string &name) {
    std::lock_guard<std::mutex> lk(gModelsMu);
    auto it = gModels.find(name);
    if (it == gModels.end())
        throw Error("nevla: model \"" + name + "\" is not registered (call nevla::model(name, schema) first)");
    return it->second;
}

[[noreturn]] void throwValidation(const std::string &model, const std::map<std::string, std::string> &errors,
                                  const std::map<std::string, std::string> &kinds = {}) {
    std::string msg = model + " validation failed: ";
    bool first = true;
    for (const auto &e : errors) {
        if (!first) msg += ", ";
        first = false;
        msg += e.first + ": " + e.second;
    }
    throw ValidationError(msg, errors, kinds);
}

void runDocHooks(const std::map<std::string, std::vector<Schema::DocHook>> &hooks, const std::string &name,
                 Document &doc) {
    auto it = hooks.find(name);
    if (it == hooks.end()) return;
    for (const auto &h : it->second) h(doc);
}

void runQueryHooks(ModelImpl &m, const std::string &op, Value &filter, Value *update) {
    auto it = m.schema.preQuery_.find(op);
    if (it == m.schema.preQuery_.end()) return;
    QueryInfo info{op, m.name, filter, update};
    for (const auto &h : it->second) h(info);
}

void runPostHooks(ModelImpl &m, const std::string &op, Value &filter, Value *update, Value &result) {
    auto it = m.schema.postQuery_.find(op);
    if (it == m.schema.postQuery_.end()) return;
    QueryInfo info{op, m.name, filter, update};
    for (const auto &h : it->second) h(info, result);
}

bool hasPostHooks(ModelImpl &m, const std::string &op) { return m.schema.postQuery_.count(op) > 0; }

bool isOperatorObject(const Value &v) {
    if (!v.isObject() || v.empty()) return false;
    for (const auto &m : v.items())
        if (m.first.empty() || m.first[0] != '$') return false;
    return true;
}

Value castFilter(const Schema &s, const Value &filter, const std::string *strictModel = nullptr);

// One filter value in the field's type (for an array field a single element also matches)
Value castScalar(const Schema &s, const Field &f, const Value &v, const std::string &path) {
    if (v.isNull()) return v;
    if (path == "_id" && !s.hasOwnId())
        return v.isString() && ObjectId::isValid(v.asString()) ? Value(ObjectId(v.asString())) : v;
    if (f.type == FieldType::Array && !v.isArray()) {
        if (f.schema_ || !f.of_) return v;
        return detail::castValue(*f.of_, v, path);
    }
    if (f.type == FieldType::Object || f.type == FieldType::Map) return v;
    return detail::castValue(f, v, path);
}

Value castOperand(const Schema &s, const Field &f, const Value &v, const std::string &path) {
    if (!isOperatorObject(v)) return castScalar(s, f, v, path);
    Value out = Value::object();
    for (const auto &m : v.items()) {
        const std::string &op = m.first;
        if (op == "$in" || op == "$nin" || op == "$all") {
            Value arr = Value::array();
            for (const auto &e : m.second.elements()) arr.push_back(castScalar(s, f, e, path));
            out[op] = std::move(arr);
        } else if (op == "$eq" || op == "$ne" || op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte") {
            out[op] = castScalar(s, f, m.second, path);
        } else if (op == "$not") {
            out[op] = castOperand(s, f, m.second, path);
        } else if (op == "$elemMatch" && f.type == FieldType::Array && m.second.isObject()) {
            if (f.schema_)
                out[op] = castFilter(*f.schema_, m.second);
            else if (f.of_ && isOperatorObject(m.second))
                out[op] = castOperand(s, *f.of_, m.second, path);
            else
                out[op] = m.second;
        } else {
            out[op] = m.second;  // $exists, $regex, $options, $size, $type, $mod ...
        }
    }
    return out;
}

size_t editDistance(const std::string &a, const std::string &b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                               prev[j - 1] + (std::tolower(a[i - 1]) == std::tolower(b[j - 1]) ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

void knownPaths(const Schema &s, const std::string &prefix, std::vector<std::string> &out) {
    for (const auto &kv : s.fields()) {
        out.push_back(prefix + kv.first);
        if (!kv.second.alias_.empty()) out.push_back(prefix + kv.second.alias_);
        if ((kv.second.type == FieldType::Object || kv.second.type == FieldType::Array) && kv.second.schema_)
            knownPaths(*kv.second.schema_, prefix + kv.first + ".", out);
    }
    if (prefix.empty()) {
        if (s.options.timestamps) {
            out.push_back(s.options.createdAt);
            out.push_back(s.options.updatedAt);
        }
    }
}

[[noreturn]] void unknownPath(const Schema &s, const std::string &model, const std::string &path) {
    std::vector<std::string> known;
    knownPaths(s, "", known);
    std::string best;
    size_t bestD = std::max<size_t>(2, path.size() / 3) + 1;
    for (const auto &k : known) {
        size_t d = editDistance(path, k);
        if (d < bestD) {
            bestD = d;
            best = k;
        }
    }
    throw Error("nevla: unknown path \"" + path + "\" in a filter on model \"" + model + "\" (strictQuery)" +
                (best.empty() ? "" : ". Did you mean \"" + best + "\"?"));
}

// strictModel != nullptr: a path that is not in the schema throws (strictQuery)
Value castFilter(const Schema &s, const Value &filter, const std::string *strictModel) {
    Value out = Value::object();
    for (const auto &m : filter.items()) {
        if (m.first == "$and" || m.first == "$or" || m.first == "$nor") {
            Value arr = Value::array();
            for (const auto &e : m.second.elements()) arr.push_back(castFilter(s, e, strictModel));
            out[m.first] = std::move(arr);
            continue;
        }
        std::string key = m.first.empty() || m.first[0] == '$' ? m.first : s.resolveAlias(m.first);
        bool mixed = false;
        const Field *f = key.empty() || key[0] == '$' ? nullptr : s.path(key, &mixed);
        if (!f && strictModel && !key.empty() && key[0] != '$') {
            std::string top = key.substr(0, key.find('.'));
            if (top != s.options.versionKey && top != s.options.discriminatorKey) unknownPath(s, *strictModel, key);
        }
        out[key] = (f && !mixed) ? castOperand(s, *f, m.second, key) : m.second;
    }
    return out;
}

// A filter may be an object or a formula string ("age >= 18 && role == 'admin'")
Value asFilter(const Value &filter) {
    if (filter.isString()) return formulaFilter(filter.asString());
    return filter.isObject() ? filter : Value::object();
}

Value prepareFilter(ModelImpl &m, const Value &filter) {
    bool strict = m.schema.options.strictQuery || gStrictQuery.load();
    Value out = castFilter(m.schema, asFilter(filter), strict ? &m.name : nullptr);
    if (!m.discKey.empty()) out[m.discKey] = m.discValue;
    return out;
}

Value castElement(const Field &f, const Value &v, const std::string &path) {
    if (f.schema_) {
        Value sub = detail::castObject(*f.schema_, v, path + ".");
        detail::applyDefaults(*f.schema_, sub);
        return sub;
    }
    return f.of_ ? detail::castValue(*f.of_, v, path) : v;
}

bool keepUnknown(const ModelImpl &m, const std::string &path) {
    const SchemaOptions &o = m.schema.options;
    if (!o.strict) return true;
    std::string top = path.substr(0, path.find('.'));
    return top == "_id" || top == o.versionKey || top == o.discriminatorKey;
}

// Mongoose-style update casting:
//  - plain fields are wrapped in $set,  aliases resolved,  unknown paths dropped (strict)
//  - values cast for $set/$setOnInsert/$min/$max/$inc/$mul/$push/$addToSet/$pull
//  - immutable fields removed (or moved to $setOnInsert on upsert)
//  - runValidators: validators for $set/$setOnInsert, required for $unset
//  - timestamps, discriminator key and defaults on upsert
Value castUpdate(ModelImpl &m, const Value &update, bool upsert, bool runValidators, const Value &filter) {
    if (!update.isObject()) throw Error("nevla: update must be an object");
    const Schema &s = m.schema;
    const SchemaOptions &opt = s.options;
    Value u = Value::object();
    for (const auto &kv : update.items()) {
        if (!kv.first.empty() && kv.first[0] == '$') {
            for (const auto &p : kv.second.items()) u[kv.first][p.first] = p.second;  // merge ($set + plain)
            if (!kv.second.isObject()) u[kv.first] = kv.second;
        } else {
            u["$set"][kv.first] = kv.second;  // { name: "x" } -> { $set: { name: "x" } }
        }
    }

    std::map<std::string, std::string> errors, kinds;
    Value moveToInsert = Value::object();
    for (auto &op : u.items()) {
        if (!op.second.isObject()) continue;
        const std::string &name = op.first;
        bool setLike = name == "$set" || name == "$setOnInsert" || name == "$min" || name == "$max";
        bool numeric = name == "$inc" || name == "$mul";
        bool pushLike = name == "$push" || name == "$addToSet";
        bool pull = name == "$pull" || name == "$pullAll";
        bool unset = name == "$unset";
        if (!setLike && !numeric && !pushLike && !pull && !unset) continue;  // $rename, $currentDate, $bit ...

        Value out = Value::object();
        for (auto &kv : op.second.items()) {
            std::string path = s.resolveAlias(kv.first);
            bool mixed = false;
            const Field *f = s.path(path, &mixed);
            if (!f) {
                if (keepUnknown(m, path)) out[path] = kv.second;
                continue;  // strict: not in the schema -> dropped
            }
            if (mixed || (path == "_id" && !s.hasOwnId())) {
                out[path] = kv.second;
                continue;
            }
            if (f->immutable_ && name != "$setOnInsert") {
                if (upsert && setLike) moveToInsert[path] = detail::castValue(*f, kv.second, path);
                continue;  // immutable: never changed by an update
            }
            Value v = kv.second;
            if (setLike) {
                v = detail::castValue(*f, v, path);
                if (runValidators && (name == "$set" || name == "$setOnInsert"))
                    detail::validateValue(*f, &v, path, errors, &kinds);
            } else if (numeric) {
                if (f->type != FieldType::Number && f->type != FieldType::Int)
                    throw CastError(path, "Cast to Number failed: " + name + " on non-number path \"" + path + "\"");
                Field num = Number();
                v = detail::castValue(num, v, path);
            } else if (pushLike && f->type == FieldType::Array) {
                if (v.isObject() && v.contains("$each")) {
                    Value arr = Value::array();
                    for (const auto &e : v["$each"].elements()) arr.push_back(castElement(*f, e, path));
                    v["$each"] = std::move(arr);
                } else {
                    v = castElement(*f, v, path);
                }
            } else if (pull && f->type == FieldType::Array && f->of_ && !f->schema_) {
                if (name == "$pullAll" || v.isArray()) {
                    Value arr = Value::array();
                    for (const auto &e : v.elements()) arr.push_back(detail::castValue(*f->of_, e, path));
                    v = std::move(arr);
                } else if (isOperatorObject(v)) {
                    v = castOperand(s, *f->of_, v, path);
                } else {
                    v = detail::castValue(*f->of_, v, path);
                }
            } else if (pull && f->type == FieldType::Array && f->schema_ && v.isObject() && !isOperatorObject(v)) {
                v = castFilter(*f->schema_, v);
            } else if (unset && runValidators && f->required_) {
                errors[path] = f->requiredMessage_.empty() ? "Path `" + path + "` is required."
                                                           : detail::formatMessage(f->requiredMessage_, path, nullptr);
                kinds[path] = "required";
            }
            out[path] = std::move(v);
        }
        op.second = std::move(out);
    }
    if (!errors.empty()) throwValidation(m.name, errors, kinds);
    for (auto &kv : moveToInsert.items()) u["$setOnInsert"][kv.first] = std::move(kv.second);

    if (opt.timestamps) {
        Date now = Date::now();
        bool userSet = u["$set"].contains(opt.updatedAt);
        if (!userSet) u["$set"][opt.updatedAt] = now;
        if (u["$set"].contains(opt.createdAt)) u["$set"].erase(opt.createdAt);  // createdAt never changes
        if (upsert && !u["$setOnInsert"].contains(opt.createdAt)) u["$setOnInsert"][opt.createdAt] = now;
    }
    if (upsert && !m.discKey.empty()) u["$setOnInsert"][m.discKey] = m.discValue;
    if (upsert) {  // a new document gets the schema defaults too (Mongoose setDefaultsOnInsert)
        Value defaults = Value::object();
        detail::applyDefaults(s, defaults);
        for (auto &d : defaults.items()) {
            const std::string &field = d.first;
            auto hits = [&](const std::string &k) { return k == field || k.rfind(field + ".", 0) == 0; };
            bool touched = field == "_id";
            for (const auto &op : u.items())
                for (const auto &kv : op.second.items()) touched |= hits(kv.first);
            for (const auto &fk : filter.items()) touched |= hits(fk.first);
            if (!touched) u["$setOnInsert"][field] = std::move(d.second);
        }
    }
    // drop empty operators ({$set: {}} is an error in MongoDB)
    Value clean = Value::object();
    for (auto &op : u.items())
        if (op.second.isObject() ? !op.second.empty() : !op.second.isNull()) clean[op.first] = std::move(op.second);
    return clean;
}

Value buildProjection(const Schema &s, const std::string &select) {
    std::vector<std::string> include, exclude, plus;
    std::istringstream in(select);
    std::string tok;
    while (in >> tok) {
        if (tok[0] == '-')
            exclude.push_back(s.resolveAlias(tok.substr(1)));
        else if (tok[0] == '+')
            plus.push_back(s.resolveAlias(tok.substr(1)));
        else
            include.push_back(s.resolveAlias(tok));
    }
    Value proj = Value::object();
    if (!include.empty()) {
        for (const auto &p : include) proj[p] = 1;
        for (const auto &p : plus) proj[p] = 1;
        for (const auto &p : exclude)
            if (p == "_id") proj["_id"] = 0;
        return proj;
    }
    for (const auto &p : exclude) proj[p] = 0;
    std::vector<std::string> hidden;
    detail::hiddenPaths(s, "", hidden);
    for (const auto &h : hidden)
        if (std::find(plus.begin(), plus.end(), h) == plus.end()) proj[h] = 0;
    return proj;
}

std::shared_ptr<ModelImpl> pickModel(const std::shared_ptr<ModelImpl> &m, const Value &data) {
    std::lock_guard<std::mutex> lk(m->childMu);
    if (m->children.empty()) return m;
    auto it = m->children.find(data[m->schema.options.discriminatorKey].asString());
    return it == m->children.end() ? m : it->second;
}

void applyFindOptions(mongocxx::options::find &opts, const detail::QuerySpec &spec, const Schema &schema, bool one) {
    if (spec.sort.isObject() && !spec.sort.empty()) opts.sort(toBson(spec.sort));
    if (one)
        opts.limit(1);
    else if (spec.limit > 0)
        opts.limit(spec.limit);
    if (spec.skip > 0) opts.skip(spec.skip);
    if (spec.batchSize > 0) opts.batch_size(static_cast<int32_t>(spec.batchSize));
    if (spec.maxTimeMS > 0) opts.max_time(std::chrono::milliseconds(spec.maxTimeMS));
    if (spec.hint.isString()) opts.hint(mongocxx::hint(spec.hint.asString()));
    if (spec.hint.isObject()) opts.hint(mongocxx::hint(toBson(spec.hint)));
    if (spec.collation.isObject()) opts.collation(toBson(spec.collation));
    if (!spec.comment.empty()) opts.comment(spec.comment);
    Value proj = buildProjection(schema, spec.select);
    if (!proj.empty()) opts.projection(toBson(proj));
}

std::vector<Value> runFind(const std::shared_ptr<ModelImpl> &m, const detail::QuerySpec &spec, bool one,
                           const char *opName);

/* ---------------- populate ---------------- */
void collectSlots(Value &cur, std::string_view path, std::vector<Value *> &out) {
    if (path.empty()) {
        out.push_back(&cur);
        return;
    }
    if (cur.isArray()) {
        for (auto &e : cur.elements()) collectSlots(e, path, out);
        return;
    }
    auto dot = path.find('.');
    Value *next = cur.find(path.substr(0, dot));
    if (!next) return;
    collectSlots(*next, dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1), out);
}

const VirtualPopulate *findVirtualPopulate(const Schema &s, const std::string &path) {
    for (const auto &vp : s.virtualPopulates_)
        if (vp.first == path) return &vp.second;
    return nullptr;
}

// Does `candidate` (a value or an array of values) contain `v`?
bool containsValue(const Value &candidate, const Value &v) {
    if (candidate.isArray()) {
        for (const auto &e : candidate.elements())
            if (e == v) return true;
        return false;
    }
    return candidate == v;
}

void populateValues(const std::shared_ptr<ModelImpl> &m, std::vector<Value *> &docs, const PopulateOptions &opt);

void populateNested(const std::shared_ptr<ModelImpl> &ref, std::vector<Value> &rows, const PopulateOptions &opt) {
    if (opt.populate.empty() || rows.empty()) return;
    std::vector<Value *> ptrs;
    for (auto &r : rows) ptrs.push_back(&r);
    for (const auto &child : opt.populate) populateValues(ref, ptrs, child);
}

void populateVirtual(std::vector<Value *> &docs, const PopulateOptions &opt, const VirtualPopulate &vp) {
    auto ref = lookup(opt.model.empty() ? vp.ref : opt.model);
    Value locals = Value::array();
    std::set<std::string> seen;
    for (Value *d : docs) {
        const Value *lv = d->path(vp.localField);
        if (!lv) continue;
        auto add = [&](const Value &x) {
            if (!x.isNull() && seen.insert(x.toJson()).second) locals.push_back(x);
        };
        if (lv->isArray())
            for (const auto &x : lv->elements()) add(x);
        else
            add(*lv);
    }
    for (Value *d : docs) (*d)[opt.path] = vp.justOne ? Value() : Value::array();
    if (locals.empty()) return;

    detail::QuerySpec spec;
    spec.filter[vp.foreignField]["$in"] = locals;
    if (opt.match.isObject())
        for (const auto &kv : opt.match.items()) spec.filter[kv.first] = kv.second;
    if (!opt.sort.isNull()) spec.sort = detail::parseSort(opt.sort);
    spec.select = opt.select;
    if (!opt.select.empty() && opt.select.find_first_not_of(" ") != std::string::npos) {
        // inclusion projection: we still need the foreign field to match rows to documents
        Value proj = buildProjection(ref->schema, opt.select);
        bool inclusion = false;
        for (const auto &kv : proj.items()) inclusion |= kv.second.asInt() == 1;
        if (inclusion) spec.select += " " + vp.foreignField;
    }
    std::vector<Value> rows = runFind(ref, spec, false, "find");
    populateNested(ref, rows, opt);

    for (Value *d : docs) {
        const Value *lv = d->path(vp.localField);
        if (!lv) continue;
        Value matched = Value::array();
        for (const auto &r : rows) {
            const Value *fv = r.path(vp.foreignField);
            if (!fv) continue;
            bool hit = false;
            if (lv->isArray()) {
                for (const auto &x : lv->elements()) hit |= containsValue(*fv, x);
            } else {
                hit = containsValue(*fv, *lv);
            }
            if (!hit) continue;
            matched.push_back(r);
            if (opt.limit > 0 && static_cast<int64_t>(matched.size()) >= opt.limit) break;
        }
        if (vp.justOne)
            (*d)[opt.path] = matched.empty() ? Value() : matched.at(0);
        else
            (*d)[opt.path] = std::move(matched);
    }
}

void populateValues(const std::shared_ptr<ModelImpl> &m, std::vector<Value *> &docs, const PopulateOptions &opt) {
    const std::string path = m->schema.resolveAlias(opt.path);
    if (const VirtualPopulate *vp = findVirtualPopulate(m->schema, path)) {
        populateVirtual(docs, opt, *vp);
        return;
    }
    const Field *f = m->schema.path(path);
    if (f && f->type == FieldType::Array && f->of_) f = f->of_.get();
    std::string refName = !opt.model.empty() ? opt.model : f ? f->ref_ : "";
    if (!f || refName.empty())
        throw Error("nevla: populate(\"" + path + "\"): the field has no .ref(\"Model\") and is not a virtualPopulate");
    auto ref = lookup(refName);

    std::vector<Value *> slots;
    for (Value *d : docs) collectSlots(*d, path, slots);
    Value ids = Value::array();
    std::set<std::string> seen;
    auto want = [&](const Value &v) {
        if (v.isNull() || v.isObject()) return;  // empty or already populated
        if (seen.insert(v.toJson()).second) ids.push_back(v);
    };
    for (Value *s : slots) {
        if (s->isArray())
            for (const auto &e : s->elements()) want(e);
        else
            want(*s);
    }
    if (ids.empty()) return;

    detail::QuerySpec spec;
    Value byIds = Value::object();
    byIds["_id"]["$in"] = ids;
    if (opt.match.isObject() && !opt.match.empty())
        spec.filter["$and"] = Value::array({byIds, opt.match});
    else
        spec.filter = byIds;
    if (!opt.sort.isNull()) spec.sort = detail::parseSort(opt.sort);
    spec.select = opt.select;
    std::vector<Value> rows = runFind(ref, spec, false, "find");
    populateNested(ref, rows, opt);

    std::map<std::string, size_t> byId;  // id -> position in rows (rows are in sort order)
    for (size_t i = 0; i < rows.size(); i++) byId[rows[i]["_id"].toJson()] = i;
    bool sorted = !opt.sort.isNull();

    for (Value *s : slots) {
        if (s->isArray()) {
            std::vector<std::pair<size_t, Value>> found;
            for (const auto &e : s->elements()) {
                if (e.isObject()) {
                    found.emplace_back(0, e);
                    continue;
                }
                auto it = byId.find(e.toJson());
                if (it != byId.end()) found.emplace_back(it->second, rows[it->second]);  // missing ones are dropped
            }
            if (sorted)
                std::stable_sort(found.begin(), found.end(),
                                 [](const auto &a, const auto &b) { return a.first < b.first; });
            Value arr = Value::array();
            for (auto &x : found) {
                if (opt.limit > 0 && static_cast<int64_t>(arr.size()) >= opt.limit) break;
                arr.push_back(std::move(x.second));
            }
            *s = std::move(arr);
        } else if (!s->isNull() && !s->isObject()) {
            auto it = byId.find(s->toJson());
            *s = it == byId.end() ? Value() : rows[it->second];
        }
    }
}

// populated document(s) -> their _id (used by depopulate() and before saving)
void depopulateSlot(Value &v) {
    if (v.isArray()) {
        for (auto &e : v.elements()) depopulateSlot(e);
        return;
    }
    if (v.isObject() && v.contains("_id")) v = v["_id"];
}

void depopulatePath(const Schema &s, Value &data, const std::string &path) {
    if (findVirtualPopulate(s, path)) {
        data.erasePath(path);
        return;
    }
    std::vector<Value *> slots;
    collectSlots(data, path, slots);
    for (Value *v : slots) depopulateSlot(*v);
}

/* ---------------- find / update / delete ---------------- */
std::vector<Value> runFind(const std::shared_ptr<ModelImpl> &m, const detail::QuerySpec &spec, bool one,
                           const char *opName) {
    Value filter = prepareFilter(*m, spec.filter);
    runQueryHooks(*m, opName, filter, nullptr);
    mongocxx::options::find opts;
    applyFindOptions(opts, spec, m->schema, one);

    std::vector<Value> out;
    guard([&] {
        Op o = open(*m);
        OpTrace nv_trace(m->collection, opName, Value{{"filter", filter}, {"sort", spec.sort}, {"limit", spec.limit}});
        retryRead([&] {
            out.clear();
            auto cursor = NV(o, find, toBson(filter), opts);
            for (auto &&doc : cursor) out.push_back(fromBson(doc));
        });
    });
    if (!spec.populate.empty() && !out.empty()) {
        std::vector<Value *> ptrs;
        for (auto &v : out) ptrs.push_back(&v);
        for (const auto &p : spec.populate) populateValues(m, ptrs, p);
    }
    if (hasPostHooks(*m, opName)) {
        Value result;
        if (one) {
            result = out.empty() ? Value() : out[0];
        } else {
            result = Value::array();
            for (auto &v : out) result.push_back(std::move(v));
        }
        runPostHooks(*m, opName, filter, nullptr, result);
        out.clear();
        if (one) {
            if (!result.isNull()) out.push_back(std::move(result));
        } else {
            for (auto &v : result.elements()) out.push_back(std::move(v));
        }
    }
    return out;
}

Value idFilter(const Value &id) {
    Value f = Value::object();
    f["_id"] = id;
    return f;
}

Value updateResultValue(const UpdateResult &r) {
    return Value{{"matchedCount", r.matchedCount}, {"modifiedCount", r.modifiedCount}, {"upsertedId", r.upsertedId}};
}

UpdateResult runUpdate(const std::shared_ptr<ModelImpl> &m, const Value &filterIn, const Value &updateIn,
                       UpdateOptions options, bool many) {
    const char *opName = many ? "updateMany" : "updateOne";
    Value filter = prepareFilter(*m, filterIn);
    Value update = updateIn;
    runQueryHooks(*m, opName, filter, &update);
    update = castUpdate(*m, update, options.upsert, options.runValidators, filter);
    UpdateResult res;
    if (update.empty()) return res;
    mongocxx::options::update opts;
    if (options.upsert) opts.upsert(true);
    guard([&] {
        Op o = open(*m);
        OpTrace nv_trace(m->collection, opName,
                         Value{{"filter", filter}, {"update", update}, {"upsert", options.upsert}});
        auto r = many ? NV(o, update_many, toBson(filter), toBson(update), opts)
                      : NV(o, update_one, toBson(filter), toBson(update), opts);
        if (!r) return;
        res.matchedCount = r->matched_count();
        res.modifiedCount = r->modified_count();
        if (auto id = r->upserted_id()) res.upsertedId = fromBson(id->get_value());
    });
    if (hasPostHooks(*m, opName)) {
        Value result = updateResultValue(res);
        runPostHooks(*m, opName, filter, &update, result);
    }
    return res;
}

int64_t runDelete(const std::shared_ptr<ModelImpl> &m, const Value &filterIn, bool many) {
    const char *opName = many ? "deleteMany" : "deleteOne";
    Value filter = prepareFilter(*m, filterIn);
    runQueryHooks(*m, opName, filter, nullptr);
    int64_t n = guard([&]() -> int64_t {
        Op o = open(*m);
        OpTrace nv_trace(m->collection, opName, Value{{"filter", filter}});
        auto r = many ? NV(o, delete_many, toBson(filter)) : NV(o, delete_one, toBson(filter));
        return r ? r->deleted_count() : 0;
    });
    if (hasPostHooks(*m, opName)) {
        Value result{{"deletedCount", n}};
        runPostHooks(*m, opName, filter, nullptr, result);
    }
    return n;
}

int64_t runCount(const std::shared_ptr<ModelImpl> &m, const detail::QuerySpec &spec) {
    Value filter = prepareFilter(*m, spec.filter);
    runQueryHooks(*m, "countDocuments", filter, nullptr);
    mongocxx::options::count opts;
    if (spec.maxTimeMS > 0) opts.max_time(std::chrono::milliseconds(spec.maxTimeMS));
    if (spec.hint.isString()) opts.hint(mongocxx::hint(spec.hint.asString()));
    if (spec.hint.isObject()) opts.hint(mongocxx::hint(toBson(spec.hint)));
    if (spec.collation.isObject()) opts.collation(toBson(spec.collation));
    int64_t n = guard([&]() -> int64_t {
        Op o = open(*m);
        OpTrace nv_trace(m->collection, "countDocuments", Value{{"filter", filter}});
        return NV(o, count_documents, toBson(filter), opts);
    });
    if (hasPostHooks(*m, "countDocuments")) {
        Value result = n;
        runPostHooks(*m, "countDocuments", filter, nullptr, result);
        n = result.asInt(n);
    }
    return n;
}

std::vector<Value> runDistinct(const std::shared_ptr<ModelImpl> &m, const std::string &field, const Value &filterIn) {
    Value filter = prepareFilter(*m, filterIn);
    std::string path = m->schema.resolveAlias(field);
    std::vector<Value> out;
    guard([&] {
        Op o = open(*m);
        OpTrace nv_trace(m->collection, "distinct", Value{{"field", path}, {"filter", filter}});
        auto cursor = NV(o, distinct, path, toBson(filter));
        for (auto &&doc : cursor) {
            Value row = fromBson(doc);
            for (const auto &e : row["values"].elements()) out.push_back(e);
        }
    });
    return out;
}

// A replacement document: cast + strict + timestamps + discriminator key (no _id change)
Value castReplacement(ModelImpl &m, const Value &replacement, bool runValidators) {
    if (!replacement.isObject()) throw Error("nevla: replacement must be an object");
    for (const auto &kv : replacement.items())
        if (!kv.first.empty() && kv.first[0] == '$')
            throw Error("nevla: a replacement document cannot contain update operators (" + kv.first + ")");
    Value doc = detail::castObject(m.schema, replacement);
    if (runValidators) {
        std::map<std::string, std::string> errors, kinds;
        detail::validateObject(m.schema, doc, "", errors, &kinds);
        if (!errors.empty()) throwValidation(m.name, errors, kinds);
    }
    const SchemaOptions &opt = m.schema.options;
    if (opt.timestamps) {
        Date now = Date::now();
        if (!doc.contains(opt.updatedAt)) doc[opt.updatedAt] = now;
        if (!doc.contains(opt.createdAt)) doc[opt.createdAt] = now;
    }
    if (!m.discKey.empty()) doc[m.discKey] = m.discValue;
    return doc;
}

// Data for a fresh insert (timestamps + version key)
void prepareInsert(ModelImpl &m, Value &data) {
    const SchemaOptions &opt = m.schema.options;
    if (opt.timestamps) {
        Date now = Date::now();
        if (!data.contains(opt.createdAt) || data[opt.createdAt].isNull()) data[opt.createdAt] = now;
        if (!data.contains(opt.updatedAt) || data[opt.updatedAt].isNull()) data[opt.updatedAt] = now;
    }
    if (!opt.versionKey.empty() && !data.contains(opt.versionKey)) data[opt.versionKey] = 0;
}

}  // namespace

/* ======================= cursor / change stream internals ======================= */
struct CursorImpl {
    std::shared_ptr<ModelImpl> m;
    Op op;
    std::vector<PopulateOptions> populate;
    std::unique_ptr<mongocxx::cursor> cursor;
    std::optional<mongocxx::cursor::iterator> it;
    bool done = false;
};

struct ChangeStreamImpl {
    Op op;
    std::unique_ptr<mongocxx::change_stream> stream;
    std::optional<mongocxx::change_stream::iterator> it;
};

/* ======================= connection ======================= */
void connect(const std::string &uri, const std::string &dbName) {
    mongocxx::instance::current();
    guard([&] {
        mongocxx::uri u{uri};
        auto c = std::make_shared<Conn>();
        c->pool = std::make_unique<mongocxx::pool>(u);
        c->db = !dbName.empty() ? dbName : !u.database().empty() ? std::string(u.database()) : "test";
        {
            auto entry = c->pool->acquire();
            (*entry)[c->db].run_command(make_document(kvp("ping", 1)));  // server down -> Error right here
        }
        c->gen = ++gGeneration;
        std::lock_guard<std::mutex> lk(gConnMu);
        gConn = std::move(c);
    });
}

void disconnect() {
    std::shared_ptr<Conn> old;
    {
        std::lock_guard<std::mutex> lk(gConnMu);
        old = std::move(gConn);
    }
    // `old` dies here unless an operation still uses it (then it dies when that operation ends)
}

bool connected() {
    std::lock_guard<std::mutex> lk(gConnMu);
    return gConn != nullptr;
}

std::string databaseName() { return currentConn()->db; }

void ping() {
    auto c = currentConn();
    guard([&] {
        auto entry = c->pool->acquire();
        (*entry)[c->db].run_command(make_document(kvp("ping", 1)));
    });
}

void transaction(const std::function<void()> &fn) {
    if (tTx.client) {  // already inside a transaction: join it
        fn();
        return;
    }
    auto c = currentConn();
    guard([&] {
        auto entry = c->pool->acquire();
        auto session = entry->start_session();
        tTx = {c, &*entry, &session};
        struct Reset {
            ~Reset() { tTx = {}; }
        } reset;
        session.with_transaction([&](mongocxx::client_session *) { fn(); });
    });
}

void setDebug(bool on) {
    std::lock_guard<std::mutex> lk(gHookMu);
    gDebugFn = nullptr;
    gDebug = on;
}

void setDebug(std::function<void(const std::string &, const std::string &, const Value &)> fn) {
    std::lock_guard<std::mutex> lk(gHookMu);
    gDebug = static_cast<bool>(fn);
    gDebugFn = std::move(fn);
}

void setSlowQueryThreshold(int64_t ms, std::function<void(const SlowQuery &)> fn) {
    std::lock_guard<std::mutex> lk(gHookMu);
    gSlowFn = std::move(fn);
    gSlowMs = ms < 0 ? 0 : ms;
}

void setStrictQuery(bool on) { gStrictQuery = on; }

void onIndexError(std::function<void(const std::string &, const std::string &)> fn) {
    std::lock_guard<std::mutex> lk(gHookMu);
    gIndexErrorFn = std::move(fn);
}

Value detail::parseSort(const Value &spec) {
    if (spec.isObject()) return spec;
    Value out = Value::object();
    std::istringstream in(spec.str());
    std::string tok;
    while (in >> tok) {
        if (tok[0] == '-')
            out[tok.substr(1)] = -1;
        else
            out[tok[0] == '+' ? tok.substr(1) : tok] = 1;
    }
    return out;
}

/* ======================= Document ======================= */
namespace {
Value applyFieldGetter(const Field *f, Value v) {
    if (!f) return v;
    if ((f->type == FieldType::Array || f->type == FieldType::Map) && f->of_ && f->of_->getter_) {
        if (v.isArray())
            for (auto &e : v.elements()) e = f->of_->getter_(e);
        if (v.isObject())
            for (auto &m : v.items()) m.second = f->of_->getter_(m.second);
    }
    if (f->getter_) v = f->getter_(v);
    return v;
}
}  // namespace

Value Document::get(std::string_view path) const {
    if (!m_) {
        const Value *p = data_.path(path);
        return p ? *p : Value();
    }
    const Schema &s = m_->schema;
    for (const auto &v : s.virtuals_)
        if (v.first == path) return v.second(*this);
    std::string real = s.resolveAlias(path);
    const Value *p = data_.path(real);
    if (!p) return Value();
    return applyFieldGetter(s.path(real), *p);
}

const Value &Document::operator[](std::string_view path) const {
    static const Value kNull;
    if (m_) {
        for (const auto &v : m_->schema.virtuals_)
            if (v.first == path) return virtualCache_[v.first] = v.second(*this);
        std::string real = m_->schema.resolveAlias(path);
        const Value *p = data_.path(real);
        return p ? *p : kNull;
    }
    const Value *p = data_.path(path);
    return p ? *p : kNull;
}

bool Document::has(std::string_view path) const {
    const Value *p = data_.path(m_ ? m_->schema.resolveAlias(path) : std::string(path));
    return p && !p->isNull();
}

std::string Document::id() const { return data_["_id"].str(); }
Value Document::_id() const { return data_["_id"]; }
std::string Document::modelName() const { return m_ ? m_->name : ""; }

Document &Document::set(std::string_view path, const Value &value) {
    std::string p(path);
    if (!m_) {
        data_.pathRef(p) = value;
        return *this;
    }
    const Schema &s = m_->schema;
    p = s.resolveAlias(p);
    bool mixed = false;
    const Field *f = s.path(p, &mixed);
    Value casted = value;
    if (p == "_id" && !s.hasOwnId()) {
        if (value.isString() && ObjectId::isValid(value.asString())) casted = ObjectId(value.asString());
    } else if (!f) {
        if (!keepUnknown(*m_, p)) return *this;  // strict: not in the schema -> ignored
    } else if (!mixed) {
        if (f->immutable_ && !isNew_) return *this;
        try {
            casted = detail::castValue(*f, value, p);
        } catch (const CastError &e) {
            castErrors_[p] = e.what();  // reported by validate() / save() as a ValidationError
            return *this;
        }
    }
    castErrors_.erase(p);
    populated_.erase(p);
    data_.pathRef(p) = std::move(casted);
    modified_.insert(p);
    return *this;
}

Document &Document::set(const Value &object) {
    for (const auto &m : object.items()) set(m.first, m.second);
    return *this;
}

Document &Document::unset(std::string_view path) {
    std::string p = m_ ? m_->schema.resolveAlias(path) : std::string(path);
    data_.erasePath(p);
    castErrors_.erase(p);
    modified_.insert(p);
    return *this;
}

Document &Document::push(std::string_view arrayPath, const Value &item) {
    std::string p = m_ ? m_->schema.resolveAlias(arrayPath) : std::string(arrayPath);
    Value casted = item;
    if (m_) {
        bool mixed = false;
        const Field *f = m_->schema.path(p, &mixed);
        if (f && !mixed && f->type == FieldType::Array) {
            try {
                casted = castElement(*f, item, p);
            } catch (const CastError &e) {
                castErrors_[p] = e.what();
                return *this;
            }
        }
    }
    Value &arr = data_.pathRef(p);
    if (!arr.isArray()) arr = Value::array();
    arr.push_back(std::move(casted));
    modified_.insert(p);
    return *this;
}

Value &Document::mut(std::string_view path) {
    std::string p = m_ ? m_->schema.resolveAlias(path) : std::string(path);
    modified_.insert(p);
    return data_.pathRef(p);
}

void Document::markModified(std::string_view path) {
    modified_.insert(m_ ? m_->schema.resolveAlias(path) : std::string(path));
}

Document &Document::overwrite(const Value &object) {
    if (!m_) {
        Value id = data_["_id"];
        data_ = object;
        if (!id.isNull()) data_["_id"] = id;
        return *this;
    }
    const SchemaOptions &opt = m_->schema.options;
    std::map<std::string, std::string> errs;
    Value fresh = detail::castObject(m_->schema, object, "", &errs);
    for (const auto &kv : data_.items())
        if (kv.first != "_id" && kv.first != opt.versionKey) modified_.insert(kv.first);
    for (const auto &keep : {std::string("_id"), opt.versionKey, opt.createdAt, std::string(m_->discKey)}) {
        if (keep.empty()) continue;
        if (const Value *v = data_.find(keep); v && !fresh.contains(keep)) fresh[keep] = *v;
    }
    for (const auto &kv : fresh.items()) modified_.insert(kv.first);
    modified_.erase("_id");
    data_ = std::move(fresh);
    castErrors_ = std::move(errs);
    populated_.clear();
    return *this;
}

bool Document::isModified(std::string_view path) const {
    if (path.empty()) return isNew_ || !modified_.empty();
    std::string p = m_ ? m_->schema.resolveAlias(path) : std::string(path);
    if (isNew_ && data_.path(p)) return true;  // in a new document everything that is set counts as modified
    for (const auto &mp : modified_)
        if (mp == p || mp.rfind(p + ".", 0) == 0 || p.rfind(mp + ".", 0) == 0) return true;
    return false;
}

std::vector<std::string> Document::modifiedPaths() const { return {modified_.begin(), modified_.end()}; }

void Document::validate() {
    if (!m_) return;
    runDocHooks(m_->schema.preDoc_, "validate", *this);
    std::map<std::string, std::string> errors, kinds;
    for (const auto &ce : castErrors_) {
        errors[ce.first] = ce.second;
        kinds[ce.first] = "cast";
    }
    Value data = data_;
    for (const auto &p : populated_) depopulatePath(m_->schema, data, p);
    std::map<std::string, std::string> vErrors, vKinds;
    detail::validateObject(m_->schema, data, "", vErrors, &vKinds);
    for (auto &e : vErrors)
        if (!errors.count(e.first)) {  // a cast error is the more useful message for that path
            errors[e.first] = e.second;
            kinds[e.first] = vKinds.count(e.first) ? vKinds[e.first] : "user defined";
        }
    if (!errors.empty()) throwValidation(m_->name, errors, kinds);
    runDocHooks(m_->schema.postDoc_, "validate", *this);
}

Document &Document::increment() {
    bumpVersion_ = true;
    return *this;
}

Document &Document::save() {
    if (!m_) throw Error("nevla: this Document has no model (create it with Model({...}))");
    validate();  // like Mongoose: validation is the first pre-save step
    runDocHooks(m_->schema.preDoc_, "save", *this);
    const SchemaOptions &opt = m_->schema.options;
    const std::string &vk = opt.versionKey;

    if (isNew_) {
        prepareInsert(*m_, data_);
        Value stored = data_;
        for (const auto &p : populated_) depopulatePath(m_->schema, stored, p);
        guard([&] {
            Op o = open(*m_);
            OpTrace nv_trace(m_->collection, "insertOne", stored);
            NV(o, insert_one, toBson(stored));
        });
        isNew_ = false;
        modified_.clear();
        bumpVersion_ = false;
    } else if (!modified_.empty() || bumpVersion_) {
        Value set = Value::object(), unset = Value::object();
        std::string previous;
        bool arrayChanged = false;
        for (const auto &p : modified_) {  // sorted: "a" comes before "a.b" -> the parent is enough
            if (p == "_id" || (!vk.empty() && p == vk)) continue;
            if (!previous.empty() && p.rfind(previous + ".", 0) == 0) continue;
            previous = p;
            const Value *v = data_.path(p);
            if (v) {
                Value copy = *v;
                for (const auto &pp : populated_)
                    if (pp == p || pp.rfind(p + ".", 0) == 0) {
                        Value wrap = Value::object();
                        wrap["x"] = copy;
                        depopulatePath(m_->schema, wrap, "x" + pp.substr(p.size()));
                        copy = wrap["x"];
                    }
                set[p] = std::move(copy);
                const Field *f = m_->schema.path(p);
                arrayChanged |= f && f->type == FieldType::Array;
            } else {
                unset[p] = "";
            }
        }
        if (opt.timestamps && (!set.empty() || !unset.empty())) {
            Date now = Date::now();
            data_[opt.updatedAt] = now;
            set[opt.updatedAt] = now;
        }
        Value update = Value::object();
        if (!set.empty()) update["$set"] = std::move(set);
        if (!unset.empty()) update["$unset"] = std::move(unset);
        Value filter = idFilter(data_["_id"]);
        bool versioned = !vk.empty();
        bool bump = versioned && (bumpVersion_ || arrayChanged || opt.optimisticConcurrency) && !update.empty();
        if (versioned && bumpVersion_ && update.empty()) bump = true;
        if (versioned && opt.optimisticConcurrency) filter[vk] = data_[vk];
        if (bump) update["$inc"][vk] = 1;
        if (!update.empty()) {
            int64_t matched = guard([&]() -> int64_t {
                Op o = open(*m_);
                OpTrace nv_trace(m_->collection, "updateOne", Value{{"filter", filter}, {"update", update}});
                auto r = NV(o, update_one, toBson(filter), toBson(update));
                return r ? r->matched_count() : 0;
            });
            if (matched == 0) {
                bool exists = guard([&]() -> bool {
                    Op o = open(*m_);
                    return NV(o, count_documents, toBson(idFilter(data_["_id"]))) > 0;
                });
                if (exists && opt.optimisticConcurrency)
                    throw VersionError("No matching document found for id \"" + id() + "\" version " + data_[vk].str() +
                                       " (it was modified by someone else) on model \"" + m_->name + "\"");
                throw NotFoundError("nevla: save(): " + m_->name + " document with _id " + id() +
                                    " was not found in the database");
            }
            if (bump) data_[vk] = data_[vk].asInt(0) + 1;
        }
        modified_.clear();
        bumpVersion_ = false;
    }
    runDocHooks(m_->schema.postDoc_, "save", *this);
    return *this;
}

void Document::deleteOne() {
    if (!m_) throw Error("nevla: this Document has no model");
    runDocHooks(m_->schema.preDoc_, "deleteOne", *this);
    guard([&] {
        Op o = open(*m_);
        OpTrace nv_trace(m_->collection, "deleteOne", idFilter(data_["_id"]));
        NV(o, delete_one, toBson(idFilter(data_["_id"])));
    });
    runDocHooks(m_->schema.postDoc_, "deleteOne", *this);
}

UpdateResult Document::updateOne(const Value &update, UpdateOptions options) {
    if (!m_) throw Error("nevla: this Document has no model");
    return runUpdate(m_, idFilter(data_["_id"]), update, options, false);
}

Document &Document::populate(const std::string &path) {
    PopulateOptions o;
    o.path = path;
    return populate(o);
}

Document &Document::populate(const PopulateOptions &options) {
    if (!m_) throw Error("nevla: this Document has no model");
    std::vector<Value *> docs{&data_};
    populateValues(m_, docs, options);
    populated_.insert(m_->schema.resolveAlias(options.path));
    return *this;
}

Document &Document::depopulate(const std::string &path) {
    if (!m_) return *this;
    if (path.empty()) {
        for (const auto &p : populated_) depopulatePath(m_->schema, data_, p);
        populated_.clear();
        return *this;
    }
    std::string p = m_->schema.resolveAlias(path);
    depopulatePath(m_->schema, data_, p);
    populated_.erase(p);
    return *this;
}

bool Document::populated(const std::string &path) const {
    return populated_.count(m_ ? m_->schema.resolveAlias(path) : path) > 0;
}

Value Document::call(const std::string &method, const Value &args) {
    if (m_) {
        auto it = m_->schema.methods_.find(method);
        if (it != m_->schema.methods_.end()) return it->second(*this, args);
    }
    throw Error("nevla: method \"" + method + "\" is not defined on the schema");
}

bool Document::equals(const Document &other) const {
    return m_ && other.m_ && m_->collection == other.m_->collection && data_["_id"] == other.data_["_id"];
}

Value Document::toObject() const { return data_; }

Value Document::toJSON() const {
    Value out = data_;
    if (m_) {
        const Schema &s = m_->schema;
        if (s.options.getters) detail::applyGetters(s, out);
        if (s.options.virtuals) {
            out["id"] = id();
            for (const auto &v : s.virtuals_) out[v.first] = v.second(*this);
            for (const auto &f : s.fields())
                if (!f.second.alias_.empty() && data_.contains(f.first)) out[f.second.alias_] = out[f.first];
        }
        if (s.jsonTransform_) s.jsonTransform_(*this, out);
    }
    return out;
}

/* ======================= Query ======================= */
namespace {
std::vector<Document> toDocuments(const std::shared_ptr<ModelImpl> &m, std::vector<Value> rows,
                                  const std::vector<PopulateOptions> &populate) {
    std::vector<Document> out;
    out.reserve(rows.size());
    for (auto &v : rows) {
        auto impl = pickModel(m, v);
        Document d = ModelImpl::make(impl, std::move(v), false);
        for (const auto &p : populate) ModelImpl::markPopulated(d, m->schema.resolveAlias(p.path));
        out.push_back(std::move(d));
    }
    return out;
}
}  // namespace

std::vector<Value> Query::lean() { return runFind(m_, spec_, false, "find"); }

std::vector<Document> Query::exec() { return toDocuments(m_, runFind(m_, spec_, false, "find"), spec_.populate); }

int64_t Query::count() { return runCount(m_, spec_); }

std::vector<Value> Query::distinct(const std::string &field) { return runDistinct(m_, field, spec_.filter); }

Cursor Query::cursor() {
    auto impl = std::make_shared<CursorImpl>();
    impl->m = m_;
    impl->populate = spec_.populate;
    Value filter = prepareFilter(*m_, spec_.filter);
    runQueryHooks(*m_, "find", filter, nullptr);
    mongocxx::options::find opts;
    applyFindOptions(opts, spec_, m_->schema, false);
    guard([&] {
        impl->op = open(*m_);
        OpTrace nv_trace(m_->collection, "find (cursor)", Value{{"filter", filter}});
        impl->cursor = std::make_unique<mongocxx::cursor>(NV(impl->op, find, toBson(filter), opts));
    });
    Cursor c;
    c.impl_ = std::move(impl);
    return c;
}

std::optional<Value> Cursor::nextLean() {
    if (!impl_ || impl_->done) return std::nullopt;
    auto &I = *impl_;
    std::optional<Value> v = guard([&]() -> std::optional<Value> {
        if (!I.it)
            I.it.emplace(I.cursor->begin());
        else
            ++(*I.it);
        if (*I.it == I.cursor->end()) return std::nullopt;
        return fromBson(**I.it);
    });
    if (!v) {
        I.done = true;
        return std::nullopt;
    }
    if (!I.populate.empty()) {
        std::vector<Value *> ptrs{&*v};
        for (const auto &p : I.populate) populateValues(I.m, ptrs, p);
    }
    return v;
}

std::optional<Document> Cursor::next() {
    auto v = nextLean();
    if (!v) return std::nullopt;
    auto docs = toDocuments(impl_->m, {std::move(*v)}, impl_->populate);
    return std::move(docs[0]);
}

void Query::forEach(const std::function<void(Document &)> &fn) {
    Cursor c = cursor();
    while (auto d = c.next()) fn(*d);
}

Page Query::paginate(int64_t page, int64_t perPage) {
    if (perPage <= 0) throw Error("nevla: paginate(): perPage must be > 0");
    if (page < 1) page = 1;
    Page out;
    out.page = page;
    out.perPage = perPage;
    out.total = runCount(m_, spec_);
    out.pages = (out.total + perPage - 1) / perPage;
    detail::QuerySpec s = spec_;
    s.skip = (page - 1) * perPage;
    s.limit = perPage;
    out.docs = toDocuments(m_, runFind(m_, s, false, "find"), s.populate);
    out.hasNext = page < out.pages;
    out.hasPrev = page > 1;
    return out;
}

std::optional<Value> QueryOne::lean() {
    auto rows = runFind(m_, spec_, true, "findOne");
    if (rows.empty()) return std::nullopt;
    return std::move(rows[0]);
}

std::optional<Document> QueryOne::exec() {
    auto rows = runFind(m_, spec_, true, "findOne");
    if (rows.empty()) return std::nullopt;
    auto docs = toDocuments(m_, std::move(rows), spec_.populate);
    return std::move(docs[0]);
}

Document QueryOne::orFail() {
    auto d = exec();
    if (!d)
        throw NotFoundError("No document found for query " + spec_.filter.toJson() + " on model \"" + m_->name + "\"");
    return std::move(*d);
}

/* ======================= ChangeStream ======================= */
std::optional<Value> ChangeStream::next() {
    if (!impl_) return std::nullopt;
    auto &I = *impl_;
    return guard([&]() -> std::optional<Value> {
        if (!I.it || *I.it == I.stream->end())
            I.it.emplace(I.stream->begin());  // (re)start: waits up to maxAwaitMS
        else
            ++(*I.it);
        if (*I.it == I.stream->end()) return std::nullopt;
        return fromBson(**I.it);
    });
}

/* ======================= Model ======================= */
const std::string &Model::name() const { return m_->name; }
const std::string &Model::collectionName() const { return m_->collection; }
const Schema &Model::schema() const { return m_->schema; }

Document Model::operator()(const Value &object) const {
    const Schema &s = m_->schema;
    std::map<std::string, std::string> castErrors;
    Value data = detail::castObject(s, object, "", &castErrors);
    detail::applyDefaults(s, data);
    if (!data.contains("_id") && !s.hasOwnId()) data["_id"] = ObjectId::generate();
    if (!m_->discKey.empty()) data[m_->discKey] = m_->discValue;
    // _id first
    Value ordered = Value::object();
    if (const Value *id = data.find("_id")) ordered["_id"] = *id;
    for (auto &kv : data.items())
        if (kv.first != "_id") ordered[kv.first] = std::move(kv.second);
    Document d = ModelImpl::make(m_, std::move(ordered), true);
    d.castErrors_ = std::move(castErrors);
    return d;
}

Document Model::hydrate(const Value &object) const { return ModelImpl::make(pickModel(m_, object), object, false); }

Document Model::create(const Value &object) const {
    Document d = (*this)(object);
    d.save();
    return d;
}

std::vector<Document> Model::insertMany(const std::vector<Value> &objects, InsertManyOptions options) const {
    std::vector<Document> docs;
    if (objects.empty()) return docs;
    std::vector<bsoncxx::document::value> raw;
    for (const auto &o : objects) {
        Document d = (*this)(o);
        d.validate();
        prepareInsert(*m_, d.data_);
        raw.push_back(toBson(d.data_));
        docs.push_back(std::move(d));
    }
    mongocxx::options::insert opts;
    opts.ordered(options.ordered);
    guard([&] {
        Op o = open(*m_);
        OpTrace nv_trace(m_->collection, "insertMany", Value{{"count", static_cast<int64_t>(raw.size())}});
        NV(o, insert_many, raw, opts);
    });
    for (auto &d : docs) {
        d.isNew_ = false;
        d.modified_.clear();
    }
    return docs;
}

Query Model::find(const Value &filter) const { return Query(m_, filter); }
QueryOne Model::findOne(const Value &filter) const { return QueryOne(m_, filter); }
QueryOne Model::findById(const Value &id) const { return QueryOne(m_, idFilter(id)); }
Query Model::where(const std::string &path) const {
    Query q(m_, Value::object());
    q.where(path);
    return q;
}

UpdateResult Model::updateOne(const Value &filter, const Value &update, UpdateOptions options) const {
    return runUpdate(m_, filter, update, options, false);
}
UpdateResult Model::updateMany(const Value &filter, const Value &update, UpdateOptions options) const {
    return runUpdate(m_, filter, update, options, true);
}

UpdateResult Model::replaceOne(const Value &filterIn, const Value &replacement, UpdateOptions options) const {
    Value filter = prepareFilter(*m_, filterIn);
    Value repl = replacement;
    runQueryHooks(*m_, "replaceOne", filter, &repl);
    Value doc = castReplacement(*m_, repl, options.runValidators);
    mongocxx::options::replace opts;
    if (options.upsert) opts.upsert(true);
    UpdateResult res;
    guard([&] {
        Op o = open(*m_);
        OpTrace nv_trace(m_->collection, "replaceOne", Value{{"filter", filter}, {"replacement", doc}});
        auto r = NV(o, replace_one, toBson(filter), toBson(doc), opts);
        if (!r) return;
        res.matchedCount = r->matched_count();
        res.modifiedCount = r->modified_count();
        if (auto id = r->upserted_id()) res.upsertedId = fromBson(id->get_value());
    });
    if (hasPostHooks(*m_, "replaceOne")) {
        Value result = updateResultValue(res);
        runPostHooks(*m_, "replaceOne", filter, &doc, result);
    }
    return res;
}

std::optional<Document> Model::findOneAndUpdate(const Value &filterIn, const Value &updateIn,
                                                FindUpdateOptions options) const {
    Value filter = prepareFilter(*m_, filterIn);
    Value update = updateIn;
    runQueryHooks(*m_, "findOneAndUpdate", filter, &update);
    update = castUpdate(*m_, update, options.upsert, options.runValidators, filter);
    if (update.empty()) {  // nothing to change: behave like findOne
        auto d = QueryOne(m_, filterIn).select(options.select).exec();
        return d;
    }
    mongocxx::options::find_one_and_update opts;
    if (options.returnNew) opts.return_document(mongocxx::options::return_document::k_after);
    if (options.upsert) opts.upsert(true);
    Value proj = buildProjection(m_->schema, options.select);
    if (!proj.empty()) opts.projection(toBson(proj));
    auto found = guard([&]() -> std::optional<Value> {
        Op o = open(*m_);
        OpTrace nv_trace(m_->collection, "findOneAndUpdate", Value{{"filter", filter}, {"update", update}});
        auto r = NV(o, find_one_and_update, toBson(filter), toBson(update), opts);
        if (!r) return std::nullopt;
        return fromBson(r->view());
    });
    if (hasPostHooks(*m_, "findOneAndUpdate")) {
        Value result = found ? *found : Value();
        runPostHooks(*m_, "findOneAndUpdate", filter, &update, result);
        found = result.isNull() ? std::nullopt : std::optional<Value>(result);
    }
    if (!found) return std::nullopt;
    return ModelImpl::make(pickModel(m_, *found), std::move(*found), false);
}

std::optional<Document> Model::findByIdAndUpdate(const Value &id, const Value &update,
                                                 FindUpdateOptions options) const {
    return findOneAndUpdate(idFilter(id), update, options);
}

std::optional<Document> Model::findOneAndReplace(const Value &filterIn, const Value &replacement,
                                                 FindUpdateOptions options) const {
    Value filter = prepareFilter(*m_, filterIn);
    Value repl = replacement;
    runQueryHooks(*m_, "findOneAndReplace", filter, &repl);
    Value doc = castReplacement(*m_, repl, options.runValidators);
    mongocxx::options::find_one_and_replace opts;
    if (options.returnNew) opts.return_document(mongocxx::options::return_document::k_after);
    if (options.upsert) opts.upsert(true);
    Value proj = buildProjection(m_->schema, options.select);
    if (!proj.empty()) opts.projection(toBson(proj));
    auto found = guard([&]() -> std::optional<Value> {
        Op o = open(*m_);
        OpTrace nv_trace(m_->collection, "findOneAndReplace", Value{{"filter", filter}, {"replacement", doc}});
        auto r = NV(o, find_one_and_replace, toBson(filter), toBson(doc), opts);
        if (!r) return std::nullopt;
        return fromBson(r->view());
    });
    if (hasPostHooks(*m_, "findOneAndReplace")) {
        Value result = found ? *found : Value();
        runPostHooks(*m_, "findOneAndReplace", filter, &doc, result);
        found = result.isNull() ? std::nullopt : std::optional<Value>(result);
    }
    if (!found) return std::nullopt;
    return ModelImpl::make(pickModel(m_, *found), std::move(*found), false);
}

std::optional<Document> Model::findOneAndDelete(const Value &filterIn) const {
    Value filter = prepareFilter(*m_, filterIn);
    runQueryHooks(*m_, "findOneAndDelete", filter, nullptr);
    auto found = guard([&]() -> std::optional<Value> {
        Op o = open(*m_);
        OpTrace nv_trace(m_->collection, "findOneAndDelete", Value{{"filter", filter}});
        auto r = NV(o, find_one_and_delete, toBson(filter));
        if (!r) return std::nullopt;
        return fromBson(r->view());
    });
    if (hasPostHooks(*m_, "findOneAndDelete")) {
        Value result = found ? *found : Value();
        runPostHooks(*m_, "findOneAndDelete", filter, nullptr, result);
        found = result.isNull() ? std::nullopt : std::optional<Value>(result);
    }
    if (!found) return std::nullopt;
    return ModelImpl::make(pickModel(m_, *found), std::move(*found), false);
}

std::optional<Document> Model::findByIdAndDelete(const Value &id) const { return findOneAndDelete(idFilter(id)); }

int64_t Model::deleteOne(const Value &filter) const { return runDelete(m_, filter, false); }
int64_t Model::deleteMany(const Value &filter) const { return runDelete(m_, filter, true); }

BulkWriteResult Model::bulkWrite(const std::vector<Value> &operations, bool ordered) const {
    BulkWriteResult res;
    if (operations.empty()) return res;
    struct Prepared {
        std::string kind;
        bsoncxx::document::value a, b;
        bool upsert;
    };
    std::vector<Prepared> ops;
    for (const auto &opv : operations) {
        if (!opv.isObject() || opv.size() != 1)
            throw Error("nevla: bulkWrite(): every operation must look like {{\"insertOne\", {...}}}");
        const std::string &kind = opv.items()[0].first;
        const Value &arg = opv.items()[0].second;
        bool upsert = arg["upsert"].asBool();
        auto filterOf = [&] { return prepareFilter(*m_, arg["filter"]); };
        if (kind == "insertOne") {
            Document d = (*this)(arg["document"]);
            d.validate();
            prepareInsert(*m_, d.data_);
            ops.push_back({kind, toBson(d.data_), toBson(Value::object()), false});
        } else if (kind == "updateOne" || kind == "updateMany") {
            Value filter = filterOf();
            Value update = castUpdate(*m_, arg["update"], upsert, arg["runValidators"].asBool(), filter);
            if (update.empty()) continue;
            ops.push_back({kind, toBson(filter), toBson(update), upsert});
        } else if (kind == "replaceOne") {
            Value filter = filterOf();
            ops.push_back({kind, toBson(filter), toBson(castReplacement(*m_, arg["replacement"], false)), upsert});
        } else if (kind == "deleteOne" || kind == "deleteMany") {
            ops.push_back({kind, toBson(filterOf()), toBson(Value::object()), false});
        } else {
            throw Error("nevla: bulkWrite(): unknown operation \"" + kind + "\"");
        }
    }
    if (ops.empty()) return res;
    guard([&] {
        Op o = open(*m_);
        OpTrace nv_trace(m_->collection, "bulkWrite", Value{{"operations", static_cast<int64_t>(ops.size())}});
        mongocxx::options::bulk_write bopts;
        bopts.ordered(ordered);
        auto bulk = o.session ? o.coll.create_bulk_write(*o.session, bopts) : o.coll.create_bulk_write(bopts);
        for (const auto &p : ops) {
            if (p.kind == "insertOne") {
                bulk.append(mongocxx::model::insert_one(p.a.view()));
            } else if (p.kind == "updateOne") {
                mongocxx::model::update_one m(p.a.view(), p.b.view());
                m.upsert(p.upsert);
                bulk.append(m);
            } else if (p.kind == "updateMany") {
                mongocxx::model::update_many m(p.a.view(), p.b.view());
                m.upsert(p.upsert);
                bulk.append(m);
            } else if (p.kind == "replaceOne") {
                mongocxx::model::replace_one m(p.a.view(), p.b.view());
                m.upsert(p.upsert);
                bulk.append(m);
            } else if (p.kind == "deleteOne") {
                bulk.append(mongocxx::model::delete_one(p.a.view()));
            } else {
                bulk.append(mongocxx::model::delete_many(p.a.view()));
            }
        }
        auto r = bulk.execute();
        if (!r) return;
        res.insertedCount = r->inserted_count();
        res.matchedCount = r->matched_count();
        res.modifiedCount = r->modified_count();
        res.deletedCount = r->deleted_count();
        res.upsertedCount = r->upserted_count();
    });
    return res;
}

int64_t Model::countDocuments(const Value &filter) const { return Query(m_, filter).count(); }

int64_t Model::estimatedDocumentCount() const {
    return guard([&]() -> int64_t {
        Op o = open(*m_);
        return o.coll.estimated_document_count();
    });
}

bool Model::exists(const Value &filter) const {
    detail::QuerySpec s;
    s.filter = asFilter(filter);
    s.select = "_id";
    return !runFind(m_, s, true, "findOne").empty();
}

std::vector<Value> Model::distinct(const std::string &field, const Value &filter) const {
    return runDistinct(m_, field, filter);
}

namespace {
// Stages after which documents no longer have the schema's shape: later $match stages are not cast
bool reshapes(const std::string &stageName) {
    static const std::set<std::string> keep = {"$match", "$sort", "$limit", "$skip", "$sample", "$unwind", "$lookup"};
    return keep.count(stageName) == 0;
}

// Cast the $match stages that still see schema-shaped documents (Mongoose does not do this)
Value castPipeline(ModelImpl &m, const Value &stages) {
    Value out = Value::array();
    bool reshaped = false;
    for (const auto &st : stages.elements()) {
        if (!st.isObject() || st.size() != 1) {
            out.push_back(st);
            reshaped = true;
            continue;
        }
        const std::string &name = st.items()[0].first;
        if (name == "$match" && !reshaped && st.items()[0].second.isObject()) {
            Value cast = Value::object();
            cast["$match"] = castFilter(m.schema, st.items()[0].second);
            out.push_back(std::move(cast));
        } else {
            out.push_back(st);
        }
        if (reshapes(name)) reshaped = true;
    }
    return out;
}

std::vector<Value> runAggregate(const std::shared_ptr<ModelImpl> &m, Value stages, const AggregateOptions &o) {
    runQueryHooks(*m, "aggregate", stages, nullptr);
    mongocxx::pipeline p;
    if (!m->discKey.empty()) {
        Value match = Value::object();
        match["$match"][m->discKey] = m->discValue;
        p.append_stage(toBson(match));
    }
    for (const auto &stage : stages.elements()) {
        if (!stage.isObject()) throw Error("nevla: every aggregation stage must be an object, got " + stage.toJson());
        p.append_stage(toBson(stage));
    }
    mongocxx::options::aggregate opts;
    if (o.allowDiskUse) opts.allow_disk_use(true);
    if (o.maxTimeMS > 0) opts.max_time(std::chrono::milliseconds(o.maxTimeMS));
    if (o.collation.isObject()) opts.collation(toBson(o.collation));
    if (o.hint.isString()) opts.hint(mongocxx::hint(o.hint.asString()));
    if (o.hint.isObject()) opts.hint(mongocxx::hint(toBson(o.hint)));
    std::vector<Value> out;
    guard([&] {
        Op op = open(*m);
        OpTrace nv_trace(m->collection, "aggregate", Value{{"pipeline", stages}});
        bool writes = false;  // $out / $merge must not run twice
        for (const auto &st : stages.elements()) writes |= st.contains("$out") || st.contains("$merge");
        auto run = [&] {
            out.clear();
            auto cursor = NV(op, aggregate, p, opts);
            for (auto &&doc : cursor) out.push_back(fromBson(doc));
        };
        if (writes)
            run();
        else
            retryRead(run);
    });
    if (hasPostHooks(*m, "aggregate")) {
        Value result = Value::array();
        for (auto &v : out) result.push_back(std::move(v));
        runPostHooks(*m, "aggregate", stages, nullptr, result);
        out.clear();
        for (auto &v : result.elements()) out.push_back(std::move(v));
    }
    return out;
}

std::string fieldRef(const std::string &field) {
    if (field.empty()) throw Error("nevla: accumulator needs a field name");
    return field[0] == '$' ? field : "$" + field;
}

// avg + "reading.temp" -> "avgReadingTemp"
std::string accName(const std::string &op, const std::string &field) {
    std::string out = op;
    bool upper = true;
    for (char c : field) {
        if (c == '$') continue;
        if (c == '.' || c == '_') {
            upper = true;
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    return out;
}

Accumulator makeAcc(const char *op, const std::string &field) {
    if (field.rfind("$$", 0) == 0)  // $$ROOT, $$NOW ...
        return Accumulator{std::string("$") + op, Value(field), op + std::string("Root")};
    if (field.empty() || detail::isFieldPath(field))
        return Accumulator{std::string("$") + op, Value(fieldRef(field)), accName(op, field)};
    return Accumulator{std::string("$") + op, expr(field), ""};  // a formula: name it with .as()
}

// Accumulators -> $group fields
void addAccumulators(Value &g, std::vector<Accumulator> &accumulators, bool reshaped, const Schema &schema,
                     const char *where) {
    for (auto &a : accumulators) {
        if (a.name.empty())
            throw Error(std::string("nevla: ") + where + ": an accumulator with a formula needs a name, e.g. " +
                        a.op.substr(1) + "(\"price * qty\").as(\"revenue\")");
        if (a.name == "_id") throw Error(std::string("nevla: ") + where + ": \"_id\" cannot be an accumulator name");
        if (g.contains(a.name))
            throw Error(std::string("nevla: ") + where + ": two accumulators are both named \"" + a.name +
                        "\" (use .as(\"other\"))");
        Value e = a.expr;
        if (!reshaped && e.isString() && e.asString().size() > 1 && e.asString()[0] == '$' && e.asString()[1] != '$')
            e = "$" + schema.resolveAlias(e.asString().substr(1));
        g[a.name][a.op] = std::move(e);
    }
}

std::vector<std::string> splitWords(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(tok[0] == '$' ? tok.substr(1) : tok);
    return out;
}

std::string lastSegment(const std::string &path) { return path.substr(path.rfind('.') + 1); }

// "15m" -> {15, "minute"}
std::pair<int64_t, std::string> parseInterval(const std::string &every) {
    size_t i = 0;
    while (i < every.size() && std::isdigit(static_cast<unsigned char>(every[i]))) i++;
    int64_t n = i == 0 ? 1 : std::stoll(every.substr(0, i));
    std::string u = every.substr(i);
    while (!u.empty() && u[0] == ' ') u.erase(0, 1);
    static const std::map<std::string, std::string> units = {
        {"ms", "millisecond"}, {"s", "second"},      {"sec", "second"},      {"m", "minute"},
        {"min", "minute"},     {"h", "hour"},        {"d", "day"},           {"w", "week"},
        {"M", "month"},        {"q", "quarter"},     {"y", "year"},          {"millisecond", "millisecond"},
        {"second", "second"},  {"minute", "minute"}, {"hour", "hour"},       {"day", "day"},
        {"week", "week"},      {"month", "month"},   {"quarter", "quarter"}, {"year", "year"}};
    std::string key = u;
    if (key.size() > 2 && key.back() == 's' && units.count(key.substr(0, key.size() - 1)) && key != "ms")
        key.pop_back();  // "minutes" -> "minute"
    auto it = units.find(key);
    if (it == units.end() || n <= 0)
        throw Error("nevla: bucketByTime(): bad interval \"" + every +
                    "\" (use e.g. \"30s\", \"15m\", \"1h\", \"1d\", \"1w\", \"1M\" for month, \"1y\")");
    return {n, it->second};
}
}  // namespace

Accumulator sum(const std::string &field) { return makeAcc("sum", field); }
Accumulator avg(const std::string &field) { return makeAcc("avg", field); }
Accumulator min(const std::string &field) { return makeAcc("min", field); }
Accumulator max(const std::string &field) { return makeAcc("max", field); }
Accumulator first(const std::string &field) { return makeAcc("first", field); }
Accumulator last(const std::string &field) { return makeAcc("last", field); }
Accumulator push(const std::string &field) { return makeAcc("push", field); }
Accumulator addToSet(const std::string &field) { return makeAcc("addToSet", field); }
Accumulator stdDevPop(const std::string &field) { return makeAcc("stdDevPop", field); }
Accumulator count() { return Accumulator{"$sum", Value(1), "count"}; }
Accumulator accumulator(const std::string &op, Value expr, std::string name) {
    return Accumulator{op.empty() || op[0] == '$' ? op : "$" + op, std::move(expr), std::move(name)};
}

/* ======================= Aggregate ======================= */
Aggregate &Aggregate::match(const Value &filterIn) {
    if (!filterIn.isObject() && !filterIn.isString())
        throw Error("nevla: aggregate().match() needs an object or a formula string");
    Value filter = asFilter(filterIn);
    Value st = Value::object();
    st["$match"] = reshaped_ ? filter : castFilter(m_->schema, filter);
    stages_.push_back(std::move(st));
    return *this;
}

Aggregate &Aggregate::group(const std::string &by, std::vector<Accumulator> accumulators) {
    std::vector<std::string> keys;
    std::istringstream in(by);
    std::string tok;
    while (in >> tok) {
        if (tok[0] == '$') tok = tok.substr(1);
        keys.push_back(reshaped_ ? tok : m_->schema.resolveAlias(tok));
    }
    Value g = Value::object();
    if (keys.empty()) {
        g["_id"] = Value();
    } else if (keys.size() == 1) {
        g["_id"] = "$" + keys[0];
    } else {
        Value id = Value::object();
        for (const auto &k : keys) id[k.substr(k.rfind('.') + 1)] = "$" + k;
        g["_id"] = std::move(id);
    }
    addAccumulators(g, accumulators, reshaped_, m_->schema, "group()");
    Value st = Value::object();
    st["$group"] = std::move(g);
    stages_.push_back(std::move(st));
    reshaped_ = true;
    return *this;
}

Aggregate &Aggregate::sort(const Value &spec) {
    Value st = Value::object();
    st["$sort"] = detail::parseSort(spec);
    if (st["$sort"].empty()) throw Error("nevla: aggregate().sort() needs at least one field");
    stages_.push_back(std::move(st));
    return *this;
}

Aggregate &Aggregate::limit(int64_t n) {
    if (n <= 0) throw Error("nevla: aggregate().limit() must be > 0");
    stages_.push_back(Value{{"$limit", n}});
    return *this;
}

Aggregate &Aggregate::skip(int64_t n) {
    if (n < 0) throw Error("nevla: aggregate().skip() must be >= 0");
    stages_.push_back(Value{{"$skip", n}});
    return *this;
}

Aggregate &Aggregate::project(const Value &spec) {
    Value proj = Value::object();
    if (spec.isString()) {
        std::istringstream in(spec.asString());
        std::string tok;
        while (in >> tok) {
            if (tok[0] == '-')
                proj[tok.substr(1)] = 0;
            else
                proj[tok[0] == '+' ? tok.substr(1) : tok] = 1;
        }
    } else if (spec.isObject()) {
        proj = spec;
    } else {
        throw Error("nevla: aggregate().project() needs \"a b -c\" or an object");
    }
    if (proj.empty()) throw Error("nevla: aggregate().project() needs at least one field");
    stages_.push_back(Value{{"$project", proj}});
    reshaped_ = true;
    return *this;
}

Aggregate &Aggregate::addFields(const Value &fields) {
    if (!fields.isObject() || fields.empty()) throw Error("nevla: aggregate().addFields() needs an object");
    stages_.push_back(Value{{"$addFields", fields}});
    reshaped_ = true;
    return *this;
}

Aggregate &Aggregate::unwind(const std::string &path, bool keepEmpty) {
    std::string p = path.empty() || path[0] != '$' ? "$" + path : path;
    if (p.size() < 2) throw Error("nevla: aggregate().unwind() needs a path");
    Value st = Value::object();
    if (keepEmpty)
        st["$unwind"] = Value{{"path", p}, {"preserveNullAndEmptyArrays", true}};
    else
        st["$unwind"] = p;
    stages_.push_back(std::move(st));
    return *this;
}

Aggregate &Aggregate::lookup(const std::string &from, const std::string &localField, const std::string &foreignField,
                             const std::string &as) {
    std::string coll = from;
    {
        std::lock_guard<std::mutex> lk(gModelsMu);
        auto it = gModels.find(from);
        if (it != gModels.end()) coll = it->second->collection;  // a model name -> its collection
    }
    stages_.push_back(Value{
        {"$lookup", Value{{"from", coll}, {"localField", localField}, {"foreignField", foreignField}, {"as", as}}}});
    return *this;
}

Aggregate &Aggregate::count(const std::string &as) {
    if (as.empty() || as[0] == '$' || as.find('.') != std::string::npos)
        throw Error("nevla: aggregate().count() needs a plain field name");
    stages_.push_back(Value{{"$count", as}});
    reshaped_ = true;
    return *this;
}

Aggregate &Aggregate::sample(int64_t n) {
    if (n <= 0) throw Error("nevla: aggregate().sample() must be > 0");
    stages_.push_back(Value{{"$sample", Value{{"size", n}}}});
    return *this;
}

Aggregate &Aggregate::replaceRoot(const std::string &path) {
    std::string p = path.empty() || path[0] != '$' ? "$" + path : path;
    stages_.push_back(Value{{"$replaceRoot", Value{{"newRoot", p}}}});
    reshaped_ = true;
    return *this;
}

Aggregate &Aggregate::stage(const Value &raw) {
    if (!raw.isObject() || raw.size() != 1 || raw.items()[0].first.empty() || raw.items()[0].first[0] != '$')
        throw Error("nevla: aggregate().stage() needs one stage like {{\"$facet\", {...}}}, got " + raw.toJson());
    const std::string &name = raw.items()[0].first;
    if (name == "$match") return match(raw.items()[0].second);
    stages_.push_back(raw);
    if (reshapes(name)) reshaped_ = true;
    return *this;
}

Aggregate &Aggregate::options(AggregateOptions o) {
    opts_ = std::move(o);
    return *this;
}

Value Aggregate::pipeline() const {
    Value out = Value::array();
    if (!m_->discKey.empty()) out.push_back(Value{{"$match", Value{{m_->discKey, m_->discValue}}}});
    for (const auto &st : stages_.elements()) out.push_back(st);
    return out;
}

std::vector<Value> Aggregate::exec() const { return runAggregate(m_, stages_, opts_); }

std::optional<Value> Aggregate::first() const {
    Value st = stages_;
    st.push_back(Value{{"$limit", 1}});
    auto rows = runAggregate(m_, st, opts_);
    if (rows.empty()) return std::nullopt;
    return std::move(rows[0]);
}

std::vector<Value> Model::aggregate(const Value &pipelineIn) const {
    if (!pipelineIn.isArray()) throw Error("nevla: aggregate() needs an array of stages (Value::array({...}))");
    return runAggregate(m_, castPipeline(*m_, pipelineIn), AggregateOptions{});
}

Aggregate &Aggregate::addField(const std::string &name, const std::string &formula) {
    if (name.empty() || name[0] == '$') throw Error("nevla: aggregate().addField() needs a plain field name");
    Value f = Value::object();
    f[name] = expr(formula);
    return addFields(f);
}

Aggregate &Aggregate::bucketByTime(const std::string &dateField, const std::string &every,
                                   std::vector<Accumulator> accumulators, BucketOptions o) {
    auto [binSize, unit] = parseInterval(every);
    std::string field = dateField[0] == '$' ? dateField.substr(1) : dateField;
    if (!reshaped_) field = m_->schema.resolveAlias(field);
    Value trunc{{"date", "$" + field}, {"unit", unit}, {"binSize", binSize}};
    if (!o.timezone.empty()) trunc["timezone"] = o.timezone;

    Value id = Value::object();
    id["time"] = Value{{"$dateTrunc", trunc}};
    std::vector<std::string> keys;
    for (auto &k : splitWords(o.by)) {
        std::string real = reshaped_ ? k : m_->schema.resolveAlias(k);
        std::string name = lastSegment(real);
        if (name == "time" || id.contains(name))
            throw Error("nevla: bucketByTime(): the 'by' field \"" + real + "\" clashes with another output name");
        id[name] = "$" + real;
        keys.push_back(name);
    }
    Value g = Value::object();
    g["_id"] = std::move(id);
    if (accumulators.empty()) accumulators.push_back(nevla::count());
    addAccumulators(g, accumulators, reshaped_, m_->schema, "bucketByTime()");
    stages_.push_back(Value{{"$group", g}});

    // flatten: {_id: {time, device}, avgValue} -> {time, device, avgValue}  (time first, for charts)
    Value flat = Value::object();
    flat["time"] = "$_id.time";
    for (const auto &k : keys) flat[k] = "$_id." + k;
    stages_.push_back(
        Value{{"$replaceRoot", Value{{"newRoot", Value{{"$mergeObjects", Value::array({flat, "$$ROOT"})}}}}}});
    stages_.push_back(Value{{"$project", Value{{"_id", 0}}}});

    if (o.fillGaps) {
        Value range{{"step", binSize}, {"unit", unit}, {"bounds", keys.empty() ? "full" : "partition"}};
        Value densify{{"field", "time"}, {"range", range}};
        if (!keys.empty()) {
            Value parts = Value::array();
            for (const auto &k : keys) parts.push_back(k);
            densify["partitionByFields"] = parts;
        }
        stages_.push_back(Value{{"$densify", densify}});
        Value zeros = Value::object();  // empty buckets: counts and sums are 0
        for (const auto &a : accumulators)
            if (a.op == "$sum") zeros[a.name] = Value{{"$ifNull", Value::array({"$" + a.name, 0})}};
        if (!zeros.empty()) stages_.push_back(Value{{"$addFields", zeros}});
    }
    Value sortSpec = Value::object();
    sortSpec["time"] = 1;
    for (const auto &k : keys) sortSpec[k] = 1;
    stages_.push_back(Value{{"$sort", sortSpec}});
    reshaped_ = true;
    return *this;
}

namespace {
// $lookup stages that do what Query.populate() does
std::vector<Value> populateStages(const std::shared_ptr<ModelImpl> &m, const PopulateOptions &opt) {
    const std::string path = m->schema.resolveAlias(opt.path);
    for (size_t dot = path.find('.'); dot != std::string::npos; dot = path.find('.', dot + 1)) {
        const Field *parent = m->schema.path(path.substr(0, dot));
        if (parent && parent->type == FieldType::Array)
            throw Error("nevla: aggregate().populate(\"" + path +
                        "\"): paths inside arrays of sub-documents are not supported here; use find().populate()");
    }
    std::string refName, local, foreign;
    bool justOne = false, idArray = false;
    if (const VirtualPopulate *vp = findVirtualPopulate(m->schema, path)) {
        refName = opt.model.empty() ? vp->ref : opt.model;
        local = vp->localField;
        foreign = vp->foreignField;
        justOne = vp->justOne;
    } else {
        const Field *f = m->schema.path(path);
        idArray = f && f->type == FieldType::Array;
        if (idArray && f->of_) f = f->of_.get();
        refName = !opt.model.empty() ? opt.model : f ? f->ref_ : "";
        if (!f || refName.empty())
            throw Error("nevla: aggregate().populate(\"" + path +
                        "\"): the field has no .ref(\"Model\") and is not a virtualPopulate");
        local = path;
        foreign = "_id";
        justOne = !idArray;
    }
    auto ref = lookup(refName);
    bool keepOrder = idArray && opt.sort.isNull();

    Value inner = Value::array();
    if (!ref->discKey.empty()) inner.push_back(Value{{"$match", Value{{ref->discKey, ref->discValue}}}});
    Value match = asFilter(opt.match);
    if (!match.empty()) inner.push_back(Value{{"$match", castFilter(ref->schema, match)}});
    if (!opt.sort.isNull()) inner.push_back(Value{{"$sort", detail::parseSort(opt.sort)}});
    if (opt.limit > 0 && !keepOrder && !justOne) inner.push_back(Value{{"$limit", opt.limit}});
    for (const auto &child : opt.populate)
        for (auto &st : populateStages(ref, child)) inner.push_back(std::move(st));
    std::string select;
    for (auto &tok : splitWords(opt.select))
        if (tok != "-_id" || !keepOrder) select += tok + " ";  // the order fix needs _id
    Value proj = buildProjection(ref->schema, select);
    if (!proj.empty()) inner.push_back(Value{{"$project", proj}});

    const std::string tmp = "__nv_pop";
    Value lk{
        {"from", ref->collection}, {"localField", local}, {"foreignField", foreign}, {"as", keepOrder ? tmp : path}};
    if (!inner.empty()) lk["pipeline"] = inner;
    std::vector<Value> out;
    out.push_back(Value{{"$lookup", lk}});

    if (keepOrder) {  // same order as the ids in the array, missing ones dropped (like Query.populate)
        Value pick{
            {"$let",
             Value{{"vars", Value{{"i", Value{{"$indexOfArray", Value::array({"$" + tmp + "._id", "$$id"})}}}}},
                   {"in", Value{{"$cond", Value::array({Value{{"$gte", Value::array({"$$i", 0})}},
                                                        Value{{"$arrayElemAt", Value::array({"$" + tmp, "$$i"})}},
                                                        Value()})}}}}}};
        Value mapped{{"$map", Value{{"input", Value{{"$ifNull", Value::array({"$" + path, Value::array()})}}},
                                    {"as", "id"},
                                    {"in", pick}}}};
        Value kept{{"$filter",
                    Value{{"input", mapped}, {"as", "d"}, {"cond", Value{{"$ne", Value::array({"$$d", Value()})}}}}}};
        if (opt.limit > 0) kept = Value{{"$slice", Value::array({kept, opt.limit})}};
        Value set = Value::object();
        set[path] = kept;
        out.push_back(Value{{"$addFields", set}});
        out.push_back(Value{{"$project", Value{{tmp, 0}}}});
    } else if (justOne) {  // single document or null
        Value set = Value::object();
        set[path] = Value{{"$ifNull", Value::array({Value{{"$arrayElemAt", Value::array({"$" + path, 0})}}, Value()})}};
        out.push_back(Value{{"$addFields", set}});
    }
    return out;
}

std::string shellValue(const Value &v) {
    switch (v.type()) {
        case Value::Type::ObjectId:
            return "ObjectId(\"" + v.str() + "\")";
        case Value::Type::Date:
            return "ISODate(\"" + v.asDate().iso() + "\")";
        case Value::Type::Array: {
            std::string out = "[";
            bool first = true;
            for (const auto &e : v.elements()) {
                out += (first ? "" : ", ") + shellValue(e);
                first = false;
            }
            return out + "]";
        }
        case Value::Type::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto &kv : v.items()) {
                out += (first ? "" : ", ") + Value(kv.first).toJson() + ": " + shellValue(kv.second);
                first = false;
            }
            return out + "}";
        }
        default:
            return v.toJson();
    }
}

std::string describePlan(const Value &n) {
    if (n.contains("queryPlan")) return describePlan(n["queryPlan"]);
    std::string s = n["stage"].str();
    if (n.contains("indexName")) s += "(" + n["indexName"].str() + ")";
    if (n.contains("inputStage")) s += " > " + describePlan(n["inputStage"]);
    if (n.contains("inputStages")) {
        s += " > [";
        bool first = true;
        for (const auto &c : n["inputStages"].elements()) {
            s += (first ? "" : ", ") + describePlan(c);
            first = false;
        }
        s += "]";
    }
    return s;
}

const Value *findKey(const Value &v, const std::string &key) {
    if (v.isObject()) {
        if (const Value *hit = v.find(key)) return hit;
        for (const auto &kv : v.items())
            if (const Value *hit = findKey(kv.second, key)) return hit;
    } else if (v.isArray()) {
        for (const auto &e : v.elements())
            if (const Value *hit = findKey(e, key)) return hit;
    }
    return nullptr;
}

std::string keyText(const Value &k) {
    if (k.isNull()) return "null";
    if (k.isObject() || k.isArray()) return k.toJson();
    return k.str();
}
}  // namespace

Aggregate &Aggregate::populate(const std::string &path) {
    PopulateOptions o;
    o.path = path;
    return populate(o);
}

Aggregate &Aggregate::populate(const PopulateOptions &options) {
    if (reshaped_)
        throw Error("nevla: aggregate().populate(\"" + options.path +
                    "\") must come before group() / project() / addFields() (the schema is needed)");
    for (auto &st : populateStages(m_, options)) stages_.push_back(std::move(st));
    return *this;
}

std::string Aggregate::toShell() const {
    return "db.getCollection(" + Value(m_->collection).toJson() + ").aggregate(" + shellValue(pipeline()) + ")";
}

Value Aggregate::explain() const {
    auto c = currentConn();
    Value cmd = Value::object();
    cmd["explain"] = Value{{"aggregate", m_->collection}, {"pipeline", pipeline()}, {"cursor", Value::object()}};
    cmd["verbosity"] = "queryPlanner";
    return guard([&]() -> Value {
        auto entry = c->pool->acquire();
        return fromBson((*entry)[c->db].run_command(toBson(cmd)).view());
    });
}

std::string Aggregate::plan() const {
    Value e = explain();
    const Value *wp = findKey(e, "winningPlan");
    return wp ? describePlan(*wp) : "unknown";
}

/* ---------------- one-line reports ---------------- */
Value Model::countBy(const std::string &field, const Value &filter) const {
    Aggregate a(m_);
    if (!asFilter(filter).empty()) a.match(filter);
    Value out = Value::object();
    for (auto &r : a.group(field, {count()}).sort("-count _id").exec()) out[keyText(r["_id"])] = r["count"];
    return out;
}

Value Model::sumBy(const std::string &groupField, const std::string &sumField, const Value &filter) const {
    Aggregate a(m_);
    if (!asFilter(filter).empty()) a.match(filter);
    Value out = Value::object();
    for (auto &r : a.group(groupField, {sum(sumField).as("v")}).sort("-v _id").exec()) out[keyText(r["_id"])] = r["v"];
    return out;
}

Value Model::avgBy(const std::string &groupField, const std::string &avgField, const Value &filter) const {
    Aggregate a(m_);
    if (!asFilter(filter).empty()) a.match(filter);
    Value out = Value::object();
    for (auto &r : a.group(groupField, {avg(avgField).as("v")}).sort("-v _id").exec()) out[keyText(r["_id"])] = r["v"];
    return out;
}

Value Model::stats(const std::string &field, const Value &filter) const {
    Aggregate a(m_);
    if (!asFilter(filter).empty()) a.match(filter);
    std::string f = m_->schema.resolveAlias(field);
    Value numeric = Value::object();
    numeric[f] = Value{{"$type", "number"}};  // only numbers count
    a.stage(Value{{"$match", numeric}});
    auto row = a.group("", {count(), sum(f).as("sum"), avg(f).as("avg"), min(f).as("min"), max(f).as("max"),
                            stdDevPop(f).as("stdDev")})
                   .first();
    if (!row)
        return Value{{"count", 0},     {"sum", 0},       {"avg", Value()},
                     {"min", Value()}, {"max", Value()}, {"stdDev", Value()}};
    row->erase("_id");
    return *row;
}

std::vector<Value> Model::latestPerGroup(const std::string &groupField, const std::string &sortField,
                                         const Value &filter) const {
    Aggregate a(m_);
    if (!asFilter(filter).empty()) a.match(filter);
    return a.sort("-" + sortField)
        .group(groupField, {first("$$ROOT").as("doc")})
        .replaceRoot("doc")
        .sort("-" + sortField)
        .exec();
}

Aggregate Model::aggregate() const { return Aggregate(m_); }

ChangeStream Model::watch(const Value &pipeline, WatchOptions options) const {
    auto impl = std::make_shared<ChangeStreamImpl>();
    mongocxx::pipeline p;
    if (!m_->discKey.empty()) {
        Value match = Value::object();
        match["$match"]["$or"] =
            Value::array({Value{{"fullDocument." + m_->discKey, m_->discValue}}, Value{{"operationType", "delete"}}});
        p.append_stage(toBson(match));
    }
    for (const auto &stage : pipeline.elements()) p.append_stage(toBson(stage));
    mongocxx::options::change_stream opts;
    if (options.fullDocument) opts.full_document("updateLookup");
    if (options.maxAwaitMS > 0) opts.max_await_time(std::chrono::milliseconds(options.maxAwaitMS));
    guard([&] {
        impl->op = open(*m_);
        OpTrace nv_trace(m_->collection, "watch", pipeline);
        impl->stream = std::make_unique<mongocxx::change_stream>(NV(impl->op, watch, p, opts));
    });
    ChangeStream cs;
    cs.impl_ = std::move(impl);
    return cs;
}

Value Model::call(const std::string &staticName, const Value &args) const {
    auto it = m_->schema.statics_.find(staticName);
    if (it == m_->schema.statics_.end())
        throw Error("nevla: static \"" + staticName + "\" is not defined on the schema of model \"" + m_->name + "\"");
    return it->second(*this, args);
}

void Model::createIndexes() const {
    auto c = currentConn();
    createIndexesNow(*m_, *c);
    m_->indexGen = c->gen;
}

std::vector<Value> Model::listIndexes() const {
    std::vector<Value> out;
    guard([&] {
        Op o = open(*m_);
        auto cursor = o.coll.list_indexes();
        for (auto &&doc : cursor) out.push_back(fromBson(doc));
    });
    return out;
}

void Model::syncIndexes() const {
    createIndexes();
    std::vector<IndexDef> wanted = wantedIndexes(*m_);
    {
        std::lock_guard<std::mutex> lk(m_->childMu);
        for (auto &ch : m_->children) {  // children share the collection
            auto more = wantedIndexes(*ch.second);
            wanted.insert(wanted.end(), more.begin(), more.end());
        }
    }
    std::set<std::string> keep;
    for (const auto &w : wanted) keep.insert(w.keys.toJson());
    for (const auto &idx : listIndexes()) {
        std::string name = idx["name"].str();
        if (name == "_id_") continue;
        // compare key specs with normalized numbers (1 vs 1.0)
        Value keys = Value::object();
        for (const auto &kv : idx["key"].items())
            keys[kv.first] = kv.second.isNumber() ? Value(static_cast<int64_t>(kv.second.asDouble())) : kv.second;
        if (keep.count(keys.toJson())) continue;
        guard([&] {
            Op o = open(*m_);
            OpTrace nv_trace(m_->collection, "dropIndex", Value(name));
            o.coll.indexes().drop_one(name);
        });
    }
}

void Model::dropCollection() const {
    guard([&] {
        Op o = open(*m_);
        OpTrace nv_trace(m_->collection, "drop", Value::object());
        o.coll.drop();
    });
    m_->indexGen = 0;  // recreate indexes before the next operation
    std::lock_guard<std::mutex> lk(m_->childMu);
    for (auto &ch : m_->children) ch.second->indexGen = 0;
}

Model Model::discriminator(const std::string &name, const Schema &extra) const {
    auto child = std::make_shared<ModelImpl>();
    child->name = name;
    child->collection = m_->collection;
    child->schema = m_->schema;
    child->schema.add(extra);
    child->discKey = m_->schema.options.discriminatorKey;
    child->discValue = name;
    {
        std::lock_guard<std::mutex> lk(m_->childMu);
        m_->children[name] = child;
    }
    std::lock_guard<std::mutex> lk(gModelsMu);
    gModels[name] = child;
    return Model(child);
}

Model model(const std::string &name, const Schema &schema, const std::string &collection) {
    if (name.empty()) throw Error("nevla: model name cannot be empty");
    auto impl = std::make_shared<ModelImpl>();
    impl->name = name;
    impl->schema = schema;
    impl->collection = !collection.empty()                  ? collection
                       : !schema.options.collection.empty() ? schema.options.collection
                                                            : pluralize(name);
    std::lock_guard<std::mutex> lk(gModelsMu);
    gModels[name] = impl;
    return Model(impl);
}

Model model(const std::string &name) { return Model(lookup(name)); }

std::vector<std::string> modelNames() {
    std::lock_guard<std::mutex> lk(gModelsMu);
    std::vector<std::string> out;
    for (const auto &kv : gModels) out.push_back(kv.first);
    return out;
}

}  // namespace nevla
