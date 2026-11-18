// In-memory database for connect("memory://..."): a MongoDB-compatible subset that runs inside the process.
// Used for unit tests and demos. Checked against a real MongoDB by the differential tests in tests/test.cc.
#include "memory.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace nevla::mem {
namespace {

/* ======================= storage ======================= */
struct Index {
    Value keys;
    std::string name;
    bool unique = false, sparse = false;
    Value options;
};
struct Coll {
    std::vector<Value> docs;
    std::vector<Index> indexes;
    // unique index name -> key -> how many documents have it (the _id index is "_id_")
    std::map<std::string, std::unordered_map<std::string, int>> uniq;
    std::string type = "collection";
    Value options = Value::object();
    Value validator;  // $jsonSchema (syncValidator)
    std::string validationLevel = "strict";
};
struct Db {
    std::map<std::string, Coll> colls;
};

std::recursive_mutex gMu;
std::map<std::string, Db> gDbs;
thread_local int tTxDepth = 0;

using Lock = std::lock_guard<std::recursive_mutex>;

[[noreturn]] void unsupported(const std::string &what) {
    throw Error("nevla memory database: " + what + " is not supported (use a real MongoDB for this)");
}

Coll &coll(const std::string &db, const std::string &name) { return gDbs[db].colls[name]; }

/* ======================= values ======================= */
int rank(const Value *v) {
    if (!v) return 0;  // missing
    switch (v->type()) {
        case Value::Type::Null:
            return 1;
        case Value::Type::Int:
        case Value::Type::Double:
            return 2;
        case Value::Type::String:
            return 3;
        case Value::Type::Object:
            return 4;
        case Value::Type::Array:
            return 5;
        case Value::Type::ObjectId:
            return 7;
        case Value::Type::Bool:
            return 8;
        case Value::Type::Date:
            return 9;
    }
    return 10;
}

int cmp(const Value *a, const Value *b);
int cmpVal(const Value &a, const Value &b) { return cmp(&a, &b); }

int cmp(const Value *a, const Value *b) {
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    if (!a || a->isNull()) return 0;
    switch (a->type()) {
        case Value::Type::Int:
        case Value::Type::Double: {
            if (a->isInt() && b->isInt()) return a->asInt() < b->asInt() ? -1 : a->asInt() > b->asInt();
            double x = a->asDouble(), y = b->asDouble();
            return x < y ? -1 : x > y;
        }
        case Value::Type::String: {
            int c = a->asString().compare(b->asString());
            return c < 0 ? -1 : c > 0;
        }
        case Value::Type::Object: {
            const auto &x = a->items(), &y = b->items();
            for (size_t i = 0; i < x.size() && i < y.size(); i++) {
                int c = cmpVal(x[i].second, y[i].second);  // Mongo compares type, field name, value
                if (x[i].first != y[i].first) return x[i].first < y[i].first ? -1 : 1;
                if (c) return c;
            }
            return x.size() < y.size() ? -1 : x.size() > y.size();
        }
        case Value::Type::Array: {
            const auto &x = a->elements(), &y = b->elements();
            for (size_t i = 0; i < x.size() && i < y.size(); i++)
                if (int c = cmpVal(x[i], y[i])) return c;
            return x.size() < y.size() ? -1 : x.size() > y.size();
        }
        case Value::Type::ObjectId: {
            int c = a->asObjectId().str().compare(b->asObjectId().str());
            return c < 0 ? -1 : c > 0;
        }
        case Value::Type::Bool:
            return a->asBool() == b->asBool() ? 0 : a->asBool() ? 1 : -1;
        case Value::Type::Date:
            return a->asDate().ms < b->asDate().ms ? -1 : a->asDate().ms > b->asDate().ms;
        default:
            return 0;
    }
}

bool eq(const Value &a, const Value &b) { return cmpVal(a, b) == 0; }

// Same type and value (1 vs 1.0 differ): decides "modified"
bool identical(const Value &a, const Value &b) {
    if (a.type() != b.type()) return false;
    if (a.isArray()) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (!identical(a.at(i), b.at(i))) return false;
        return true;
    }
    if (a.isObject()) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.items().size(); i++)
            if (a.items()[i].first != b.items()[i].first || !identical(a.items()[i].second, b.items()[i].second))
                return false;
        return true;
    }
    return a == b;
}

// Group / distinct key: numbers normalised (1 == 1.0)
std::string keyOf(const Value &v) {
    switch (v.type()) {
        case Value::Type::Int:
        case Value::Type::Double: {
            std::ostringstream o;
            o.precision(17);
            o << "n:" << v.asDouble();
            return o.str();
        }
        case Value::Type::Array: {
            std::string s = "[";
            for (const auto &e : v.elements()) s += keyOf(e) + ",";
            return s + "]";
        }
        case Value::Type::Object: {
            std::string s = "{";
            for (const auto &kv : v.items()) s += kv.first + ":" + keyOf(kv.second) + ",";
            return s + "}";
        }
        default:
            return std::to_string(static_cast<int>(v.type())) + ":" + v.toJson();
    }
}

std::string typeName(const Value *v) {
    if (!v) return "missing";
    switch (v->type()) {
        case Value::Type::Null:
            return "null";
        case Value::Type::Bool:
            return "bool";
        case Value::Type::Int:
            return v->asInt() >= INT32_MIN && v->asInt() <= INT32_MAX ? "int" : "long";
        case Value::Type::Double:
            return "double";
        case Value::Type::String:
            return "string";
        case Value::Type::Date:
            return "date";
        case Value::Type::ObjectId:
            return "objectId";
        case Value::Type::Array:
            return "array";
        case Value::Type::Object:
            return "object";
    }
    return "unknown";
}

bool isOps(const Value &v) {
    if (!v.isObject() || v.empty()) return false;
    for (const auto &kv : v.items())
        if (kv.first.empty() || kv.first[0] != '$') return false;
    return true;
}

std::vector<std::string> split(const std::string &path) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); i++)
        if (i == path.size() || path[i] == '.') {
            out.push_back(path.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

bool numeric(const std::string &s) {
    return !s.empty() && s.size() < 10 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

/* ---------------- reading paths (query semantics) ---------------- */
// The values a path reaches, plus how many places the path ends in a missing field.
// {items: [{qty: 3}, {}]} for "items.qty" -> vals [3], missing 1 (so it matches {items.qty: null});
// {items: []} -> nothing at all (so it does NOT match null, like MongoDB).
struct Cands {
    std::vector<Value> vals;
    int missing = 0;
    bool empty() const { return vals.empty(); }
};
void collect(const Value &cur, const std::vector<std::string> &segs, size_t i, Cands &out) {
    if (i == segs.size()) {
        out.vals.push_back(cur);
        return;
    }
    if (cur.isObject()) {
        if (const Value *n = cur.find(segs[i]))
            collect(*n, segs, i + 1, out);
        else
            out.missing++;
    } else if (cur.isArray()) {
        if (numeric(segs[i])) {
            size_t idx = std::stoul(segs[i]);
            if (idx < cur.size())
                collect(cur.at(idx), segs, i + 1, out);
            else
                out.missing++;
        }
        bool docs = false;
        for (const auto &e : cur.elements())
            if (e.isObject()) {
                docs = true;
                if (!numeric(segs[i])) collect(e, segs, i, out);
            }
        (void)docs;
    } else {
        out.missing++;  // a scalar in the middle of the path
    }
}
Cands candidates(const Value &doc, const std::string &path) {
    Cands out;
    collect(doc, split(path), 0, out);
    return out;
}

/* ======================= query matching ======================= */
bool matchCond(const Cands &cands, const Value &cond, const Value &whole);

Cands one(const Value &v) {
    Cands c;
    c.vals.push_back(v);
    return c;
}

bool eqMatch(const Cands &cands, const Value &x) {
    if (x.isNull() && cands.missing > 0) return true;
    for (const auto &c : cands.vals) {
        if (eq(c, x)) return true;
        if (c.isArray())
            for (const auto &e : c.elements())
                if (eq(e, x)) return true;
    }
    return false;
}

template <class P>
bool cmpMatch(const Cands &cands, const Value &x, P pred, bool orEqualNull) {
    if (x.isNull()) return orEqualNull && eqMatch(cands, x);
    auto ok = [&](const Value &v) { return rank(&v) == rank(&x) && pred(cmpVal(v, x)); };
    for (const auto &c : cands.vals) {
        if (ok(c)) return true;
        if (c.isArray())
            for (const auto &e : c.elements())
                if (ok(e)) return true;
    }
    return false;
}

bool regexMatch(const Cands &cands, const std::string &pattern, const std::string &opts) {
    auto flags = std::regex::ECMAScript;
    for (char o : opts) {
        if (o == 'i')
            flags |= std::regex::icase;
        else if (o == 'm')
            flags |= std::regex::multiline;
        else if (o != 's' && o != 'x' && o != 'u')
            unsupported("regex option " + std::string(1, o));
    }
    std::regex re;
    try {
        re = std::regex(pattern, flags);
    } catch (const std::regex_error &) {
        throw Error("nevla memory database: invalid regular expression: " + pattern);
    }
    for (const auto &c : cands.vals) {
        if (c.isString() && std::regex_search(c.asString(), re)) return true;
        if (c.isArray())
            for (const auto &e : c.elements())
                if (e.isString() && std::regex_search(e.asString(), re)) return true;
    }
    return false;
}

bool truthy(const std::optional<Value> &v) {
    if (!v || v->isNull()) return false;
    if (v->isBool()) return v->asBool();
    if (v->isNumber()) return v->asDouble() != 0;
    return true;
}

std::optional<Value> evalTop(const Value &expr, const Value &doc);

bool applyOp(const std::string &op, const Value &arg, const Cands &cands, const Value &whole) {
    if (op == "$eq") return eqMatch(cands, arg);
    if (op == "$ne") return !eqMatch(cands, arg);
    if (op == "$gt") return cmpMatch(cands, arg, [](int c) { return c > 0; }, false);
    if (op == "$gte") return cmpMatch(cands, arg, [](int c) { return c >= 0; }, true);
    if (op == "$lt") return cmpMatch(cands, arg, [](int c) { return c < 0; }, false);
    if (op == "$lte") return cmpMatch(cands, arg, [](int c) { return c <= 0; }, true);
    if (op == "$in" || op == "$nin") {
        if (!arg.isArray()) throw Error("nevla memory database: " + op + " needs an array");
        bool any = false;
        for (const auto &x : arg.elements()) any |= eqMatch(cands, x);
        return op == "$in" ? any : !any;
    }
    if (op == "$exists") return !cands.empty() == truthy(arg);
    if (op == "$size") {
        for (const auto &c : cands.vals)
            if (c.isArray() && static_cast<int64_t>(c.size()) == arg.asInt(-1)) return true;
        return false;
    }
    if (op == "$all") {
        if (!arg.isArray() || arg.empty()) return false;
        for (const auto &x : arg.elements())
            if (!eqMatch(cands, x)) return false;
        return true;
    }
    if (op == "$elemMatch") {
        bool scalar = isOps(arg);
        for (const auto &c : cands.vals) {
            if (!c.isArray()) continue;
            for (const auto &e : c.elements())
                if (scalar ? matchCond(one(e), arg, arg) : (e.isObject() && matches(e, arg))) return true;
        }
        return false;
    }
    if (op == "$regex") return regexMatch(cands, arg.str(), whole["$options"].str());
    if (op == "$options") return true;
    if (op == "$not") return !matchCond(cands, arg, arg);
    if (op == "$type") {
        std::set<std::string> want;
        auto add = [&](const Value &t) {
            std::string n = t.str();
            if (n == "number")
                want.insert({"int", "long", "double"});
            else
                want.insert(n);
        };
        if (arg.isArray())
            for (const auto &t : arg.elements()) add(t);
        else
            add(arg);
        for (const auto &c : cands.vals) {
            if (want.count(typeName(&c))) return true;
            if (c.isArray())
                for (const auto &e : c.elements())
                    if (want.count(typeName(&e))) return true;
        }
        return false;
    }
    if (op == "$mod") {
        if (!arg.isArray() || arg.size() != 2) throw Error("nevla memory database: $mod needs [divisor, remainder]");
        int64_t d = arg.at(0).asInt(), r = arg.at(1).asInt();
        if (d == 0) throw Error("nevla memory database: $mod by 0");
        for (const auto &c : cands.vals)
            if (c.isNumber() && static_cast<int64_t>(c.asDouble()) % d == r) return true;
        return false;
    }
    unsupported("query operator " + op);
}

bool matchCond(const Cands &cands, const Value &cond, const Value &) {
    if (isOps(cond)) {
        for (const auto &kv : cond.items())
            if (!applyOp(kv.first, kv.second, cands, cond)) return false;
        return true;
    }
    return eqMatch(cands, cond);
}

}  // namespace

bool matches(const Value &doc, const Value &filter) {
    for (const auto &kv : filter.items()) {
        const std::string &k = kv.first;
        if (k == "$and" || k == "$or" || k == "$nor") {
            if (!kv.second.isArray() || kv.second.empty())
                throw Error("nevla memory database: " + k + " needs a non-empty array");
            bool all = true, any = false;
            for (const auto &f : kv.second.elements()) {
                bool m = matches(doc, f);
                all &= m;
                any |= m;
            }
            if (k == "$and" && !all) return false;
            if (k == "$or" && !any) return false;
            if (k == "$nor" && any) return false;
        } else if (k == "$expr") {
            if (!truthy(evalTop(kv.second, doc))) return false;
        } else if (k == "$comment") {
            continue;
        } else if (!k.empty() && k[0] == '$') {
            unsupported("query operator " + k);
        } else if (!matchCond(candidates(doc, k), kv.second, kv.second)) {
            return false;
        }
    }
    return true;
}

int compareValues(const Value &a, const Value &b) { return cmpVal(a, b); }

namespace {

/* ======================= sort / projection ======================= */
// Sort key of a document for one field, like MongoDB: arrays sort by their smallest element (ascending) or
// largest (descending); missing = null; an empty array sorts before null.
struct SortKey {
    int cls;  // 0 = empty array, 1 = a value (null for missing)
    Value v;
};
SortKey sortKey(const Value &doc, const std::string &path, int dir) {
    auto c = candidates(doc, path);
    std::optional<Value> best;
    bool sawEmptyArray = false;
    auto consider = [&](const Value &v) {
        if (!best || (dir > 0 ? cmpVal(v, *best) < 0 : cmpVal(v, *best) > 0)) best = v;
    };
    for (const auto &v : c.vals) {
        if (v.isArray() && v.empty())
            sawEmptyArray = true;
        else if (v.isArray())
            for (const auto &e : v.elements()) consider(e);
        else
            consider(v);
    }
    if (c.missing) consider(Value());  // a missing value sorts as null
    if (!best && sawEmptyArray) return {0, Value()};
    return {1, best ? *best : Value()};
}

void sortDocs(std::vector<Value> &docs, const Value &spec) {
    if (!spec.isObject() || spec.empty()) return;
    std::stable_sort(docs.begin(), docs.end(), [&](const Value &a, const Value &b) {
        for (const auto &kv : spec.items()) {
            int dir = kv.second.asInt(1) < 0 ? -1 : 1;
            SortKey x = sortKey(a, kv.first, dir), y = sortKey(b, kv.first, dir);
            int c = x.cls != y.cls ? (x.cls < y.cls ? -1 : 1) : cmpVal(x.v, y.v);
            if (c) return dir > 0 ? c < 0 : c > 0;
        }
        return false;
    });
}

void setPathRaw(Value &doc, const std::vector<std::string> &segs, size_t i, Value v);

// Inclusion projection of one path (arrays of documents keep one projected document per element)
void includePath(const Value &src, Value &dst, const std::vector<std::string> &segs, size_t i) {
    if (!src.isObject()) return;
    const Value *n = src.find(segs[i]);
    if (!n) return;
    if (i + 1 == segs.size()) {
        dst[segs[i]] = *n;
        return;
    }
    if (n->isObject()) {
        Value &d = dst[segs[i]];
        if (!d.isObject()) d = Value::object();
        includePath(*n, d, segs, i + 1);
    } else if (n->isArray()) {
        Value &d = dst[segs[i]];
        Value built = d.isArray() ? d : Value::array();
        size_t k = 0;
        for (const auto &e : n->elements()) {
            if (!e.isObject()) continue;
            Value sub = k < built.size() ? built.at(k) : Value::object();
            includePath(e, sub, segs, i + 1);
            if (k < built.size())
                built.at(k) = sub;
            else
                built.push_back(sub);
            k++;
        }
        d = built;
    }
}
void copyPath(const Value &from, Value &to, const std::string &path) { includePath(from, to, split(path), 0); }

Value project(const Value &doc, const Value &proj) {
    if (!proj.isObject() || proj.empty()) return doc;
    bool inclusion = false;
    for (const auto &kv : proj.items())
        if (kv.first != "_id" && truthy(kv.second)) inclusion = true;
    bool keepId = !(proj.contains("_id") && !truthy(proj["_id"]));
    if (inclusion) {
        Value out = Value::object();
        if (keepId && doc.contains("_id")) out["_id"] = doc["_id"];
        for (const auto &kv : proj.items())
            if (kv.first != "_id" && truthy(kv.second)) copyPath(doc, out, kv.first);
        return out;
    }
    Value out = doc;
    for (const auto &kv : proj.items())
        if (!truthy(kv.second)) out.erasePath(kv.first);
    return out;
}

/* ======================= aggregation expressions ======================= */
struct Ctx {
    const Value *root;
    const Value *current;
    std::map<std::string, Value> vars;
    Date now;
};

std::optional<Value> ev(const Value &e, Ctx &c);

std::optional<Value> fieldValue(const Value &cur, const std::vector<std::string> &segs, size_t i) {
    if (i == segs.size()) return cur;
    if (cur.isObject()) {
        const Value *n = cur.find(segs[i]);
        if (!n) return std::nullopt;
        return fieldValue(*n, segs, i + 1);
    }
    if (cur.isArray()) {  // "$items.qty" -> [qty of every item]
        Value out = Value::array();
        for (const auto &el : cur.elements())
            if (auto v = fieldValue(el, segs, i)) out.push_back(*v);
        return out;
    }
    return std::nullopt;
}

std::vector<Value> argList(const Value &e) {
    if (e.isArray()) return e.elements();
    return {e};
}

Value orNull(const std::optional<Value> &v) { return v ? *v : Value(); }

Value numResult(double d, bool asInt) { return asInt ? Value(static_cast<int64_t>(d)) : Value(d); }

// Offsets of the time zones nevla's memory database knows (fixed offsets only, no DST zones)
int64_t tzOffsetMs(const std::optional<Value> &tz) {
    if (!tz || tz->isNull()) return 0;
    std::string z = tz->str();
    static const std::map<std::string, int> known = {{"UTC", 0},
                                                     {"GMT", 0},
                                                     {"Etc/UTC", 0},
                                                     {"Z", 0},
                                                     {"Asia/Kolkata", 330},
                                                     {"Asia/Calcutta", 330},
                                                     {"Asia/Dhaka", 360},
                                                     {"Asia/Karachi", 300},
                                                     {"Asia/Kathmandu", 345},
                                                     {"Asia/Dubai", 240},
                                                     {"Asia/Singapore", 480},
                                                     {"Asia/Shanghai", 480},
                                                     {"Asia/Tokyo", 540},
                                                     {"Asia/Seoul", 540},
                                                     {"Asia/Colombo", 330},
                                                     {"Asia/Bangkok", 420},
                                                     {"Asia/Jakarta", 420},
                                                     {"Asia/Riyadh", 180}};
    if (auto it = known.find(z); it != known.end()) return int64_t(it->second) * 60000;
    if (z.size() == 6 && (z[0] == '+' || z[0] == '-') && z[3] == ':') {
        int64_t m = std::stoi(z.substr(1, 2)) * 60 + std::stoi(z.substr(4, 2));
        return (z[0] == '-' ? -m : m) * 60000;
    }
    unsupported("time zone \"" + z + "\" (memory database knows fixed-offset zones like Asia/Kolkata or \"+05:30\")");
}

// days since 1970-01-01 <-> civil date (Howard Hinnant's algorithm)
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
void civilFromDays(int64_t z, int64_t &y, unsigned &m, unsigned &d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}
int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

struct Parts {
    int64_t year;
    unsigned month, day;
    int64_t hour, minute, second, ms, dow;  // dow 1 = Sunday
};
Parts parts(int64_t localMs) {
    int64_t days = floorDiv(localMs, 86400000), rest = localMs - days * 86400000;
    Parts p{};
    civilFromDays(days, p.year, p.month, p.day);
    p.hour = rest / 3600000;
    p.minute = rest / 60000 % 60;
    p.second = rest / 1000 % 60;
    p.ms = rest % 1000;
    p.dow = ((days % 7) + 7 + 4) % 7 + 1;  // 1970-01-01 was a Thursday
    return p;
}

int64_t unitMs(const std::string &u) {
    if (u == "millisecond") return 1;
    if (u == "second") return 1000;
    if (u == "minute") return 60000;
    if (u == "hour") return 3600000;
    if (u == "day") return 86400000;
    if (u == "week") return 7 * 86400000LL;
    return 0;
}

int64_t dateTrunc(int64_t ms, const std::string &unit, int64_t bin, int64_t off) {
    int64_t local = ms + off;
    const int64_t ref = daysFromCivil(2000, 1, 1) * 86400000LL;  // MongoDB bins start at 2000-01-01
    if (unit == "month" || unit == "quarter" || unit == "year") {
        Parts p = parts(local);
        int64_t months = (p.year - 2000) * 12 + (p.month - 1);
        int64_t step = unit == "month" ? bin : unit == "quarter" ? 3 * bin : 12 * bin;
        int64_t start = floorDiv(months, step) * step;
        int64_t y = 2000 + floorDiv(start, 12);
        unsigned m = static_cast<unsigned>(start - floorDiv(start, 12) * 12) + 1;
        return daysFromCivil(y, m, 1) * 86400000LL - off;
    }
    int64_t u = unitMs(unit);
    if (!u) throw Error("nevla memory database: $dateTrunc unit \"" + unit + "\" is not valid");
    int64_t base = ref;
    if (unit == "week") base = daysFromCivil(2000, 1, 2) * 86400000LL;  // first Sunday of 2000
    int64_t size = u * bin;
    return base + floorDiv(local - base, size) * size - off;
}

std::optional<Value> evalOp(const std::string &op, const Value &arg, Ctx &c);
std::optional<Value> ev(const Value &e, Ctx &c);

std::optional<Value> evalTop(const Value &expr, const Value &doc) {
    Ctx c{&doc, &doc, {}, Date::now()};
    return ev(expr, c);
}

std::optional<Value> ev(const Value &e, Ctx &c) {
    if (e.isString()) {
        const std::string &s = e.asString();
        if (s.size() > 1 && s[0] == '$' && s[1] == '$') {
            std::string name = s.substr(2), rest;
            if (auto dot = name.find('.'); dot != std::string::npos) {
                rest = name.substr(dot + 1);
                name = name.substr(0, dot);
            }
            Value base;
            if (name == "ROOT")
                base = *c.root;
            else if (name == "CURRENT")
                base = *c.current;
            else if (name == "NOW")
                base = c.now;
            else if (name == "REMOVE")
                return std::nullopt;
            else if (auto it = c.vars.find(name); it != c.vars.end())
                base = it->second;
            else
                throw Error("nevla memory database: unknown variable $$" + name);
            if (rest.empty()) return base;
            return fieldValue(base, split(rest), 0);
        }
        if (!s.empty() && s[0] == '$') return fieldValue(*c.current, split(s.substr(1)), 0);
        return e;
    }
    if (e.isArray()) {
        Value out = Value::array();
        for (const auto &x : e.elements()) out.push_back(orNull(ev(x, c)));
        return out;
    }
    if (e.isObject()) {
        if (e.size() == 1 && !e.items()[0].first.empty() && e.items()[0].first[0] == '$')
            return evalOp(e.items()[0].first, e.items()[0].second, c);
        Value out = Value::object();
        for (const auto &kv : e.items())
            if (auto v = ev(kv.second, c)) out[kv.first] = *v;
        return out;
    }
    return e;
}

std::optional<Value> evalOp(const std::string &op, const Value &arg, Ctx &c) {
    auto args = [&]() {
        std::vector<std::optional<Value>> out;
        for (const auto &a : argList(arg)) out.push_back(ev(a, c));
        return out;
    };
    auto need = [&](size_t n, const std::vector<std::optional<Value>> &a) {
        if (a.size() != n) throw Error("nevla memory database: " + op + " takes " + std::to_string(n) + " arguments");
    };
    auto nullish = [](const std::optional<Value> &v) { return !v || v->isNull(); };

    if (op == "$literal") return arg;
    // ---- arithmetic
    if (op == "$add" || op == "$multiply") {
        auto a = args();
        bool allInt = true, date = false;
        double acc = op == "$add" ? 0 : 1;
        int64_t dateMs = 0;
        for (auto &v : a) {
            if (nullish(v)) return Value();
            if (v->isDate() && op == "$add") {
                date = true;
                dateMs = v->asDate().ms;
                continue;
            }
            if (!v->isNumber()) throw Error("nevla memory database: " + op + " only supports numeric types");
            allInt &= v->isInt();
            acc = op == "$add" ? acc + v->asDouble() : acc * v->asDouble();
        }
        if (date) return Value(Date{dateMs + static_cast<int64_t>(acc)});
        return numResult(acc, allInt && std::fabs(acc) < 9e15);
    }
    if (op == "$subtract") {
        auto a = args();
        need(2, a);
        if (nullish(a[0]) || nullish(a[1])) return Value();
        if (a[0]->isDate() && a[1]->isDate()) return Value(a[0]->asDate().ms - a[1]->asDate().ms);
        if (a[0]->isDate() && a[1]->isNumber()) return Value(Date{a[0]->asDate().ms - a[1]->asInt()});
        if (!a[0]->isNumber() || !a[1]->isNumber())
            throw Error("nevla memory database: $subtract needs numbers or dates");
        return numResult(a[0]->asDouble() - a[1]->asDouble(), a[0]->isInt() && a[1]->isInt());
    }
    if (op == "$divide") {
        auto a = args();
        need(2, a);
        if (nullish(a[0]) || nullish(a[1])) return Value();
        if (a[1]->asDouble() == 0) throw Error("nevla memory database: can't $divide by zero");
        return Value(a[0]->asDouble() / a[1]->asDouble());
    }
    if (op == "$mod") {
        auto a = args();
        need(2, a);
        if (nullish(a[0]) || nullish(a[1])) return Value();
        if (a[1]->asDouble() == 0) throw Error("nevla memory database: can't $mod by zero");
        return numResult(std::fmod(a[0]->asDouble(), a[1]->asDouble()), a[0]->isInt() && a[1]->isInt());
    }
    if (op == "$abs" || op == "$ceil" || op == "$floor" || op == "$sqrt" || op == "$exp" || op == "$ln" ||
        op == "$log10") {
        auto a = args();
        need(1, a);
        if (nullish(a[0])) return Value();
        double x = a[0]->asDouble();
        if (op == "$abs") return a[0]->isInt() ? Value(std::llabs(a[0]->asInt())) : Value(std::fabs(x));
        if (op == "$ceil") return a[0]->isInt() ? *a[0] : Value(std::ceil(x));
        if (op == "$floor") return a[0]->isInt() ? *a[0] : Value(std::floor(x));
        if (op == "$sqrt") return Value(std::sqrt(x));
        if (op == "$exp") return Value(std::exp(x));
        if (op == "$ln") return Value(std::log(x));
        return Value(std::log10(x));
    }
    if (op == "$pow" || op == "$log") {
        auto a = args();
        need(2, a);
        if (nullish(a[0]) || nullish(a[1])) return Value();
        if (op == "$log") return Value(std::log(a[0]->asDouble()) / std::log(a[1]->asDouble()));
        double r = std::pow(a[0]->asDouble(), a[1]->asDouble());
        return numResult(r, a[0]->isInt() && a[1]->isInt() && a[1]->asInt() >= 0 && std::fabs(r) < 9e15);
    }
    if (op == "$round" || op == "$trunc") {
        auto a = args();
        if (a.empty() || a.size() > 2) throw Error("nevla memory database: " + op + " takes 1 or 2 arguments");
        if (nullish(a[0])) return Value();
        int64_t places = a.size() == 2 ? a[1]->asInt() : 0;
        if (a[0]->isInt() && places >= 0) return *a[0];
        double f = std::pow(10.0, static_cast<double>(places)), x = a[0]->asDouble() * f;
        double r;
        if (op == "$trunc") {
            r = std::trunc(x);
        } else {  // round half to even, like MongoDB
            r = std::floor(x);
            double diff = x - r;
            if (diff > 0.5 + 1e-9 || (std::fabs(diff - 0.5) <= 1e-9 && std::fmod(r, 2.0) != 0)) r += 1;
        }
        r /= f;
        return a[0]->isInt() ? Value(static_cast<int64_t>(r)) : Value(r);
    }
    // ---- comparison / logic
    if (op == "$eq" || op == "$ne" || op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte" || op == "$cmp") {
        auto a = args();
        need(2, a);
        int r = cmp(a[0] ? &*a[0] : nullptr, a[1] ? &*a[1] : nullptr);
        if (op == "$cmp") return Value(r);
        bool b = op == "$eq"    ? r == 0
                 : op == "$ne"  ? r != 0
                 : op == "$gt"  ? r > 0
                 : op == "$gte" ? r >= 0
                 : op == "$lt"  ? r < 0
                                : r <= 0;
        return Value(b);
    }
    if (op == "$and" || op == "$or") {
        bool isAnd = op == "$and";
        for (const auto &x : argList(arg)) {
            bool t = truthy(ev(x, c));
            if (isAnd && !t) return Value(false);
            if (!isAnd && t) return Value(true);
        }
        return Value(isAnd);
    }
    if (op == "$not") {
        auto a = args();
        need(1, a);
        return Value(!truthy(a[0]));
    }
    if (op == "$cond") {
        Value i, t, f;
        if (arg.isArray()) {
            if (arg.size() != 3) throw Error("nevla memory database: $cond takes 3 arguments");
            i = arg.at(0), t = arg.at(1), f = arg.at(2);
        } else {
            i = arg["if"], t = arg["then"], f = arg["else"];
        }
        return truthy(ev(i, c)) ? ev(t, c) : ev(f, c);
    }
    if (op == "$ifNull") {
        auto list = argList(arg);
        for (size_t k = 0; k + 1 < list.size(); k++) {
            auto v = ev(list[k], c);
            if (!nullish(v)) return v;
        }
        return list.empty() ? Value() : orNull(ev(list.back(), c));
    }
    // ---- arrays
    if (op == "$in") {
        auto a = args();
        need(2, a);
        if (!a[1] || !a[1]->isArray()) throw Error("nevla memory database: $in needs an array as second argument");
        for (const auto &x : a[1]->elements())
            if (a[0] && eq(x, *a[0])) return Value(true);
        return Value(false);
    }
    if (op == "$size") {
        auto a = args();
        need(1, a);
        if (!a[0] || !a[0]->isArray()) throw Error("nevla memory database: $size needs an array");
        return Value(static_cast<int64_t>(a[0]->size()));
    }
    if (op == "$arrayElemAt") {
        auto a = args();
        need(2, a);
        if (nullish(a[0])) return Value();
        int64_t i = a[1]->asInt(), n = static_cast<int64_t>(a[0]->size());
        if (i < 0) i += n;
        if (i < 0 || i >= n) return std::nullopt;
        return a[0]->at(static_cast<size_t>(i));
    }
    if (op == "$indexOfArray") {
        auto a = args();
        if (a.size() < 2) throw Error("nevla memory database: $indexOfArray takes 2 arguments");
        if (nullish(a[0])) return Value();
        for (size_t i = 0; i < a[0]->size(); i++)
            if (a[1] && eq(a[0]->at(i), *a[1])) return Value(static_cast<int64_t>(i));
        return Value(-1);
    }
    if (op == "$slice") {
        auto a = args();
        if (a.size() < 2 || a.size() > 3) throw Error("nevla memory database: $slice takes 2 or 3 arguments");
        if (nullish(a[0])) return Value();
        const auto &arr = a[0]->elements();
        int64_t n = static_cast<int64_t>(arr.size()), start = 0, count;
        if (a.size() == 2) {
            count = a[1]->asInt();
            if (count < 0) {
                start = std::max<int64_t>(0, n + count);
                count = -count;
            }
        } else {
            start = a[1]->asInt();
            if (start < 0) start = std::max<int64_t>(0, n + start);
            count = a[2]->asInt();
        }
        Value out = Value::array();
        for (int64_t i = start; i < n && i < start + count; i++) out.push_back(arr[static_cast<size_t>(i)]);
        return out;
    }
    if (op == "$map" || op == "$filter") {
        auto input = ev(arg["input"], c);
        if (nullish(input)) return Value();
        if (!input->isArray()) throw Error("nevla memory database: " + op + " input must be an array");
        std::string as = arg["as"].isString() ? arg["as"].asString() : "this";
        Value out = Value::array();
        auto saved = c.vars;
        int64_t limit = arg.contains("limit") ? orNull(ev(arg["limit"], c)).asInt(0) : 0;
        for (const auto &el : input->elements()) {
            c.vars[as] = el;
            if (op == "$map") {
                out.push_back(orNull(ev(arg["in"], c)));
            } else if (truthy(ev(arg["cond"], c))) {
                out.push_back(el);
                if (limit > 0 && static_cast<int64_t>(out.size()) >= limit) break;
            }
        }
        c.vars = saved;
        return out;
    }
    if (op == "$let") {
        auto saved = c.vars;
        std::map<std::string, Value> add;
        for (const auto &kv : arg["vars"].items()) add[kv.first] = orNull(ev(kv.second, c));
        for (auto &kv : add) c.vars[kv.first] = kv.second;
        auto r = ev(arg["in"], c);
        c.vars = saved;
        return r;
    }
    if (op == "$sum" || op == "$avg" || op == "$min" || op == "$max") {
        auto a = args();
        std::vector<Value> vals;
        if (a.size() == 1 && a[0] && a[0]->isArray())
            vals = a[0]->elements();
        else
            for (auto &v : a)
                if (v) vals.push_back(*v);
        if (op == "$sum" || op == "$avg") {
            double s = 0;
            bool allInt = true;
            int64_t n = 0;
            for (const auto &v : vals)
                if (v.isNumber()) {
                    s += v.asDouble();
                    allInt &= v.isInt();
                    n++;
                }
            if (op == "$sum") return numResult(s, allInt);
            return n ? Value(s / n) : Value();
        }
        std::optional<Value> best;
        for (const auto &v : vals)
            if (!v.isNull() && (!best || (op == "$min" ? cmpVal(v, *best) < 0 : cmpVal(v, *best) > 0))) best = v;
        return best ? *best : Value();
    }
    if (op == "$mergeObjects") {
        Value out = Value::object();
        for (auto &v : args())
            if (v && v->isObject())
                for (const auto &kv : v->items()) out[kv.first] = kv.second;
        return out;
    }
    // ---- strings
    if (op == "$concat") {
        std::string s;
        for (auto &v : args()) {
            if (nullish(v)) return Value();
            if (!v->isString()) throw Error("nevla memory database: $concat only supports strings");
            s += v->asString();
        }
        return Value(s);
    }
    auto cps = [](const std::string &s) {  // UTF-8 code points
        std::vector<std::string> out;
        for (size_t i = 0; i < s.size();) {
            unsigned char ch = static_cast<unsigned char>(s[i]);
            size_t len = ch < 0x80 ? 1 : (ch >> 5) == 6 ? 2 : (ch >> 4) == 14 ? 3 : 4;
            out.push_back(s.substr(i, len));
            i += len;
        }
        return out;
    };
    if (op == "$toLower" || op == "$toUpper") {
        auto a = args();
        need(1, a);
        if (nullish(a[0])) return Value("");
        std::string s = a[0]->str();
        for (auto &ch : s)
            ch = op == "$toLower" ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
                                  : static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return Value(s);
    }
    if (op == "$strLenCP") {
        auto a = args();
        need(1, a);
        if (!a[0] || !a[0]->isString()) throw Error("nevla memory database: $strLenCP needs a string");
        return Value(static_cast<int64_t>(cps(a[0]->asString()).size()));
    }
    if (op == "$substrCP") {
        auto a = args();
        need(3, a);
        if (nullish(a[0])) return Value("");
        auto p = cps(a[0]->str());
        std::string s;
        for (int64_t i = a[1]->asInt(); i < static_cast<int64_t>(p.size()) && i < a[1]->asInt() + a[2]->asInt(); i++)
            if (i >= 0) s += p[static_cast<size_t>(i)];
        return Value(s);
    }
    if (op == "$trim") {
        auto in = ev(arg["input"], c);
        if (nullish(in)) return Value();
        std::string s = in->str();
        size_t b = s.find_first_not_of(" \t\n\r"), e = s.find_last_not_of(" \t\n\r");
        return Value(b == std::string::npos ? std::string() : s.substr(b, e - b + 1));
    }
    // ---- conversion / type
    if (op == "$toString") {
        auto a = args();
        need(1, a);
        if (nullish(a[0])) return Value();
        return Value(a[0]->str());
    }
    if (op == "$toInt" || op == "$toLong" || op == "$toDouble" || op == "$toDecimal") {
        auto a = args();
        need(1, a);
        if (nullish(a[0])) return Value();
        double d;
        if (a[0]->isString()) {
            try {
                size_t used = 0;
                d = std::stod(a[0]->asString(), &used);
                if (used != a[0]->asString().size()) throw std::invalid_argument("x");
            } catch (const std::exception &) {
                throw Error("nevla memory database: cannot convert \"" + a[0]->asString() + "\" to a number");
            }
        } else if (a[0]->isBool()) {
            d = a[0]->asBool();
        } else if (a[0]->isDate()) {
            d = static_cast<double>(a[0]->asDate().ms);
        } else {
            d = a[0]->asDouble();
        }
        return op == "$toDouble" || op == "$toDecimal" ? Value(d) : Value(static_cast<int64_t>(d));
    }
    if (op == "$toDate") {
        auto a = args();
        need(1, a);
        if (nullish(a[0])) return Value();
        if (a[0]->isDate()) return *a[0];
        if (a[0]->isNumber()) return Value(Date{a[0]->asInt()});
        if (auto d = Date::parse(a[0]->str())) return Value(*d);
        throw Error("nevla memory database: cannot convert \"" + a[0]->str() + "\" to a date");
    }
    if (op == "$toBool") {
        auto a = args();
        need(1, a);
        if (nullish(a[0])) return Value();
        return Value(a[0]->isString() ? true : truthy(a[0]));
    }
    if (op == "$type") {
        auto a = args();
        need(1, a);
        return Value(typeName(a[0] ? &*a[0] : nullptr));
    }
    // ---- dates
    if (op == "$year" || op == "$month" || op == "$dayOfMonth" || op == "$hour" || op == "$minute" || op == "$second" ||
        op == "$millisecond" || op == "$dayOfWeek") {
        std::optional<Value> d, tz;
        if (arg.isObject() && arg.contains("date")) {
            d = ev(arg["date"], c);
            if (arg.contains("timezone")) tz = ev(arg["timezone"], c);
        } else {
            d = ev(arg.isArray() && arg.size() == 1 ? arg.at(0) : arg, c);
        }
        if (nullish(d)) return Value();
        if (!d->isDate()) throw Error("nevla memory database: " + op + " needs a date");
        Parts p = parts(d->asDate().ms + tzOffsetMs(tz));
        if (op == "$year") return Value(p.year);
        if (op == "$month") return Value(static_cast<int64_t>(p.month));
        if (op == "$dayOfMonth") return Value(static_cast<int64_t>(p.day));
        if (op == "$hour") return Value(p.hour);
        if (op == "$minute") return Value(p.minute);
        if (op == "$second") return Value(p.second);
        if (op == "$millisecond") return Value(p.ms);
        return Value(p.dow);
    }
    if (op == "$dateTrunc") {
        auto d = ev(arg["date"], c);
        if (nullish(d)) return Value();
        if (!d->isDate()) throw Error("nevla memory database: $dateTrunc needs a date");
        std::string unit = orNull(ev(arg["unit"], c)).str();
        int64_t bin = arg.contains("binSize") ? orNull(ev(arg["binSize"], c)).asInt(1) : 1;
        std::optional<Value> tz;
        if (arg.contains("timezone")) tz = ev(arg["timezone"], c);
        return Value(Date{dateTrunc(d->asDate().ms, unit, bin, tzOffsetMs(tz))});
    }
    if (op == "$dateDiff") {
        auto s = ev(arg["startDate"], c), e = ev(arg["endDate"], c);
        if (nullish(s) || nullish(e)) return Value();
        std::string unit = orNull(ev(arg["unit"], c)).str();
        std::optional<Value> tz;
        if (arg.contains("timezone")) tz = ev(arg["timezone"], c);
        int64_t off = tzOffsetMs(tz);
        int64_t a = dateTrunc(s->asDate().ms, unit == "week" ? "week" : unit, 1, off);
        int64_t b = dateTrunc(e->asDate().ms, unit == "week" ? "week" : unit, 1, off);
        if (unit == "month" || unit == "quarter" || unit == "year") {
            Parts pa = parts(a + off), pb = parts(b + off);
            int64_t months = (pb.year - pa.year) * 12 + (static_cast<int64_t>(pb.month) - pa.month);
            return Value(unit == "month" ? months : unit == "quarter" ? floorDiv(months, 3) : pb.year - pa.year);
        }
        return Value((b - a) / unitMs(unit));
    }
    unsupported("aggregation operator " + op);
}

/* ======================= updates ======================= */
void setPathRaw(Value &doc, const std::vector<std::string> &segs, size_t i, Value v) {
    const std::string &seg = segs[i];
    bool last = i + 1 == segs.size();
    if (doc.isArray()) {
        if (!numeric(seg)) throw Error("nevla memory database: cannot use the part (" + seg + ") to traverse an array");
        size_t idx = std::stoul(seg);
        while (doc.size() <= idx) doc.push_back(Value());
        if (last)
            doc.at(idx) = std::move(v);
        else {
            Value &next = doc.at(idx);
            if (next.isNull()) next = Value::object();
            setPathRaw(next, segs, i + 1, std::move(v));
        }
        return;
    }
    if (!doc.isObject()) {
        if (!doc.isNull()) throw Error("nevla memory database: cannot create field '" + seg + "' in a non-object");
        doc = Value::object();
    }
    if (last) {
        doc[seg] = std::move(v);
        return;
    }
    Value &next = doc[seg];
    if (next.isNull()) next = Value::object();
    setPathRaw(next, segs, i + 1, std::move(v));
}

Value *getPtr(Value &doc, const std::vector<std::string> &segs) {
    Value *cur = &doc;
    for (const auto &s : segs) {
        if (cur->isObject())
            cur = cur->find(s);
        else if (cur->isArray() && numeric(s) && std::stoul(s) < cur->size())
            cur = &cur->at(std::stoul(s));
        else
            return nullptr;
        if (!cur) return nullptr;
    }
    return cur;
}

void unsetPath(Value &doc, const std::vector<std::string> &segs) {
    std::vector<std::string> parent(segs.begin(), segs.end() - 1);
    Value *p = parent.empty() ? &doc : getPtr(doc, parent);
    if (!p) return;
    if (p->isObject())
        p->erase(segs.back());
    else if (p->isArray() && numeric(segs.back()) && std::stoul(segs.back()) < p->size())
        p->at(std::stoul(segs.back())) = Value();
}

// "items.$.qty" / "items.$[].qty" -> concrete paths
std::vector<std::vector<std::string>> expand(const Value &doc, const std::vector<std::string> &segs,
                                             const Value &filter) {
    std::vector<std::vector<std::string>> out{{}};
    for (size_t i = 0; i < segs.size(); i++) {
        const std::string &s = segs[i];
        if (s == "$" || s == "$[]") {
            std::vector<std::vector<std::string>> next;
            for (auto &prefix : out) {
                const Value *arr = getPtr(const_cast<Value &>(doc), prefix);
                if (!arr || !arr->isArray())
                    throw Error("nevla memory database: the positional operator needs an array");
                if (s == "$[]") {
                    for (size_t k = 0; k < arr->size(); k++) {
                        auto p = prefix;
                        p.push_back(std::to_string(k));
                        next.push_back(p);
                    }
                    continue;
                }
                // first element that matches the filter's conditions on this array
                std::string pre;
                for (const auto &x : prefix) pre += (pre.empty() ? "" : ".") + x;
                Value sub = Value::object();
                for (const auto &kv : filter.items())
                    if (kv.first == pre || kv.first.rfind(pre + ".", 0) == 0) sub[kv.first] = kv.second;
                bool found = false;
                for (size_t k = 0; k < arr->size() && !found; k++) {
                    Value probe = Value::object();
                    Value one = Value::array();
                    one.push_back(arr->at(k));
                    probe.pathRef(pre) = one;
                    if (matches(probe, sub)) {
                        auto p = prefix;
                        p.push_back(std::to_string(k));
                        next.push_back(p);
                        found = true;
                    }
                }
                if (!found)
                    throw Error(
                        "nevla memory database: the positional operator did not find the match needed from the query");
            }
            out = std::move(next);
        } else if (s.size() > 2 && s[0] == '$' && s[1] == '[') {
            unsupported("arrayFilters (" + s + ")");
        } else {
            for (auto &p : out) p.push_back(s);
        }
    }
    return out;
}

void applyUpdate(Value &doc, const Value &update, bool inserting, const Value &filter, Date now) {
    for (const auto &opkv : update.items()) {
        const std::string &op = opkv.first;
        if (op.empty() || op[0] != '$') throw Error("nevla memory database: an update needs operators ($set ...)");
        if (op == "$setOnInsert" && !inserting) continue;
        for (const auto &kv : opkv.second.items()) {
            if (kv.first == "_id" && op != "$setOnInsert" && !inserting) {
                const Value *cur = doc.find("_id");
                if (op == "$set" && cur && eq(*cur, kv.second)) continue;
                throw Error(
                    "nevla memory database: performing an update on the path '_id' would modify the immutable field "
                    "'_id'");
            }
            if (op == "$rename") {
                auto from = split(kv.first);
                Value *v = getPtr(doc, from);
                if (!v) continue;
                Value moved = *v;
                unsetPath(doc, from);
                setPathRaw(doc, split(kv.second.str()), 0, moved);
                continue;
            }
            for (auto &segs : expand(doc, split(kv.first), filter)) {
                Value *cur = getPtr(doc, segs);
                const Value &v = kv.second;
                if (op == "$set" || op == "$setOnInsert") {
                    setPathRaw(doc, segs, 0, v);
                } else if (op == "$unset") {
                    unsetPath(doc, segs);
                } else if (op == "$inc" || op == "$mul") {
                    if (!v.isNumber()) throw Error("nevla memory database: " + op + " needs a number");
                    if (!cur) {
                        setPathRaw(doc, segs, 0, op == "$inc" ? v : (v.isInt() ? Value(0) : Value(0.0)));
                    } else {
                        if (!cur->isNumber())
                            throw Error("nevla memory database: cannot apply " + op +
                                        " to a value of non-numeric type");
                        double r = op == "$inc" ? cur->asDouble() + v.asDouble() : cur->asDouble() * v.asDouble();
                        *cur = numResult(r, cur->isInt() && v.isInt());
                    }
                } else if (op == "$min" || op == "$max") {
                    if (!cur || (op == "$min" ? cmpVal(v, *cur) < 0 : cmpVal(v, *cur) > 0)) setPathRaw(doc, segs, 0, v);
                } else if (op == "$currentDate") {
                    setPathRaw(doc, segs, 0, Value(now));
                } else if (op == "$push" || op == "$addToSet") {
                    std::vector<Value> items;
                    if (v.isObject() && v.contains("$each")) {
                        for (const auto &k : v.items())
                            if (k.first != "$each") unsupported(op + " modifier " + k.first);
                        items = v["$each"].elements();
                    } else {
                        items.push_back(v);
                    }
                    if (!cur) {
                        setPathRaw(doc, segs, 0, Value::array());
                        cur = getPtr(doc, segs);
                    }
                    if (!cur->isArray()) throw Error("nevla memory database: " + op + " needs an array field");
                    for (auto &it : items) {
                        if (op == "$addToSet") {
                            bool has = false;
                            for (const auto &e : cur->elements()) has |= eq(e, it);
                            if (has) continue;
                        }
                        cur->push_back(it);
                    }
                } else if (op == "$pull" || op == "$pullAll") {
                    if (!cur) continue;
                    if (!cur->isArray()) throw Error("nevla memory database: " + op + " needs an array field");
                    Value kept = Value::array();
                    for (const auto &e : cur->elements()) {
                        bool remove;
                        if (op == "$pullAll") {
                            remove = false;
                            for (const auto &x : v.elements()) remove |= eq(e, x);
                        } else if (isOps(v)) {
                            remove = matchCond(one(e), v, v);
                        } else if (v.isObject()) {
                            remove = e.isObject() && matches(e, v);
                        } else {
                            remove = eq(e, v);
                        }
                        if (!remove) kept.push_back(e);
                    }
                    *cur = kept;
                } else if (op == "$pop") {
                    if (cur && cur->isArray() && !cur->empty()) {
                        Value kept = Value::array();
                        auto &els = cur->elements();
                        for (size_t k = 0; k < els.size(); k++)
                            if (v.asInt() < 0 ? k != 0 : k + 1 != els.size()) kept.push_back(els[k]);
                        *cur = kept;
                    }
                } else {
                    unsupported("update operator " + op);
                }
            }
        }
    }
}

// The document an upsert starts from: the filter's equality conditions
Value seedFromFilter(const Value &filter) {
    Value doc = Value::object();
    std::function<void(const Value &)> walk = [&](const Value &f) {
        for (const auto &kv : f.items()) {
            if (kv.first == "$and") {
                for (const auto &e : kv.second.elements()) walk(e);
            } else if (!kv.first.empty() && kv.first[0] != '$') {
                if (!isOps(kv.second))
                    setPathRaw(doc, split(kv.first), 0, kv.second);
                else if (kv.second.contains("$eq"))
                    setPathRaw(doc, split(kv.first), 0, kv.second["$eq"]);
            }
        }
    };
    walk(filter);
    return doc;
}

Value withIdFirst(Value doc) {
    if (!doc.contains("_id")) {
        Value out = Value::object();
        out["_id"] = ObjectId::generate();
        for (auto &kv : doc.items()) out[kv.first] = std::move(kv.second);
        return out;
    }
    if (!doc.items().empty() && doc.items()[0].first == "_id") return doc;
    Value out = Value::object();
    out["_id"] = doc["_id"];
    for (auto &kv : doc.items())
        if (kv.first != "_id") out[kv.first] = std::move(kv.second);
    return out;
}

/* ======================= unique indexes ======================= */
std::string indexName(const Value &keys) {
    std::string n;
    for (const auto &kv : keys.items()) n += (n.empty() ? "" : "_") + kv.first + "_" + kv.second.str();
    return n;
}

// nullopt: a sparse index skips this document
std::optional<std::string> indexKey(const Index &ix, const Value &doc) {
    std::string key;
    bool any = false;
    for (const auto &kv : ix.keys.items()) {
        auto c = candidates(doc, kv.first);
        any |= !c.empty();
        key += keyOf(c.vals.empty() ? Value() : c.vals[0]) + "|";
    }
    if (ix.sparse && !any) return std::nullopt;
    return key;
}

[[noreturn]] void dupError(const std::string &db, const std::string &name, const Index &ix, const Value &doc) {
    std::string dup;
    for (const auto &kv : ix.keys.items()) {
        auto c = candidates(doc, kv.first);
        dup += (dup.empty() ? "" : ", ") + kv.first + ": " + (c.vals.empty() ? Value() : c.vals[0]).toJson();
    }
    throw DuplicateKeyError("E11000 duplicate key error collection: " + db + "." + name + " index: " + ix.name +
                            " dup key: { " + dup + " }");
}

const Index &idIndex() {
    static const Index ix{Value{{"_id", 1}}, "_id_", true, false, Value()};
    return ix;
}

// Unique keys of a document: (index, key)
std::vector<std::pair<const Index *, std::string>> uniqueKeys(const Coll &c, const Value &doc) {
    std::vector<std::pair<const Index *, std::string>> out;
    if (auto k = indexKey(idIndex(), doc)) out.emplace_back(&idIndex(), *k);
    for (const auto &ix : c.indexes)
        if (ix.unique)
            if (auto k = indexKey(ix, doc)) out.emplace_back(&ix, *k);
    return out;
}
void addKeys(Coll &c, const Value &doc) {
    for (auto &[ix, k] : uniqueKeys(c, doc)) c.uniq[ix->name][k]++;
}
void removeKeys(Coll &c, const Value &doc) {
    for (auto &[ix, k] : uniqueKeys(c, doc)) {
        auto &m = c.uniq[ix->name];
        auto it = m.find(k);
        if (it != m.end() && --it->second <= 0) m.erase(it);
    }
}
// Throws DuplicateKeyError if `doc` (not yet registered) collides with a stored document
void checkUnique(const std::string &db, const std::string &name, Coll &c, const Value &doc) {
    for (auto &[ix, k] : uniqueKeys(c, doc)) {
        auto it = c.uniq.find(ix->name);
        if (it != c.uniq.end() && it->second.count(k)) dupError(db, name, *ix, doc);
    }
}
// Replace a stored document (in place) keeping the unique keys right; on a clash nothing changes
void validateDoc(const Coll &c, const Value &doc, const Value *before);
void storeChanged(const std::string &db, const std::string &name, Coll &c, Value &slot, Value next) {
    validateDoc(c, next, &slot);
    removeKeys(c, slot);
    try {
        checkUnique(db, name, c, next);
    } catch (...) {
        addKeys(c, slot);
        throw;
    }
    addKeys(c, next);
    slot = std::move(next);
}

/* ======================= $jsonSchema validation ======================= */
bool jsonValid(const Value &schema, const Value &v) {
    if (schema.contains("bsonType")) {
        std::string t = typeName(&v);
        bool ok = false;
        for (const auto &want : argList(schema["bsonType"])) {
            std::string w = want.str();
            ok |= w == t || (w == "number" && (t == "int" || t == "long" || t == "double")) ||
                  (w == "decimal" && t == "double");
        }
        if (!ok) return false;
    }
    if (schema.contains("enum")) {
        bool ok = false;
        for (const auto &e : schema["enum"].elements()) ok |= eq(e, v);
        if (!ok) return false;
    }
    if (v.isString()) {
        size_t cp = 0;
        for (unsigned char ch : v.asString()) cp += (ch & 0xC0) != 0x80;
        if (schema.contains("minLength") && static_cast<int64_t>(cp) < schema["minLength"].asInt()) return false;
        if (schema.contains("maxLength") && static_cast<int64_t>(cp) > schema["maxLength"].asInt()) return false;
        if (schema.contains("pattern") && !std::regex_search(v.asString(), std::regex(schema["pattern"].str())))
            return false;
    }
    if (v.isNumber()) {
        if (schema.contains("minimum") && v.asDouble() < schema["minimum"].asDouble()) return false;
        if (schema.contains("maximum") && v.asDouble() > schema["maximum"].asDouble()) return false;
    }
    if (v.isArray()) {
        if (schema.contains("minItems") && static_cast<int64_t>(v.size()) < schema["minItems"].asInt()) return false;
        if (schema.contains("maxItems") && static_cast<int64_t>(v.size()) > schema["maxItems"].asInt()) return false;
        if (schema["items"].isObject())
            for (const auto &e : v.elements())
                if (!jsonValid(schema["items"], e)) return false;
    }
    if (v.isObject()) {
        for (const auto &r : schema["required"].elements())
            if (!v.contains(r.str())) return false;
        const Value &props = schema["properties"];
        for (const auto &kv : v.items()) {
            if (const Value *p = props.find(kv.first)) {
                if (!jsonValid(*p, kv.second)) return false;
            } else if (schema.contains("additionalProperties")) {
                const Value &ap = schema["additionalProperties"];
                if (ap.isBool() && !ap.asBool()) return false;
                if (ap.isObject() && !jsonValid(ap, kv.second)) return false;
            }
        }
    }
    return true;
}

void validateDoc(const Coll &c, const Value &doc, const Value *before) {
    if (!c.validator.isObject() || c.validationLevel == "off") return;
    const Value &schema = c.validator["$jsonSchema"];
    if (!schema.isObject()) return;
    if (c.validationLevel == "moderate" && before && !jsonValid(schema, *before)) return;  // was already invalid
    if (!jsonValid(schema, doc)) throw Error("MongoDB: Document failed validation");
}

/* ======================= aggregation pipeline ======================= */
std::vector<Value> runPipeline(const std::string &db, std::vector<Value> docs, const Value &pipeline);

struct Acc {
    Acc(std::string o, Value a) : op(std::move(o)), arg(std::move(a)) {}
    std::string op;
    Value arg;
    // state
    double sum = 0, sumSq = 0;
    int64_t n = 0;
    bool allInt = true;
    std::optional<Value> best, first, last;
    bool hasFirst = false;
    Value list = Value::array();
    std::set<std::string> seen;
};

void accumulate(Acc &a, Ctx &c) {
    if (a.op == "$count") {
        a.n++;
        return;
    }
    auto v = ev(a.arg, c);
    if (a.op == "$sum" || a.op == "$avg" || a.op == "$stdDevPop" || a.op == "$stdDevSamp") {
        if (v && v->isArray() && a.op == "$sum") {
            for (const auto &e : v->elements())
                if (e.isNumber()) a.sum += e.asDouble(), a.allInt &= e.isInt(), a.n++;
            return;
        }
        if (v && v->isNumber()) {
            a.sum += v->asDouble();
            a.sumSq += v->asDouble() * v->asDouble();
            a.allInt &= v->isInt();
            a.n++;
        }
    } else if (a.op == "$min" || a.op == "$max") {
        if (v && !v->isNull() && (!a.best || (a.op == "$min" ? cmpVal(*v, *a.best) < 0 : cmpVal(*v, *a.best) > 0)))
            a.best = v;
    } else if (a.op == "$first") {
        if (!a.hasFirst) {
            a.first = v ? *v : Value();
            a.hasFirst = true;
        }
    } else if (a.op == "$last") {
        a.last = v ? *v : Value();
    } else if (a.op == "$push") {
        if (v) a.list.push_back(*v);
    } else if (a.op == "$addToSet") {
        if (v && a.seen.insert(keyOf(*v)).second) a.list.push_back(*v);
    } else {
        unsupported("group accumulator " + a.op);
    }
}

Value finish(const Acc &a) {
    if (a.op == "$count") return Value(a.n);
    if (a.op == "$sum") return numResult(a.sum, a.allInt && std::fabs(a.sum) < 9e15);
    if (a.op == "$avg") return a.n ? Value(a.sum / a.n) : Value();
    if (a.op == "$stdDevPop" || a.op == "$stdDevSamp") {
        int64_t d = a.op == "$stdDevPop" ? a.n : a.n - 1;
        if (a.n == 0 || d <= 0) return Value();
        double mean = a.sum / a.n, var = (a.sumSq - a.n * mean * mean) / d;
        return Value(std::sqrt(std::max(0.0, var)));
    }
    if (a.op == "$min" || a.op == "$max") return a.best ? *a.best : Value();
    if (a.op == "$first") return a.first ? *a.first : Value();
    if (a.op == "$last") return a.last ? *a.last : Value();
    return a.list;
}

std::vector<Value> groupStage(std::vector<Value> &docs, const Value &spec) {
    if (!spec.contains("_id")) throw Error("nevla memory database: $group needs an _id");
    struct G {
        Value id;
        std::vector<Acc> accs;
    };
    std::vector<G> groups;
    std::map<std::string, size_t> index;
    for (const auto &d : docs) {
        Ctx c{&d, &d, {}, Date::now()};
        Value id = orNull(ev(spec["_id"], c));
        std::string k = keyOf(id);
        auto it = index.find(k);
        if (it == index.end()) {
            G g{id, {}};
            for (const auto &kv : spec.items()) {
                if (kv.first == "_id") continue;
                if (!kv.second.isObject() || kv.second.size() != 1)
                    throw Error("nevla memory database: $group field \"" + kv.first + "\" needs one accumulator");
                g.accs.emplace_back(kv.second.items()[0].first, kv.second.items()[0].second);
            }
            it = index.emplace(k, groups.size()).first;
            groups.push_back(std::move(g));
        }
        for (auto &a : groups[it->second].accs) accumulate(a, c);
    }
    std::vector<Value> out;
    for (auto &g : groups) {
        Value row = Value::object();
        row["_id"] = g.id;
        size_t i = 0;
        for (const auto &kv : spec.items()) {
            if (kv.first == "_id") continue;
            row[kv.first] = finish(g.accs[i++]);
        }
        out.push_back(std::move(row));
    }
    return out;
}

Value projectStage(const Value &doc, const Value &spec) {
    bool inclusion = false, exclusion = false;
    for (const auto &kv : spec.items()) {
        if (kv.first == "_id") continue;
        const Value &v = kv.second;
        if ((v.isNumber() || v.isBool()) && !truthy(v))
            exclusion = true;
        else
            inclusion = true;
    }
    if (inclusion && exclusion) throw Error("nevla memory database: $project cannot mix inclusion and exclusion");
    if (!inclusion) {
        Value out = doc;
        for (const auto &kv : spec.items())
            if (!truthy(kv.second)) out.erasePath(kv.first);
        return out;
    }
    Value out = Value::object();
    bool keepId = !(spec.contains("_id") && (spec["_id"].isNumber() || spec["_id"].isBool()) && !truthy(spec["_id"]));
    if (keepId && doc.contains("_id") && !(spec.contains("_id") && !(spec["_id"].isNumber() || spec["_id"].isBool())))
        out["_id"] = doc["_id"];
    for (const auto &kv : spec.items()) {
        if (kv.first == "_id" && (kv.second.isNumber() || kv.second.isBool())) continue;
        if (kv.second.isNumber() || kv.second.isBool()) {
            copyPath(doc, out, kv.first);
        } else {
            Ctx c{&doc, &doc, {}, Date::now()};
            if (auto v = ev(kv.second, c)) out.pathRef(kv.first) = *v;
        }
    }
    return out;
}

std::vector<Value> lookupStage(const std::string &db, std::vector<Value> &docs, const Value &spec) {
    if (spec.contains("let")) unsupported("$lookup with let");
    std::string from = spec["from"].str(), as = spec["as"].str();
    std::vector<Value> foreign;
    {
        auto &dbs = gDbs[db].colls;
        if (auto it = dbs.find(from); it != dbs.end()) foreign = it->second.docs;
    }
    bool hasLocal = spec.contains("localField");
    std::vector<Value> out;
    for (auto &d : docs) {
        std::vector<Value> hits;
        if (hasLocal) {
            auto locals = candidates(d, spec["localField"].str());
            std::vector<Value> keys;
            for (const auto &l : locals.vals) {
                if (l.isArray())
                    for (const auto &e : l.elements()) keys.push_back(e);
                else
                    keys.push_back(l);
            }
            if (keys.empty()) keys.push_back(Value());  // missing local matches null / missing foreign
            for (const auto &f : foreign) {
                auto fc = candidates(f, spec["foreignField"].str());
                bool hit = false;
                for (const auto &k : keys) hit |= eqMatch(fc, k);
                if (hit) hits.push_back(f);
            }
        } else {
            hits = foreign;
        }
        if (spec.contains("pipeline")) hits = runPipeline(db, hits, spec["pipeline"]);
        Value arr = Value::array();
        for (auto &h : hits) arr.push_back(std::move(h));
        d.pathRef(as) = arr;
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<Value> densifyStage(std::vector<Value> &docs, const Value &spec) {
    std::string field = spec["field"].str();
    const Value &range = spec["range"];
    std::vector<std::string> partFields;
    for (const auto &p : spec["partitionByFields"].elements()) partFields.push_back(p.str());
    std::string unit = range["unit"].str();
    double step = range["step"].asDouble();
    if (step <= 0) throw Error("nevla memory database: $densify step must be > 0");
    auto add = [&](const Value &v, int64_t times) -> Value {
        if (!v.isDate()) return Value(v.asDouble() + step * times);
        int64_t ms = v.asDate().ms;
        if (unit == "month" || unit == "quarter" || unit == "year") {
            Parts p = parts(ms);
            int64_t months = static_cast<int64_t>(step) * times * (unit == "month" ? 1 : unit == "quarter" ? 3 : 12);
            int64_t total = p.year * 12 + (p.month - 1) + months;
            int64_t y = floorDiv(total, 12);
            unsigned m = static_cast<unsigned>(total - y * 12) + 1;
            return Value(Date{daysFromCivil(y, m, p.day) * 86400000LL + p.hour * 3600000 + p.minute * 60000 +
                              p.second * 1000 + p.ms});
        }
        int64_t u = unitMs(unit);
        if (!u) throw Error("nevla memory database: $densify unit \"" + unit + "\" is not valid");
        return Value(Date{ms + static_cast<int64_t>(step) * u * times});
    };
    // partitions in first-seen order
    std::map<std::string, std::vector<size_t>> byPart;
    std::vector<std::string> order;
    for (size_t i = 0; i < docs.size(); i++) {
        std::string k;
        for (const auto &p : partFields) k += keyOf(orNull(fieldValue(docs[i], split(p), 0))) + "|";
        if (!byPart.count(k)) order.push_back(k);
        byPart[k].push_back(i);
    }
    std::optional<Value> gmin, gmax;
    for (const auto &d : docs) {
        auto v = fieldValue(d, split(field), 0);
        if (!v) continue;
        if (!gmin || cmpVal(*v, *gmin) < 0) gmin = v;
        if (!gmax || cmpVal(*v, *gmax) > 0) gmax = v;
    }
    std::string bounds = range["bounds"].isString() ? range["bounds"].asString() : "explicit";
    std::vector<Value> out;
    for (const auto &k : order) {
        auto &idx = byPart[k];
        std::vector<Value> part;
        for (size_t i : idx) part.push_back(docs[i]);
        sortDocs(part, Value{{field, 1}});
        std::optional<Value> lo, hi;
        if (bounds == "full")
            lo = gmin, hi = gmax;
        else if (bounds == "partition") {
            for (const auto &d : part)
                if (auto v = fieldValue(d, split(field), 0)) {
                    if (!lo) lo = v;
                    hi = v;
                }
        } else {
            unsupported("$densify explicit bounds");
        }
        std::set<std::string> have;
        for (const auto &d : part) have.insert(keyOf(orNull(fieldValue(d, split(field), 0))));
        std::vector<Value> extra;
        if (lo && hi)
            for (int64_t t = 0;; t++) {
                Value v = add(*lo, t);
                if (cmpVal(v, *hi) > 0 || t > 1000000) break;
                if (have.count(keyOf(v))) continue;
                Value nd = Value::object();
                for (const auto &p : partFields)
                    if (auto pv = fieldValue(part[0], split(p), 0)) nd.pathRef(p) = *pv;
                nd.pathRef(field) = v;
                extra.push_back(nd);
            }
        for (auto &e : extra) part.push_back(std::move(e));
        sortDocs(part, Value{{field, 1}});
        for (auto &d : part) out.push_back(std::move(d));
    }
    return out;
}

std::vector<Value> runPipeline(const std::string &db, std::vector<Value> docs, const Value &pipeline) {
    if (!pipeline.isArray()) throw Error("nevla memory database: a pipeline must be an array");
    for (const auto &stage : pipeline.elements()) {
        if (!stage.isObject() || stage.size() != 1)
            throw Error("nevla memory database: bad pipeline stage " + stage.toJson());
        const std::string &name = stage.items()[0].first;
        const Value &spec = stage.items()[0].second;
        std::vector<Value> next;
        if (name == "$match") {
            for (auto &d : docs)
                if (matches(d, spec)) next.push_back(std::move(d));
        } else if (name == "$sort") {
            sortDocs(docs, spec);
            next = std::move(docs);
        } else if (name == "$limit") {
            if (spec.asInt() <= 0) throw Error("nevla memory database: $limit must be positive");
            for (size_t i = 0; i < docs.size() && static_cast<int64_t>(i) < spec.asInt(); i++)
                next.push_back(std::move(docs[i]));
        } else if (name == "$skip") {
            for (size_t i = static_cast<size_t>(std::max<int64_t>(0, spec.asInt())); i < docs.size(); i++)
                next.push_back(std::move(docs[i]));
        } else if (name == "$project") {
            for (auto &d : docs) next.push_back(projectStage(d, spec));
        } else if (name == "$addFields" || name == "$set") {
            for (auto &d : docs) {
                Ctx c{&d, &d, {}, Date::now()};
                Value nd = d;
                for (const auto &kv : spec.items()) {
                    auto v = ev(kv.second, c);
                    if (v)
                        nd.pathRef(kv.first) = *v;
                    else
                        nd.erasePath(kv.first);
                }
                next.push_back(std::move(nd));
            }
        } else if (name == "$unset") {
            for (auto &d : docs) {
                for (const auto &f : argList(spec)) d.erasePath(f.str());
                next.push_back(std::move(d));
            }
        } else if (name == "$group") {
            next = groupStage(docs, spec);
        } else if (name == "$unwind") {
            std::string path = (spec.isObject() ? spec["path"] : spec).str();
            bool keep = spec.isObject() && spec["preserveNullAndEmptyArrays"].asBool();
            if (spec.isObject() && spec.contains("includeArrayIndex")) unsupported("$unwind includeArrayIndex");
            if (path.empty() || path[0] != '$') throw Error("nevla memory database: $unwind path must start with $");
            path = path.substr(1);
            for (auto &d : docs) {
                const Value *v = d.path(path);
                if (v && v->isArray() && !v->empty()) {
                    for (const auto &e : v->elements()) {
                        Value nd = d;
                        nd.pathRef(path) = e;
                        next.push_back(std::move(nd));
                    }
                } else if (v && !v->isArray() && !v->isNull()) {
                    next.push_back(std::move(d));
                } else if (keep) {
                    if (v && v->isArray()) d.erasePath(path);
                    next.push_back(std::move(d));
                }
            }
        } else if (name == "$count") {
            if (!docs.empty()) next.push_back(Value{{spec.str(), static_cast<int64_t>(docs.size())}});
        } else if (name == "$lookup") {
            next = lookupStage(db, docs, spec);
        } else if (name == "$replaceRoot" || name == "$replaceWith") {
            const Value &e = name == "$replaceRoot" ? spec["newRoot"] : spec;
            for (auto &d : docs) {
                Ctx c{&d, &d, {}, Date::now()};
                auto v = ev(e, c);
                if (!v || !v->isObject()) throw Error("nevla memory database: " + name + " must produce a document");
                next.push_back(*v);
            }
        } else if (name == "$sample") {
            static thread_local std::mt19937 rng{std::random_device{}()};
            std::shuffle(docs.begin(), docs.end(), rng);
            for (size_t i = 0; i < docs.size() && static_cast<int64_t>(i) < spec["size"].asInt(); i++)
                next.push_back(std::move(docs[i]));
        } else if (name == "$facet") {
            Value row = Value::object();
            for (const auto &kv : spec.items()) {
                Value arr = Value::array();
                for (auto &r : runPipeline(db, docs, kv.second)) arr.push_back(std::move(r));
                row[kv.first] = arr;
            }
            next.push_back(row);
        } else if (name == "$densify") {
            next = densifyStage(docs, spec);
        } else {
            unsupported("pipeline stage " + name);
        }
        docs = std::move(next);
    }
    return docs;
}

}  // namespace

/* ======================= public functions ======================= */
std::vector<Value> find(const std::string &db, const std::string &name, const FindSpec &spec) {
    Lock lk(gMu);
    std::vector<Value> out;
    for (const auto &d : coll(db, name).docs)
        if (matches(d, spec.filter)) out.push_back(d);
    sortDocs(out, spec.sort);
    std::vector<Value> page;
    for (size_t i = static_cast<size_t>(std::max<int64_t>(0, spec.skip)); i < out.size(); i++) {
        if (spec.limit > 0 && static_cast<int64_t>(page.size()) >= spec.limit) break;
        page.push_back(project(out[i], spec.projection));
    }
    return page;
}

int64_t count(const std::string &db, const std::string &name, const Value &filter) {
    Lock lk(gMu);
    int64_t n = 0;
    for (const auto &d : coll(db, name).docs) n += matches(d, filter);
    return n;
}

int64_t estimatedCount(const std::string &db, const std::string &name) {
    Lock lk(gMu);
    return static_cast<int64_t>(coll(db, name).docs.size());
}

std::vector<Value> distinct(const std::string &db, const std::string &name, const std::string &field,
                            const Value &filter) {
    Lock lk(gMu);
    std::vector<Value> out;
    std::set<std::string> seen;
    for (const auto &d : coll(db, name).docs) {
        if (!matches(d, filter)) continue;
        for (const auto &v : candidates(d, field).vals) {
            auto add = [&](const Value &x) {
                if (seen.insert(keyOf(x)).second) out.push_back(x);
            };
            if (v.isArray())
                for (const auto &e : v.elements()) add(e);
            else
                add(v);
        }
    }
    std::sort(out.begin(), out.end(), [](const Value &a, const Value &b) { return cmpVal(a, b) < 0; });
    return out;
}

void insertOne(const std::string &db, const std::string &name, const Value &doc) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    Value d = withIdFirst(doc);
    validateDoc(c, d, nullptr);
    checkUnique(db, name, c, d);
    addKeys(c, d);
    c.docs.push_back(std::move(d));
}

InsertResult insertMany(const std::string &db, const std::string &name, const std::vector<Value> &docs, bool ordered) {
    Lock lk(gMu);
    InsertResult r;
    for (const auto &d : docs) {
        try {
            insertOne(db, name, d);
            r.inserted++;
        } catch (const Error &e) {
            if (r.error.empty()) r.error = e.what();
            if (ordered) break;
        }
    }
    return r;
}

WriteResult update(const std::string &db, const std::string &name, const Value &filter, const Value &upd, bool upsert,
                   bool many) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    WriteResult r;
    Date now = Date::now();
    for (auto &d : c.docs) {
        if (!matches(d, filter)) continue;
        r.matched++;
        Value next = d;
        applyUpdate(next, upd, false, filter, now);
        if (!identical(next, d)) {
            storeChanged(db, name, c, d, std::move(next));
            r.modified++;
        }
        if (!many) break;
    }
    if (r.matched == 0 && upsert) {
        Value d = seedFromFilter(filter);
        applyUpdate(d, upd, true, filter, now);
        d = withIdFirst(d);
        validateDoc(c, d, nullptr);
        checkUnique(db, name, c, d);
        addKeys(c, d);
        r.upsertedId = d["_id"];
        c.docs.push_back(std::move(d));
    }
    return r;
}

WriteResult replace(const std::string &db, const std::string &name, const Value &filter, const Value &replacement,
                    bool upsert) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    WriteResult r;
    for (auto &d : c.docs) {
        if (!matches(d, filter)) continue;
        r.matched = 1;
        if (replacement.contains("_id") && !eq(replacement["_id"], d["_id"]))
            throw Error("nevla memory database: the _id field cannot be changed by a replacement");
        Value nd = replacement;
        nd["_id"] = d["_id"];
        nd = withIdFirst(nd);
        r.modified = !identical(nd, d);
        storeChanged(db, name, c, d, std::move(nd));
        return r;
    }
    if (upsert) {
        Value d = seedFromFilter(filter);
        for (const auto &kv : replacement.items()) d[kv.first] = kv.second;
        d = withIdFirst(d);
        validateDoc(c, d, nullptr);
        checkUnique(db, name, c, d);
        addKeys(c, d);
        r.upsertedId = d["_id"];
        c.docs.push_back(std::move(d));
    }
    return r;
}

int64_t remove(const std::string &db, const std::string &name, const Value &filter, bool many) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    int64_t n = 0;
    std::vector<Value> kept;
    kept.reserve(c.docs.size());
    for (auto &d : c.docs) {
        if ((many || n == 0) && matches(d, filter)) {
            removeKeys(c, d);
            n++;
        } else {
            kept.push_back(std::move(d));
        }
    }
    c.docs = std::move(kept);
    return n;
}

std::optional<Value> findOneAndUpdate(const std::string &db, const std::string &name, const Value &filter,
                                      const Value &upd, bool upsert, bool returnNew, const Value &projection) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    for (auto &d : c.docs) {
        if (!matches(d, filter)) continue;
        Value before = d, next = d;
        applyUpdate(next, upd, false, filter, Date::now());
        if (!identical(next, d)) storeChanged(db, name, c, d, std::move(next));
        return project(returnNew ? d : before, projection);
    }
    if (!upsert) return std::nullopt;
    auto r = update(db, name, filter, upd, true, false);
    if (!returnNew) return std::nullopt;
    for (const auto &d : c.docs)
        if (eq(d["_id"], r.upsertedId)) return project(d, projection);
    return std::nullopt;
}

std::optional<Value> findOneAndReplace(const std::string &db, const std::string &name, const Value &filter,
                                       const Value &replacement, bool upsert, bool returnNew, const Value &projection) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    std::optional<Value> before;
    for (const auto &d : c.docs)
        if (matches(d, filter)) {
            before = d;
            break;
        }
    auto r = replace(db, name, filter, replacement, upsert);
    if (!returnNew) return before ? std::optional<Value>(project(*before, projection)) : std::nullopt;
    Value id = before ? (*before)["_id"] : r.upsertedId;
    if (id.isNull()) return std::nullopt;
    for (const auto &d : c.docs)
        if (eq(d["_id"], id)) return project(d, projection);
    return std::nullopt;
}

std::optional<Value> findOneAndDelete(const std::string &db, const std::string &name, const Value &filter) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    for (size_t i = 0; i < c.docs.size(); i++)
        if (matches(c.docs[i], filter)) {
            Value d = c.docs[i];
            removeKeys(c, d);
            c.docs.erase(c.docs.begin() + static_cast<std::ptrdiff_t>(i));
            return d;
        }
    return std::nullopt;
}

std::vector<Value> aggregate(const std::string &db, const std::string &name, const Value &pipeline) {
    Lock lk(gMu);
    // $indexStats (first stage): one document per index
    for (size_t i = 0; i < pipeline.size(); i++)
        if (pipeline.at(i).isObject() && pipeline.at(i).contains("$indexStats")) {
            std::vector<Value> stats;
            for (const auto &ix : listIndexes(db, name))
                stats.push_back(Value{{"name", ix["name"]}, {"key", ix["key"]}});
            Value rest = Value::array();
            for (size_t k = i + 1; k < pipeline.size(); k++) rest.push_back(pipeline.at(k));
            return runPipeline(db, stats, rest);
        }
    return runPipeline(db, coll(db, name).docs, pipeline);
}

void createIndex(const std::string &db, const std::string &name, const Value &keys, const Value &options) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    Index ix{keys, options["name"].isString() ? options["name"].asString() : indexName(keys),
             options["unique"].asBool(), options["sparse"].asBool(), options};
    for (const auto &e : c.indexes)
        if (e.name == ix.name) return;
    if (ix.unique) {
        std::unordered_map<std::string, int> keys;
        for (const auto &d : c.docs)
            if (auto k = indexKey(ix, d); k && keys[*k]++) dupError(db, name, ix, d);
        c.uniq[ix.name] = std::move(keys);
    }
    c.indexes.push_back(std::move(ix));
}

std::vector<Value> listIndexes(const std::string &db, const std::string &name) {
    Lock lk(gMu);
    std::vector<Value> out{Value{{"v", 2}, {"key", Value{{"_id", 1}}}, {"name", "_id_"}}};
    for (const auto &ix : coll(db, name).indexes) {
        Value v{{"v", 2}, {"key", ix.keys}, {"name", ix.name}};
        for (const auto &kv : ix.options.items())
            if (kv.first != "name") v[kv.first] = kv.second;
        out.push_back(v);
    }
    return out;
}

void dropIndex(const std::string &db, const std::string &name, const std::string &index) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    c.indexes.erase(std::remove_if(c.indexes.begin(), c.indexes.end(), [&](const Index &i) { return i.name == index; }),
                    c.indexes.end());
    c.uniq.erase(index);
}

void drop(const std::string &db, const std::string &name) {
    Lock lk(gMu);
    gDbs[db].colls.erase(name);
}

void dropDatabase(const std::string &db) {
    Lock lk(gMu);
    gDbs.erase(db);
}

bool createCollection(const std::string &db, const std::string &name, const Value &options) {
    Lock lk(gMu);
    auto &colls = gDbs[db].colls;
    if (colls.count(name)) return false;
    Coll &c = colls[name];
    c.options = options.isObject() ? options : Value::object();
    if (c.options.contains("timeseries")) c.type = "timeseries";
    return true;
}

Value collectionInfo(const std::string &db, const std::string &name) {
    Lock lk(gMu);
    auto &colls = gDbs[db].colls;
    auto it = colls.find(name);
    if (it == colls.end()) return Value();
    Value opts = it->second.options;
    if (it->second.validator.isObject()) {
        opts["validator"] = it->second.validator;
        opts["validationLevel"] = it->second.validationLevel;
    }
    return Value{{"name", name}, {"type", it->second.type}, {"options", opts}};
}

void setValidator(const std::string &db, const std::string &name, const Value &validator, const std::string &level) {
    Lock lk(gMu);
    Coll &c = coll(db, name);
    c.validator = validator;
    c.validationLevel = level;
}

void transaction(const std::string &db, const std::function<void()> &fn) {
    std::unique_lock<std::recursive_mutex> lk(gMu);  // other threads wait: full isolation
    if (tTxDepth > 0) {                              // nested: join
        fn();
        return;
    }
    Db snapshot = gDbs[db];
    tTxDepth++;
    try {
        fn();
    } catch (...) {
        gDbs[db] = std::move(snapshot);
        tTxDepth--;
        throw;
    }
    tTxDepth--;
}

}  // namespace nevla::mem
