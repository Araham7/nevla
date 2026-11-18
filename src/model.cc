#include "nevla/model.hpp"

#include "bson.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

namespace nevla {

using detail::fromBson;
using detail::toBson;

struct ModelImpl {
    std::string name, collection;
    Schema schema;
    std::string discKey, discValue;                               // discriminator child ho to
    std::map<std::string, std::shared_ptr<ModelImpl>> children;  // base ho to: type -> child
    std::atomic<uint64_t> indexGen{0};
    std::mutex indexMu;

    static Document make(const std::shared_ptr<ModelImpl> &m, Value data, bool isNew) {
        Document d;
        d.m_ = m;
        d.data_ = std::move(data);
        d.isNew_ = isNew;
        return d;
    }
};

namespace {

/* ---------------- connection ---------------- */
std::unique_ptr<mongocxx::pool> gPool;
std::string gDb;
std::atomic<uint64_t> gGeneration{0};
std::mutex gModelsMu;
std::map<std::string, std::shared_ptr<ModelImpl>> gModels;

struct Tx {
    mongocxx::client *client = nullptr;
    mongocxx::client_session *session = nullptr;
};
thread_local Tx tTx;

// mongocxx / bsoncxx ke exceptions -> nevla::Error
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

struct Op {
    std::optional<mongocxx::pool::entry> entry;
    mongocxx::client_session *session = nullptr;
    mongocxx::collection coll;
};

// Transaction ke andar ho to session ke saath, warna seedha
#define NV(o, call, ...) \
    ((o).session ? (o).coll.call(*(o).session __VA_OPT__(, ) __VA_ARGS__) : (o).coll.call(__VA_ARGS__))

void collectIndexFields(const Schema &s, const std::string &prefix, std::vector<std::pair<std::string, bool>> &out) {
    for (const auto &kv : s.fields()) {
        const Field &f = kv.second;
        if (f.unique_ || f.index_) out.emplace_back(prefix + kv.first, f.unique_);
        if (f.type == FieldType::Object && f.schema_) collectIndexFields(*f.schema_, prefix + kv.first + ".", out);
    }
}

void createIndexesNow(ModelImpl &m) {
    if (!gPool) throw Error("nevla: pehle nevla::connect() bulao");
    guard([&] {
        auto entry = gPool->acquire();
        auto coll = (*entry)[gDb][m.collection];
        std::vector<std::pair<std::string, bool>> fields;
        collectIndexFields(m.schema, "", fields);
        for (const auto &f : fields) {
            Value keys = Value::object(), opts = Value::object();
            keys[f.first] = 1;
            if (f.second) opts["unique"] = true;
            coll.create_index(toBson(keys), toBson(opts));
        }
        for (const auto &i : m.schema.indexes_) coll.create_index(toBson(i.keys), toBson(i.options));
    });
}

void ensureIndexes(ModelImpl &m) {
    uint64_t gen = gGeneration.load();
    if (m.indexGen.load() == gen) return;
    std::lock_guard<std::mutex> lk(m.indexMu);
    if (m.indexGen.load() == gen) return;
    if (m.schema.options.autoIndex) createIndexesNow(m);
    m.indexGen = gen;
}

Op open(ModelImpl &m) {
    if (!gPool) throw Error("nevla: pehle nevla::connect() bulao");
    ensureIndexes(m);
    Op o;
    if (tTx.client) {
        o.session = tTx.session;
        o.coll = (*tTx.client)[gDb][m.collection];
    } else {
        o.entry.emplace(gPool->acquire());
        o.coll = (**o.entry)[gDb][m.collection];
    }
    return o;
}

/* ---------------- helpers ---------------- */
std::string pluralize(const std::string &name) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    auto ends = [&](const char *suffix) {
        std::string x(suffix);
        return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
    };
    if (s.empty()) return s;
    if (ends("y") && s.size() > 1 && std::string("aeiou").find(s[s.size() - 2]) == std::string::npos)
        return s.substr(0, s.size() - 1) + "ies";
    if (ends("s") || ends("x") || ends("z") || ends("ch") || ends("sh")) return s + "es";
    return s + "s";
}

std::shared_ptr<ModelImpl> lookup(const std::string &name) {
    std::lock_guard<std::mutex> lk(gModelsMu);
    auto it = gModels.find(name);
    if (it == gModels.end()) throw Error("nevla: model \"" + name + "\" bana hi nahi (nevla::model(name, schema) pehle bulao)");
    return it->second;
}

[[noreturn]] void throwValidation(const std::string &model, const std::map<std::string, std::string> &errors) {
    std::string msg = model + " validation failed: ";
    bool first = true;
    for (const auto &e : errors) {
        if (!first) msg += ", ";
        first = false;
        msg += e.first + ": " + e.second;
    }
    throw ValidationError(msg, errors);
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

bool isOperatorObject(const Value &v) {
    if (!v.isObject() || v.empty()) return false;
    for (const auto &m : v.items())
        if (m.first.empty() || m.first[0] != '$') return false;
    return true;
}

// Filter ka ek value field ke type me (query me array field ka element bhi match hota hai)
Value castScalar(const Schema &s, const Field &f, const Value &v, const std::string &path) {
    if (v.isNull()) return v;
    if (path == "_id" && !s.hasOwnId())
        return v.isString() && ObjectId::isValid(v.asString()) ? Value(ObjectId(v.asString())) : v;
    if (f.type == FieldType::Array && !v.isArray()) {
        if (f.schema_ || !f.of_) return v;
        return detail::castValue(*f.of_, v, path);
    }
    if (f.type == FieldType::Object) return v;
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
        } else {
            out[op] = m.second;  // $exists, $regex, $options, $size, $elemMatch ...
        }
    }
    return out;
}

Value castFilter(const Schema &s, const Value &filter) {
    Value out = Value::object();
    for (const auto &m : filter.items()) {
        const std::string &key = m.first;
        if (key == "$and" || key == "$or" || key == "$nor") {
            Value arr = Value::array();
            for (const auto &e : m.second.elements()) arr.push_back(castFilter(s, e));
            out[key] = std::move(arr);
            continue;
        }
        bool mixed = false;
        const Field *f = key.empty() || key[0] == '$' ? nullptr : s.path(key, &mixed);
        out[key] = (f && !mixed) ? castOperand(s, *f, m.second, key) : m.second;
    }
    return out;
}

Value prepareFilter(ModelImpl &m, const Value &filter) {
    Value out = castFilter(m.schema, filter.isObject() ? filter : Value::object());
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

Value castUpdate(ModelImpl &m, const Value &update, bool upsert, bool runValidators, const Value &filter) {
    if (!update.isObject()) throw Error("nevla: update ek object hona chahiye");
    Value u = Value::object();
    bool hasOperator = false;
    for (const auto &kv : update.items()) hasOperator |= (!kv.first.empty() && kv.first[0] == '$');
    if (hasOperator)
        u = update;
    else
        u["$set"] = update;  // { name: "x" } -> { $set: { name: "x" } }  (Mongoose jaisa)

    std::map<std::string, std::string> errors;
    for (auto &op : u.items()) {
        if (!op.second.isObject()) continue;
        bool setLike = op.first == "$set" || op.first == "$setOnInsert" || op.first == "$min" || op.first == "$max";
        bool pushLike = op.first == "$push" || op.first == "$addToSet";
        if (!setLike && !pushLike) continue;
        for (auto &kv : op.second.items()) {
            bool mixed = false;
            const Field *f = m.schema.path(kv.first, &mixed);
            if (!f || mixed) continue;
            if (kv.first == "_id" && !m.schema.hasOwnId()) continue;
            if (setLike) {
                kv.second = detail::castValue(*f, kv.second, kv.first);
                if (runValidators && op.first == "$set") detail::validateValue(*f, &kv.second, kv.first, errors);
            } else if (f->type == FieldType::Array) {
                if (kv.second.isObject() && kv.second.contains("$each")) {
                    Value arr = Value::array();
                    for (const auto &e : kv.second["$each"].elements()) arr.push_back(castElement(*f, e, kv.first));
                    kv.second["$each"] = std::move(arr);
                } else {
                    kv.second = castElement(*f, kv.second, kv.first);
                }
            }
        }
    }
    if (!errors.empty()) throwValidation(m.name, errors);

    if (m.schema.options.timestamps) {
        Date now = Date::now();
        if (!u["$set"].contains("updatedAt")) u["$set"]["updatedAt"] = now;
        if (upsert && !u["$set"].contains("createdAt")) u["$setOnInsert"]["createdAt"] = now;
    }
    if (upsert && !m.discKey.empty()) u["$setOnInsert"][m.discKey] = m.discValue;
    if (upsert) {  // naya document bane to schema ke defaults bhi (Mongoose ka setDefaultsOnInsert)
        Value defaults = Value::object();
        detail::applyDefaults(m.schema, defaults);
        for (auto &d : defaults.items()) {
            const std::string &name = d.first;
            bool touched = name == "_id" || filter.contains(name);
            for (const auto &op : u.items()) {
                if (touched || !op.second.isObject()) continue;
                for (const auto &kv : op.second.items())
                    touched |= kv.first == name || kv.first.rfind(name + ".", 0) == 0;
            }
            for (const auto &fk : filter.items()) touched |= fk.first.rfind(name + ".", 0) == 0;
            if (!touched) u["$setOnInsert"][name] = std::move(d.second);
        }
    }
    if (u.contains("$set") && u["$set"].empty()) u.erase("$set");
    return u;
}

Value buildProjection(const Schema &s, const std::string &select) {
    std::vector<std::string> include, exclude, plus;
    std::istringstream in(select);
    std::string tok;
    while (in >> tok) {
        if (tok[0] == '-')
            exclude.push_back(tok.substr(1));
        else if (tok[0] == '+')
            plus.push_back(tok.substr(1));
        else
            include.push_back(tok);
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
    if (m->children.empty()) return m;
    auto it = m->children.find(data[m->schema.options.discriminatorKey].asString());
    return it == m->children.end() ? m : it->second;
}

std::vector<Value> runFind(const std::shared_ptr<ModelImpl> &m, const detail::QuerySpec &spec, bool one,
                           const char *opName);

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

void populateValues(ModelImpl &m, std::vector<Value *> &docs, const std::string &path) {
    const Field *f = m.schema.path(path);
    if (f && f->type == FieldType::Array && f->of_) f = f->of_.get();
    if (!f || f->ref_.empty())
        throw Error("nevla: populate(\"" + path + "\"): is path par .ref(\"Model\") nahi laga");
    auto ref = lookup(f->ref_);

    std::vector<Value *> slots;
    for (Value *d : docs) collectSlots(*d, path, slots);
    Value ids = Value::array();
    std::set<std::string> seen;
    auto want = [&](const Value &v) {
        if (v.isNull() || v.isObject()) return;  // khaali ya pehle se populated
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
    spec.filter["_id"]["$in"] = ids;
    std::map<std::string, Value> byId;
    for (auto &d : runFind(ref, spec, false, "find")) byId[d["_id"].toJson()] = d;

    for (Value *s : slots) {
        if (s->isArray()) {
            Value arr = Value::array();
            for (const auto &e : s->elements()) {
                if (e.isObject()) {
                    arr.push_back(e);
                    continue;
                }
                auto it = byId.find(e.toJson());
                if (it != byId.end()) arr.push_back(it->second);  // jo mila hi nahi wo hat jata hai
            }
            *s = std::move(arr);
        } else if (!s->isNull() && !s->isObject()) {
            auto it = byId.find(s->toJson());
            *s = it == byId.end() ? Value() : it->second;
        }
    }
}

std::vector<Value> runFind(const std::shared_ptr<ModelImpl> &m, const detail::QuerySpec &spec, bool one,
                           const char *opName) {
    Value filter = prepareFilter(*m, spec.filter);
    runQueryHooks(*m, opName, filter, nullptr);
    mongocxx::options::find opts;
    if (spec.sort.isObject() && !spec.sort.empty()) opts.sort(toBson(spec.sort));
    if (one)
        opts.limit(1);
    else if (spec.limit > 0)
        opts.limit(spec.limit);
    if (spec.skip > 0) opts.skip(spec.skip);
    Value proj = buildProjection(m->schema, spec.select);
    if (!proj.empty()) opts.projection(toBson(proj));

    std::vector<Value> out;
    guard([&] {
        Op o = open(*m);
        auto cursor = NV(o, find, toBson(filter), opts);
        for (auto &&doc : cursor) out.push_back(fromBson(doc));
    });
    if (!spec.populate.empty() && !out.empty()) {
        std::vector<Value *> ptrs;
        for (auto &v : out) ptrs.push_back(&v);
        for (const auto &p : spec.populate) populateValues(*m, ptrs, p);
    }
    return out;
}

Value idFilter(const Value &id) {
    Value f = Value::object();
    f["_id"] = id;
    return f;
}

UpdateResult runUpdate(const std::shared_ptr<ModelImpl> &m, const Value &filterIn, const Value &updateIn,
                       UpdateOptions options, bool many) {
    Value filter = prepareFilter(*m, filterIn);
    Value update = updateIn;
    runQueryHooks(*m, many ? "updateMany" : "updateOne", filter, &update);
    update = castUpdate(*m, update, options.upsert, options.runValidators, filter);
    UpdateResult res;
    if (update.empty()) return res;
    mongocxx::options::update opts;
    if (options.upsert) opts.upsert(true);
    guard([&] {
        Op o = open(*m);
        auto r = many ? NV(o, update_many, toBson(filter), toBson(update), opts)
                      : NV(o, update_one, toBson(filter), toBson(update), opts);
        if (!r) return;
        res.matchedCount = r->matched_count();
        res.modifiedCount = r->modified_count();
        if (auto id = r->upserted_id()) res.upsertedId = fromBson(id->get_value());
    });
    return res;
}

int64_t runDelete(const std::shared_ptr<ModelImpl> &m, const Value &filterIn, bool many) {
    Value filter = prepareFilter(*m, filterIn);
    runQueryHooks(*m, many ? "deleteMany" : "deleteOne", filter, nullptr);
    return guard([&]() -> int64_t {
        Op o = open(*m);
        auto r = many ? NV(o, delete_many, toBson(filter)) : NV(o, delete_one, toBson(filter));
        return r ? r->deleted_count() : 0;
    });
}

}  // namespace

/* ======================= connection ======================= */
void connect(const std::string &uri, const std::string &dbName) {
    mongocxx::instance::current();
    guard([&] {
        mongocxx::uri u{uri};
        auto pool = std::make_unique<mongocxx::pool>(u);
        std::string db = !dbName.empty() ? dbName : !u.database().empty() ? std::string(u.database()) : "test";
        {
            auto entry = pool->acquire();
            (*entry)[db].run_command(make_document(kvp("ping", 1)));  // band ho to yahin Error
        }
        gPool = std::move(pool);
        gDb = db;
        gGeneration++;
    });
}

void disconnect() { gPool.reset(); }
bool connected() { return gPool != nullptr; }

void transaction(const std::function<void()> &fn) {
    if (!gPool) throw Error("nevla: pehle nevla::connect() bulao");
    if (tTx.client) {  // pehle se transaction ke andar: usi me chalao
        fn();
        return;
    }
    guard([&] {
        auto entry = gPool->acquire();
        auto session = entry->start_session();
        tTx = {&*entry, &session};
        struct Reset {
            ~Reset() { tTx = {}; }
        } reset;
        session.with_transaction([&](mongocxx::client_session *) { fn(); });
    });
}

Value detail::parseSort(const Value &spec) {
    if (spec.isObject()) return spec;
    Value out = Value::object();
    std::istringstream in(spec.asString());
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
Value Document::get(std::string_view path) const {
    if (m_)
        for (const auto &v : m_->schema.virtuals_)
            if (v.first == path) return v.second(*this);
    const Value *p = data_.path(path);
    return p ? *p : Value();
}

const Value &Document::operator[](std::string_view path) const {
    static const Value kNull;
    if (m_)
        for (const auto &v : m_->schema.virtuals_)
            if (v.first == path) return virtualCache_[v.first] = v.second(*this);
    const Value *p = data_.path(path);
    return p ? *p : kNull;
}

bool Document::has(std::string_view path) const {
    const Value *p = data_.path(path);
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
    bool mixed = false;
    const Field *f = s.path(p, &mixed);
    Value casted = value;
    if (p == "_id" && !s.hasOwnId()) {
        if (value.isString() && ObjectId::isValid(value.asString())) casted = ObjectId(value.asString());
    } else if (!f) {
        std::string top = p.substr(0, p.find('.'));
        if (s.options.strict && top != s.options.versionKey && top != s.options.discriminatorKey)
            return *this;  // strict: schema me nahi hai, chhod do
    } else if (!mixed) {
        if (f->immutable_ && !isNew_) return *this;
        casted = detail::castValue(*f, value, p);
    }
    data_.pathRef(p) = std::move(casted);
    modified_.insert(p);
    return *this;
}

Document &Document::set(const Value &object) {
    for (const auto &m : object.items()) set(m.first, m.second);
    return *this;
}

Document &Document::unset(std::string_view path) {
    data_.erasePath(path);
    modified_.insert(std::string(path));
    return *this;
}

Document &Document::push(std::string_view arrayPath, const Value &item) {
    std::string p(arrayPath);
    Value casted = item;
    if (m_) {
        bool mixed = false;
        const Field *f = m_->schema.path(p, &mixed);
        if (f && !mixed && f->type == FieldType::Array) casted = castElement(*f, item, p);
    }
    Value &arr = data_.pathRef(p);
    if (!arr.isArray()) arr = Value::array();
    arr.push_back(std::move(casted));
    modified_.insert(p);
    return *this;
}

Value &Document::mut(std::string_view path) {
    modified_.insert(std::string(path));
    return data_.pathRef(path);
}

void Document::markModified(std::string_view path) { modified_.insert(std::string(path)); }

bool Document::isModified(std::string_view path) const {
    if (path.empty()) return isNew_ || !modified_.empty();
    std::string p(path);
    if (isNew_ && data_.path(p)) return true;  // naye document me jo bhi set hai wo "modified" hai
    for (const auto &mp : modified_)
        if (mp == p || mp.rfind(p + ".", 0) == 0 || p.rfind(mp + ".", 0) == 0) return true;
    return false;
}

std::vector<std::string> Document::modifiedPaths() const { return {modified_.begin(), modified_.end()}; }

void Document::validate() {
    if (!m_) return;
    runDocHooks(m_->schema.preDoc_, "validate", *this);
    std::map<std::string, std::string> errors;
    detail::validateObject(m_->schema, data_, "", errors);
    if (!errors.empty()) throwValidation(m_->name, errors);
    runDocHooks(m_->schema.postDoc_, "validate", *this);
}

Document &Document::save() {
    if (!m_) throw Error("nevla: ye Document kisi model ka nahi hai (Model({...}) se banao)");
    validate();  // Mongoose jaisa: validation pehla pre-save kaam hai
    runDocHooks(m_->schema.preDoc_, "save", *this);
    const SchemaOptions &opt = m_->schema.options;
    Date now = Date::now();

    if (isNew_) {
        if (opt.timestamps) {
            if (!has("createdAt")) data_["createdAt"] = now;
            if (!has("updatedAt")) data_["updatedAt"] = now;
        }
        if (!opt.versionKey.empty() && !data_.contains(opt.versionKey)) data_[opt.versionKey] = 0;
        guard([&] {
            Op o = open(*m_);
            NV(o, insert_one, toBson(data_));
        });
        isNew_ = false;
        modified_.clear();
    } else if (!modified_.empty()) {
        Value set = Value::object(), unset = Value::object();
        std::string previous;
        for (const auto &p : modified_) {  // sorted: "a" ke baad "a.b" aata hai -> parent kaafi hai
            if (p == "_id") continue;
            if (!previous.empty() && p.rfind(previous + ".", 0) == 0) continue;
            previous = p;
            const Value *v = data_.path(p);
            if (v)
                set[p] = *v;
            else
                unset[p] = "";
        }
        if (opt.timestamps) {
            data_["updatedAt"] = now;
            set["updatedAt"] = now;
        }
        Value update = Value::object();
        if (!set.empty()) update["$set"] = std::move(set);
        if (!unset.empty()) update["$unset"] = std::move(unset);
        if (!update.empty()) {
            int64_t matched = guard([&]() -> int64_t {
                Op o = open(*m_);
                auto r = NV(o, update_one, toBson(idFilter(data_["_id"])), toBson(update));
                return r ? r->matched_count() : 0;
            });
            if (matched == 0)
                throw NotFoundError("nevla: save(): " + m_->name + " document _id " + id() + " DB me nahi mila");
        }
        modified_.clear();
    }
    runDocHooks(m_->schema.postDoc_, "save", *this);
    return *this;
}

void Document::deleteOne() {
    if (!m_) throw Error("nevla: ye Document kisi model ka nahi hai");
    runDocHooks(m_->schema.preDoc_, "deleteOne", *this);
    guard([&] {
        Op o = open(*m_);
        NV(o, delete_one, toBson(idFilter(data_["_id"])));
    });
    runDocHooks(m_->schema.postDoc_, "deleteOne", *this);
}

Document &Document::populate(const std::string &path) {
    if (!m_) throw Error("nevla: ye Document kisi model ka nahi hai");
    std::vector<Value *> docs{&data_};
    populateValues(*m_, docs, path);
    return *this;
}

Value Document::call(const std::string &method, const Value &args) {
    if (m_) {
        auto it = m_->schema.methods_.find(method);
        if (it != m_->schema.methods_.end()) return it->second(*this, args);
    }
    throw Error("nevla: method \"" + method + "\" schema me nahi hai");
}

Value Document::toObject() const { return data_; }

Value Document::toJSON() const {
    Value out = data_;
    if (m_) {
        if (m_->schema.options.virtuals) {
            out["id"] = id();
            for (const auto &v : m_->schema.virtuals_) out[v.first] = v.second(*this);
        }
        if (m_->schema.jsonTransform_) m_->schema.jsonTransform_(*this, out);
    }
    return out;
}

/* ======================= Query ======================= */
std::vector<Value> Query::lean() { return runFind(m_, spec_, false, "find"); }

std::vector<Document> Query::exec() {
    std::vector<Document> out;
    for (auto &v : runFind(m_, spec_, false, "find")) {
        auto impl = pickModel(m_, v);
        out.push_back(ModelImpl::make(impl, std::move(v), false));
    }
    return out;
}

int64_t Query::count() {
    Value filter = prepareFilter(*m_, spec_.filter);
    runQueryHooks(*m_, "countDocuments", filter, nullptr);
    return guard([&]() -> int64_t {
        Op o = open(*m_);
        return NV(o, count_documents, toBson(filter));
    });
}

std::optional<Value> QueryOne::lean() {
    auto rows = runFind(m_, spec_, true, "findOne");
    if (rows.empty()) return std::nullopt;
    return std::move(rows[0]);
}

std::optional<Document> QueryOne::exec() {
    auto v = lean();
    if (!v) return std::nullopt;
    auto impl = pickModel(m_, *v);
    return ModelImpl::make(impl, std::move(*v), false);
}

Document QueryOne::orFail() {
    auto d = exec();
    if (!d) throw NotFoundError("No document found for query " + spec_.filter.toJson() + " on model \"" + m_->name + "\"");
    return std::move(*d);
}

/* ======================= Model ======================= */
const std::string &Model::name() const { return m_->name; }
const std::string &Model::collectionName() const { return m_->collection; }
const Schema &Model::schema() const { return m_->schema; }

Document Model::operator()(const Value &object) const {
    const Schema &s = m_->schema;
    Value data = detail::castObject(s, object);
    detail::applyDefaults(s, data);
    if (!data.contains("_id") && !s.hasOwnId()) data["_id"] = ObjectId::generate();
    if (!m_->discKey.empty()) data[m_->discKey] = m_->discValue;
    // _id sabse pehle
    Value ordered = Value::object();
    if (const Value *id = data.find("_id")) ordered["_id"] = *id;
    for (auto &kv : data.items())
        if (kv.first != "_id") ordered[kv.first] = std::move(kv.second);
    return ModelImpl::make(m_, std::move(ordered), true);
}

Document Model::hydrate(const Value &object) const { return ModelImpl::make(pickModel(m_, object), object, false); }

Document Model::create(const Value &object) const {
    Document d = (*this)(object);
    d.save();
    return d;
}

std::vector<Document> Model::insertMany(const std::vector<Value> &objects) const {
    std::vector<Document> docs;
    if (objects.empty()) return docs;
    const SchemaOptions &opt = m_->schema.options;
    Date now = Date::now();
    std::vector<bsoncxx::document::value> raw;
    for (const auto &o : objects) {
        Document d = (*this)(o);
        d.validate();
        if (opt.timestamps) {
            if (!d.has("createdAt")) d.data_["createdAt"] = now;
            if (!d.has("updatedAt")) d.data_["updatedAt"] = now;
        }
        if (!opt.versionKey.empty() && !d.data_.contains(opt.versionKey)) d.data_[opt.versionKey] = 0;
        raw.push_back(toBson(d.data_));
        docs.push_back(std::move(d));
    }
    guard([&] {
        Op o = open(*m_);
        NV(o, insert_many, raw);
    });
    for (auto &d : docs) d.isNew_ = false;
    return docs;
}

Query Model::find(const Value &filter) const { return Query(m_, filter); }
QueryOne Model::findOne(const Value &filter) const { return QueryOne(m_, filter); }
QueryOne Model::findById(const Value &id) const { return QueryOne(m_, idFilter(id)); }

UpdateResult Model::updateOne(const Value &filter, const Value &update, UpdateOptions options) const {
    return runUpdate(m_, filter, update, options, false);
}
UpdateResult Model::updateMany(const Value &filter, const Value &update, UpdateOptions options) const {
    return runUpdate(m_, filter, update, options, true);
}

std::optional<Document> Model::findOneAndUpdate(const Value &filterIn, const Value &updateIn,
                                                FindUpdateOptions options) const {
    Value filter = prepareFilter(*m_, filterIn);
    Value update = updateIn;
    runQueryHooks(*m_, "findOneAndUpdate", filter, &update);
    update = castUpdate(*m_, update, options.upsert, options.runValidators, filter);
    mongocxx::options::find_one_and_update opts;
    if (options.returnNew) opts.return_document(mongocxx::options::return_document::k_after);
    if (options.upsert) opts.upsert(true);
    Value proj = buildProjection(m_->schema, "");
    if (!proj.empty()) opts.projection(toBson(proj));
    auto found = guard([&]() -> std::optional<Value> {
        Op o = open(*m_);
        auto r = NV(o, find_one_and_update, toBson(filter), toBson(update), opts);
        if (!r) return std::nullopt;
        return fromBson(r->view());
    });
    if (!found) return std::nullopt;
    return ModelImpl::make(pickModel(m_, *found), std::move(*found), false);
}

std::optional<Document> Model::findByIdAndUpdate(const Value &id, const Value &update, FindUpdateOptions options) const {
    return findOneAndUpdate(idFilter(id), update, options);
}

std::optional<Document> Model::findOneAndDelete(const Value &filterIn) const {
    Value filter = prepareFilter(*m_, filterIn);
    runQueryHooks(*m_, "findOneAndDelete", filter, nullptr);
    auto found = guard([&]() -> std::optional<Value> {
        Op o = open(*m_);
        auto r = NV(o, find_one_and_delete, toBson(filter));
        if (!r) return std::nullopt;
        return fromBson(r->view());
    });
    if (!found) return std::nullopt;
    return ModelImpl::make(pickModel(m_, *found), std::move(*found), false);
}

std::optional<Document> Model::findByIdAndDelete(const Value &id) const { return findOneAndDelete(idFilter(id)); }

int64_t Model::deleteOne(const Value &filter) const { return runDelete(m_, filter, false); }
int64_t Model::deleteMany(const Value &filter) const { return runDelete(m_, filter, true); }

int64_t Model::countDocuments(const Value &filter) const { return Query(m_, filter).count(); }

int64_t Model::estimatedDocumentCount() const {
    return guard([&]() -> int64_t {
        Op o = open(*m_);
        return o.coll.estimated_document_count();
    });
}

bool Model::exists(const Value &filter) const { return QueryOne(m_, filter).select("_id").lean().has_value(); }

std::vector<Value> Model::distinct(const std::string &field, const Value &filterIn) const {
    Value filter = prepareFilter(*m_, filterIn);
    std::vector<Value> out;
    guard([&] {
        Op o = open(*m_);
        auto cursor = NV(o, distinct, field, toBson(filter));
        for (auto &&doc : cursor) {
            Value row = fromBson(doc);
            for (const auto &e : row["values"].elements()) out.push_back(e);
        }
    });
    return out;
}

std::vector<Value> Model::aggregate(const Value &pipeline) const {
    mongocxx::pipeline p;
    if (!m_->discKey.empty()) {
        Value match = Value::object();
        match["$match"][m_->discKey] = m_->discValue;
        p.append_stage(toBson(match));
    }
    for (const auto &stage : pipeline.elements()) p.append_stage(toBson(stage));
    std::vector<Value> out;
    guard([&] {
        Op o = open(*m_);
        mongocxx::options::aggregate opts;
        auto cursor = NV(o, aggregate, p, opts);
        for (auto &&doc : cursor) out.push_back(fromBson(doc));
    });
    return out;
}

void Model::createIndexes() const {
    createIndexesNow(*m_);
    m_->indexGen = gGeneration.load();
}

void Model::dropCollection() const {
    if (!gPool) throw Error("nevla: pehle nevla::connect() bulao");
    guard([&] {
        auto entry = gPool->acquire();
        (*entry)[gDb][m_->collection].drop();
    });
    m_->indexGen = 0;  // agle operation par indexes dobara
}

Model Model::discriminator(const std::string &name, const Schema &extra) const {
    auto child = std::make_shared<ModelImpl>();
    child->name = name;
    child->collection = m_->collection;
    child->schema = m_->schema;
    child->schema.add(extra);
    child->discKey = m_->schema.options.discriminatorKey;
    child->discValue = name;
    m_->children[name] = child;
    std::lock_guard<std::mutex> lk(gModelsMu);
    gModels[name] = child;
    return Model(child);
}

Model model(const std::string &name, const Schema &schema, const std::string &collection) {
    auto impl = std::make_shared<ModelImpl>();
    impl->name = name;
    impl->schema = schema;
    impl->collection =
        !collection.empty() ? collection : !schema.options.collection.empty() ? schema.options.collection : pluralize(name);
    std::lock_guard<std::mutex> lk(gModelsMu);
    gModels[name] = impl;
    return Model(impl);
}

Model model(const std::string &name) { return Model(lookup(name)); }

}  // namespace nevla
