// Built-in plugins: softDelete, history, multiTenant; request context (tenant / actor)
#include "nevla/plugins.hpp"

#include <map>
#include <mutex>

#include "nevla/model.hpp"

namespace nevla {

/* ---------------- context ---------------- */
TenantScope::TenantScope(std::string tenant) : saved_(detail::context()) {
    if (tenant.empty()) throw Error("nevla: TenantScope needs a tenant id");
    auto &c = detail::context();
    c.tenant = std::move(tenant);
    c.hasTenant = true;
    c.allTenants = false;
}
TenantScope::~TenantScope() {
    auto &c = detail::context();
    c.tenant = saved_.tenant;
    c.hasTenant = saved_.hasTenant;
    c.allTenants = saved_.allTenants;
}
AllTenantsScope::AllTenantsScope() : saved_(detail::context()) { detail::context().allTenants = true; }
AllTenantsScope::~AllTenantsScope() {
    auto &c = detail::context();
    c.tenant = saved_.tenant;
    c.hasTenant = saved_.hasTenant;
    c.allTenants = saved_.allTenants;
}
ActorScope::ActorScope(std::string actor) : saved_(detail::context()) { detail::context().actor = std::move(actor); }
ActorScope::~ActorScope() { detail::context().actor = saved_.actor; }

std::optional<std::string> currentTenant() {
    const auto &c = detail::context();
    if (!c.hasTenant || c.allTenants) return std::nullopt;
    return c.tenant;
}
std::string currentActor() { return detail::context().actor; }

namespace plugins {
namespace {

std::string option(const Value &opts, const char *key, const char *fallback) {
    const Value &v = opts[key];
    if (v.isNull()) return fallback;
    if (!v.isString() || v.asString().empty())
        throw Error(std::string("nevla: plugin option \"") + key + "\" must be text");
    return v.asString();
}

Value toFilter(const Value &f) {
    if (f.isString()) return formulaFilter(f.asString());
    return f.isObject() ? f : Value::object();
}

// Put a $match in front of an aggregation pipeline
void prependMatch(Value &pipeline, const Value &match) {
    Value out = Value::array();
    out.push_back(Value{{"$match", match}});
    for (auto &st : pipeline.elements()) out.push_back(std::move(st));
    pipeline = std::move(out);
}

const char *kScopedOps[] = {"find",       "findOne",          "countDocuments",    "distinct",   "updateOne",
                            "updateMany", "findOneAndUpdate", "findOneAndReplace", "replaceOne", "findOneAndDelete",
                            "deleteOne",  "deleteMany",       "aggregate"};

/* ---------------- history helpers ---------------- */
thread_local std::map<const Document *, Value> tPending;

Model historyStore(const std::string &collection) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    std::string name = "__nevla_history:" + collection;
    try {
        return model(name);
    } catch (const Error &) {
        return model(name, Schema({}, {.strict = false, .versionKey = ""}), collection);
    }
}

void maskObject(const Schema &s, Value &obj);
void maskField(const Field &f, Value &v) {
    if (f.encrypted_ && !v.isNull()) {
        v = "[encrypted]";
    } else if (f.type == FieldType::Object && f.schema_ && v.isObject()) {
        maskObject(*f.schema_, v);
    } else if (f.type == FieldType::Array && v.isArray()) {
        for (auto &e : v.elements()) {
            if (f.schema_ && e.isObject())
                maskObject(*f.schema_, e);
            else if (f.of_ && f.of_->encrypted_ && !e.isNull())
                e = "[encrypted]";
        }
    }
}
void maskObject(const Schema &s, Value &obj) {
    for (auto &kv : obj.items())
        if (const Field *f = s.field(kv.first)) maskField(*f, kv.second);
}

void writeHistory(const std::string &collection, Value entry) {
    entry["by"] = currentActor().empty() ? Value() : Value(currentActor());
    if (auto t = currentTenant()) entry["tenant"] = *t;
    entry["at"] = Date::now();
    historyStore(collection).create(entry);
}

}  // namespace

void SoftDelete::operator()(Schema &s, const Value &opts) const {
    std::string field = option(opts, "field", "deletedAt");
    s.add(field, DateTime());
    Schema::QueryHook hide = [field](QueryInfo &q) {
        if (q.op == "aggregate") {
            Value &p = q.filter;
            if (p.size() && p.at(0).isObject() && p.at(0).contains("$match")) {
                Value &m = p.at(0)["$match"];
                if (m.contains("$withDeleted")) {
                    m.erase("$withDeleted");
                    return;
                }
                if (m.contains(field)) return;
            }
            prependMatch(p, Value{{field, Value()}});
            return;
        }
        if (q.filter.contains("$withDeleted")) {
            q.filter.erase("$withDeleted");
            return;
        }
        if (!q.filter.contains(field)) q.filter[field] = Value();  // null or missing = not deleted
    };
    for (const char *op : {"find", "findOne", "countDocuments", "distinct", "updateOne", "updateMany",
                           "findOneAndUpdate", "findOneAndReplace", "replaceOne", "aggregate"})
        s.pre(op, hide);
    s.method("softDelete", [field](Document &d, const Value &) {
        d.set(field, Date::now()).save();
        return Value(true);
    });
    s.method("restore", [field](Document &d, const Value &) {
        d.unset(field).save();
        return Value(true);
    });
    s.method("isDeleted", [field](Document &d, const Value &) { return Value(d.has(field)); });
    s.static_("softDelete", [field](const Model &M, const Value &filter) {
        Value set = Value::object();
        set[field] = Date::now();
        return Value(M.updateMany(toFilter(filter), set).modifiedCount);
    });
    s.static_("restore", [field](const Model &M, const Value &filter) {
        Value f = toFilter(filter);
        f["$withDeleted"] = true;
        if (!f.contains(field)) f[field] = Value{{"$ne", Value()}};
        Value unset = Value::object();
        unset["$unset"][field] = "";
        return Value(M.updateMany(f, unset).modifiedCount);
    });
}

void History::operator()(Schema &s, const Value &opts) const {
    std::string coll = option(opts, "collection", "nevla_history");
    s.pre("save", [](Document &d) {
        Model owner = model(d.modelName());
        const Schema &schema = owner.schema();
        Value entry = Value::object();
        if (d.isNew()) {
            Value all = d.toObject();
            maskObject(schema, all);
            entry["op"] = "create";
            entry["changes"] = all;
        } else {
            Value changes = Value::object();
            for (const auto &p : d.modifiedPaths()) {
                const Value *pv = d.data().path(p);
                Value v = pv ? *pv : Value();  // null = removed
                if (const Field *f = schema.path(p)) maskField(*f, v);
                changes[p] = v;
            }
            entry["op"] = "update";
            entry["changes"] = changes;
        }
        tPending[&d] = std::move(entry);
    });
    s.post("save", [coll](Document &d) {
        auto it = tPending.find(&d);
        if (it == tPending.end()) return;
        Value entry = std::move(it->second);
        tPending.erase(it);
        if (entry["op"].asString() == "update" && entry["changes"].empty()) return;
        entry["model"] = d.modelName();
        entry["docId"] = d._id();
        writeHistory(coll, std::move(entry));
    });
    s.post("deleteOne", [coll](Document &d) {
        writeHistory(coll, Value{{"model", d.modelName()}, {"docId", d._id()}, {"op", "delete"}});
    });
    for (const char *op : {"updateOne", "updateMany", "replaceOne", "deleteOne", "deleteMany", "findOneAndUpdate",
                           "findOneAndReplace", "findOneAndDelete"}) {
        s.post(op, Schema::QueryPostHook([coll](QueryInfo &q, Value &result) {
                   bool findAnd = q.op.rfind("findOneAnd", 0) == 0;
                   if (findAnd ? result.isNull()
                               : result["matchedCount"].asInt(0) == 0 && result["deletedCount"].asInt(0) == 0 &&
                                     result["upsertedId"].isNull())
                       return;  // nothing happened
                   Value entry{{"model", q.model}, {"op", q.op}, {"filter", q.filter}};
                   if (q.update) entry["update"] = *q.update;
                   if (findAnd)
                       entry["docId"] = result["_id"];
                   else
                       entry["result"] = result;
                   writeHistory(coll, std::move(entry));
               }));
    }
    s.static_("history", [coll](const Model &M, const Value &id) {
        Value out = Value::array();
        Value filter{{"model", M.name()}};
        if (!id.isNull())
            filter["docId"] = id.isString() && ObjectId::isValid(id.asString()) ? Value(ObjectId(id.asString())) : id;
        for (auto &e : historyStore(coll).find(filter).sort({{"at", 1}, {"_id", 1}}).lean())
            out.push_back(std::move(e));
        return out;
    });
}

void MultiTenant::operator()(Schema &s, const Value &opts) const {
    std::string field = option(opts, "field", "tenantId");
    s.add(field, String().required().index().immutable());
    auto tenant = []() -> std::optional<std::string> {
        const auto &c = detail::context();
        if (c.allTenants) return std::nullopt;
        if (!c.hasTenant)
            throw Error(
                "nevla: no tenant set for a multiTenant model (use nevla::TenantScope or nevla::AllTenantsScope)");
        return c.tenant;
    };
    s.pre("validate", [field, tenant](Document &d) {
        auto t = tenant();
        if (!t) return;
        const Value &cur = d[field];
        if (cur.isNull()) {
            if (d.isNew()) d.set(field, *t);
        } else if (cur.asString() != *t) {
            throw Error("nevla: this document belongs to another tenant");
        }
    });
    Schema::QueryHook scope = [field, tenant](QueryInfo &q) {
        auto t = tenant();
        if (!t) return;
        if (q.op == "aggregate") {
            prependMatch(q.filter, Value{{field, *t}});
            return;
        }
        q.filter[field] = *t;  // overrides anything the caller put there
        if ((q.op == "replaceOne" || q.op == "findOneAndReplace") && q.update) (*q.update)[field] = *t;
    };
    for (const char *op : kScopedOps) s.pre(op, scope);
}

}  // namespace plugins
}  // namespace nevla
