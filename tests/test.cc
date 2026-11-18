// nevla tests: need a real MongoDB.
//   NEVLA_TEST_URI="mongodb://127.0.0.1:27017" ./nevla_tests
// Transactions and change streams need a replica set (they are skipped on a standalone server).
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <nevla/nevla.hpp>
#include <thread>

using namespace nevla;

static int passed = 0, failed = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (cond)                                                                        \
            ++passed;                                                                    \
        else {                                                                           \
            ++failed;                                                                    \
            std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n"; \
        }                                                                                \
    } while (0)
#define CHECK_THROWS(expr, Type)                                                                              \
    do {                                                                                                      \
        bool nv_ok = false;                                                                                   \
        try {                                                                                                 \
            expr;                                                                                             \
        } catch (const Type &) {                                                                              \
            nv_ok = true;                                                                                     \
        } catch (const std::exception &e) {                                                                   \
            std::cout << "  (wrong exception: " << e.what() << ")\n";                                         \
        }                                                                                                     \
        if (nv_ok)                                                                                            \
            ++passed;                                                                                         \
        else {                                                                                                \
            ++failed;                                                                                         \
            std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #expr " did not throw " #Type "\n"; \
        }                                                                                                     \
    } while (0)
static void section(const char *name) { std::cout << "- " << name << "\n"; }

struct Address {
    std::string city;
    int pin = 0;
};
NEVLA_STRUCT(Address, city, pin)

struct UserRow {
    std::string id, name, email;
    int age = 0;
    std::vector<std::string> tags;
    std::optional<std::string> nick;
    Address address;
};
NEVLA_STRUCT(UserRow, id, name, email, age, tags, nick, address)

/* ============================ no database needed ============================ */
static void testValue() {
    section("Value");
    Value v = {{"name", "Araham"},
               {"age", 24},
               {"ok", true},
               {"pi", 3.5},
               {"tags", Value::array({"a", "b"})},
               {"address", {{"city", "Durgapur"}, {"pin", 713402}}}};
    CHECK(v.isObject());
    CHECK(v["name"].asString() == "Araham");
    CHECK(v["age"].asInt() == 24);
    CHECK(v["ok"].asBool());
    CHECK(v["pi"].asDouble() == 3.5);
    CHECK(v["tags"].isArray() && v["tags"].size() == 2);
    CHECK(v["tags"].at(1).asString() == "b");
    CHECK(v["address"]["city"].asString() == "Durgapur");
    CHECK(v.path("address.pin") && v.path("address.pin")->asInt() == 713402);
    CHECK(v.path("address.nahi") == nullptr);
    CHECK(v.contains("age") && !v.contains("xyz"));
    const Value &cv = v;
    CHECK(cv["xyz"].isNull());
    v.pathRef("a.b.c") = 5;
    CHECK(v["a"]["b"]["c"].asInt() == 5);
    v.erasePath("a.b.c");
    CHECK(!v["a"]["b"].contains("c"));
    v.erase("a");
    CHECK(!v.contains("a"));

    Value list = {1, 2, 3};
    CHECK(list.isArray() && list.size() == 3);
    Value pairList = Value::array({"x", 1});
    CHECK(pairList.isArray());
    Value empty = Value::object();
    CHECK(empty.isObject() && empty.empty());

    Value parsed = Value::parse(R"({"a":[1,2,{"b":null}],"s":"hi","d":1.25})");
    CHECK(parsed["a"].at(2)["b"].isNull());
    CHECK(parsed["s"].asString() == "hi");
    CHECK(parsed["d"].asDouble() == 1.25);
    CHECK(Value::parse(parsed.toJson()) == parsed);
    CHECK_THROWS(Value::parse("{galat"), Error);
    CHECK(v["name"].asInt(-1) == -1);
    CHECK(v["name"].asObjectId().empty());

    CHECK(v["age"].as<int>() == 24);
    CHECK(v["name"].as<std::string>() == "Araham");
    CHECK(v["tags"].as<std::vector<std::string>>().size() == 2);
    Value fromVec = std::vector<int>{4, 5};
    CHECK(fromVec.at(0).asInt() == 4);

    ObjectId a = ObjectId::generate(), b = ObjectId::generate();
    CHECK(a.str().size() == 24 && a != b);
    CHECK(ObjectId::isValid(a.str()) && !ObjectId::isValid("xyz"));
    CHECK_THROWS(ObjectId("nahi"), Error);
    Date d = *Date::parse("2026-11-17T10:30:00.000Z");
    CHECK(d.iso() == "2026-11-17T10:30:00.000Z");
    CHECK(Date::now().ms > d.ms - 1000LL * 86400 * 365 * 5);
    Value withDate = {{"at", d}, {"id", a}};
    CHECK(withDate["at"].isDate() && withDate["id"].isObjectId());
    CHECK(withDate.toJson().find("2026-11-17T10:30:00.000Z") != std::string::npos);
}

static void testSchemaOffline() {
    section("Schema: cast + defaults + validation (no DB)");
    Schema s({
        {"name", String().required().trim().minLength(2).maxLength(10)},
        {"email", String().lowercase().match("^[^@]+@[^@]+$", "email galat hai")},
        {"code", String().uppercase()},
        {"age", Int().min(0).max(150)},
        {"score", Number().def(1.5)},
        {"role", String().enumOf({"user", "admin"}).def("user")},
        {"active", Boolean().def(true)},
        {"joined", DateTime()},
        {"even", Int().validate([](const Value &v) { return v.asInt() % 2 == 0; }, "even hona chahiye")},
        {"slug", String().set([](const Value &v) { return Value("s-" + v.asString()); })},
        {"tags", Array(String().lowercase())},
        {"address", Object(Schema({{"city", String().required()}, {"pin", Int()}}))},
        {"items", Array(Schema({{"sku", String().required()}, {"qty", Int().def(1).min(1)}}))},
        {"any", Mixed()},
    });
    auto M = model("OfflineThing", s);

    auto d = M({{"name", "  Araham  "},
                {"email", "A@B.COM"},
                {"code", "ab"},
                {"age", "24"},
                {"active", "false"},
                {"joined", "2026-01-02T03:04:05.000Z"},
                {"slug", "x"},
                {"tags", Value::array({"IoT", "ESP"})},
                {"address", {{"city", "Durgapur"}, {"pin", "713402"}}},
                {"items", Value::array({{{"sku", "A1"}}, {{"sku", "B2"}, {"qty", 3}}})},
                {"any", {{"kuch", "bhi"}}},
                {"faltu", 1}});
    CHECK(d["name"].asString() == "Araham");
    CHECK(d["email"].asString() == "a@b.com");
    CHECK(d["code"].asString() == "AB");
    CHECK(d["age"].isInt() && d["age"].asInt() == 24);
    CHECK(d["active"].isBool() && !d["active"].asBool());
    CHECK(d["joined"].isDate());
    CHECK(d["slug"].asString() == "s-x");
    CHECK(d["tags"].at(0).asString() == "iot");
    CHECK(d["address.pin"].asInt() == 713402);
    CHECK(d["items"].at(0)["qty"].asInt() == 1);  // sub-document default
    CHECK(d["items"].at(1)["qty"].asInt() == 3);
    CHECK(d["any"]["kuch"].asString() == "bhi");
    CHECK(d["score"].asDouble() == 1.5);
    CHECK(d["role"].asString() == "user");
    CHECK(!d.has("faltu"));  // strict
    CHECK(d["_id"].isObjectId() && d.id().size() == 24);
    CHECK(d.isNew());
    d.validate();  // all valid

    // Like Mongoose: a value that cannot be cast is reported by validate() as a ValidationError (kind "cast")
    {
        auto c = M({{"name", "Araham"}, {"email", "a@b.com"}, {"age", "abc"}, {"joined", "kal"}, {"address", 5}});
        try {
            c.validate();
            CHECK(false);
        } catch (const ValidationError &e) {
            CHECK(e.errors.count("age") && e.kinds.at("age") == "cast");
            CHECK(e.errors.count("joined") && e.kinds.at("joined") == "cast");
            CHECK(e.errors.count("address"));
        }
        c.set("age", "abc");
        CHECK_THROWS(c.validate(), ValidationError);
    }

    auto bad = M({{"name", "A"},
                  {"email", "galat"},
                  {"age", 200},
                  {"role", "raja"},
                  {"even", 3},
                  {"address", {{"pin", 1}}},
                  {"items", Value::array({{{"qty", 0}}})}});
    try {
        bad.validate();
        CHECK(false);
    } catch (const ValidationError &e) {
        CHECK(e.errors.count("name"));
        CHECK(e.errors.count("email") && e.errors.at("email") == "email galat hai");
        CHECK(e.errors.count("age"));
        CHECK(e.errors.count("role"));
        CHECK(e.errors.count("even") && e.errors.at("even") == "even hona chahiye");
        CHECK(e.errors.count("address.city"));
        CHECK(e.errors.count("items.0.sku"));
        CHECK(e.errors.count("items.0.qty"));
        CHECK(std::string(e.what()).find("OfflineThing validation failed") == 0);
    }
    auto missing = M({});
    try {
        missing.validate();
        CHECK(false);
    } catch (const ValidationError &e) {
        CHECK(e.errors.size() == 2 && e.errors.count("name") &&
              e.errors.count("address.city"));  // required inside nested objects too
        CHECK(e.errors.at("name") == "Path `name` is required.");
    }

    // change tracking
    auto t = M.hydrate({{"_id", ObjectId::generate()}, {"name", "Ravi"}, {"age", 5}});
    CHECK(!t.isNew() && !t.isModified());
    t.set("age", "6").set("address.city", "Burdwan");
    CHECK(t["age"].asInt() == 6);
    CHECK(t.isModified("age") && t.isModified("address") && t.isModified("address.city") && !t.isModified("name"));
    CHECK(t.modifiedPaths().size() == 2);
    t.push("tags", "NEW");
    CHECK(t["tags"].at(0).asString() == "new");
    t.mut("any")["x"] = 1;
    CHECK(t.isModified("any"));
    t.unset("age");
    CHECK(!t.has("age"));
    t.set("faltu", 1);
    CHECK(!t.has("faltu"));

    // NEVLA_STRUCT
    UserRow u;
    u.name = "Sita";
    u.email = "s@x.in";
    u.age = 30;
    u.tags = {"a", "b"};
    u.address = {"Kolkata", 700001};
    Value uv = toValue(u);
    CHECK(!uv.contains("_id"));  // an empty id is not sent
    CHECK(uv["address"]["city"].asString() == "Kolkata");
    CHECK(uv["nick"].isNull());
    uv["_id"] = ObjectId::generate();
    uv["nick"] = "situ";
    UserRow back = uv.as<UserRow>();
    CHECK(back.id.size() == 24 && back.name == "Sita" && back.age == 30 && back.tags.size() == 2);
    CHECK(back.nick && *back.nick == "situ" && back.address.pin == 700001);
}

/* ============================ DB wale ============================ */
static void testCrud() {
    section("CRUD + query builder");
    Schema s({{"name", String().required().trim()},
              {"email", String().required().lowercase().unique()},
              {"age", Int().min(0)},
              {"role", String().enumOf({"user", "admin"}).def("user")},
              {"password", String().select(false)},
              {"tags", Array(String())},
              {"address", Object(Schema({{"city", String()}, {"pin", Int()}}))},
              {"device", String().immutable()}},
             {.timestamps = true});
    auto User = model("User", s);
    CHECK(User.collectionName() == "users");
    User.dropCollection();

    auto a = User.create({{"name", " Araham "},
                          {"email", "ARAHAM@X.IN"},
                          {"age", 24},
                          {"password", "chhupa"},
                          {"tags", Value::array({"iot", "cpp"})},
                          {"address", {{"city", "Durgapur"}, {"pin", 713402}}},
                          {"device", "d1"}});
    CHECK(!a.isNew() && !a.isModified());
    CHECK(a["createdAt"].isDate() && a["updatedAt"].isDate());
    CHECK(a["__v"].asInt() == 0);
    CHECK(a["email"].asString() == "araham@x.in");

    User.insertMany({{{"name", "Ravi"}, {"email", "ravi@x.in"}, {"age", 17}, {"tags", Value::array({"iot"})}},
                     {{"name", "Sita"}, {"email", "sita@x.in"}, {"age", 31}, {"role", "admin"}},
                     {{"name", "Mohan"}, {"email", "mohan@x.in"}, {"age", 45}}});
    CHECK(User.countDocuments() == 4);
    CHECK(User.estimatedDocumentCount() == 4);
    CHECK(User.countDocuments({{"age", {{"$gte", 18}}}}) == 3);
    CHECK(User.exists({{"email", "ravi@x.in"}}) && !User.exists({{"email", "koi@nahi.in"}}));

    CHECK_THROWS(User.create({{"name", "Dup"}, {"email", "Araham@x.in"}}), DuplicateKeyError);
    CHECK_THROWS(User.create({{"email", "noname@x.in"}}), ValidationError);
    CHECK_THROWS(User.insertMany({{{"name", "ok"}, {"email", "ok@x.in"}}, {{"name", "bad"}}}), ValidationError);
    CHECK(User.countDocuments() == 4);

    // findById: a string id works too, select(false) fields are hidden
    auto got = User.findById(a.id()).exec();
    CHECK(got && (*got)["name"].asString() == "Araham");
    CHECK(got && !got->has("password"));
    auto withPw = User.findById(a._id()).select("+password").exec();
    CHECK(withPw && (*withPw)["password"].asString() == "chhupa");
    auto only = User.findOne({{"email", "ravi@x.in"}}).select("name age").lean();
    CHECK(only && only->contains("name") && only->contains("_id") && !only->contains("email"));
    auto minus = User.findOne({{"email", "ravi@x.in"}}).select("-age -tags").lean();
    CHECK(minus && !minus->contains("age") && minus->contains("email"));
    CHECK(!User.findOne({{"email", "koi@nahi.in"}}).exec());
    CHECK_THROWS(User.findOne({{"email", "koi@nahi.in"}}).orFail(), NotFoundError);
    CHECK(!User.findById("not-an-objectid").exec());

    // filter cast: "24" -> 24
    CHECK(User.find({{"age", "24"}}).count() == 1);
    CHECK(User.find({{"age", {{"$in", Value::array({"17", 45})}}}}).count() == 2);
    CHECK(User.find({{"$or", Value::array({{{"age", {{"$lt", 18}}}}, {{"role", "admin"}}})}}).count() == 2);
    CHECK(User.find({{"tags", "iot"}}).count() == 2);
    CHECK(User.find({{"address.city", "Durgapur"}}).count() == 1);

    // chain
    auto adults = User.find().where("age").gte(18).lt(40).sort("-age").exec();
    CHECK(adults.size() == 2 && adults[0]["name"].asString() == "Sita");
    auto page = User.find().sort({{"age", 1}}).skip(1).limit(2).lean();
    CHECK(page.size() == 2 && page[0]["name"].asString() == "Araham" && page[1]["name"].asString() == "Sita");
    CHECK(User.find().where("role").equals("admin").count() == 1);
    CHECK(User.find().where("role", "user").where("age").gt(20).count() == 2);
    CHECK(User.find().where("name").in({"Ravi", "Sita"}).count() == 2);
    CHECK(User.find().where("name").nin({"Ravi", "Sita"}).count() == 2);
    CHECK(User.find().where("role").ne("admin").count() == 3);
    CHECK(User.find().where("address").exists().count() == 1);
    CHECK(User.find().where("name").regex("^s", "i").count() == 1);
    CHECK(User.find().sort("role -age").limit(1).exec()[0]["name"].asString() == "Sita");

    // save(): sirf badle hue paths
    auto ravi = User.findOne({{"email", "ravi@x.in"}}).orFail();
    Date before = ravi["updatedAt"].asDate();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ravi.set("age", 18).set("address.city", "Burdwan").push("tags", "esp32");
    ravi.save();
    auto ravi2 = User.findById(ravi._id()).orFail();
    CHECK(ravi2["age"].asInt() == 18 && ravi2["address.city"].asString() == "Burdwan");
    CHECK(ravi2["tags"].size() == 2);
    CHECK(ravi2["updatedAt"].asDate().ms > before.ms);
    CHECK(ravi2["createdAt"].asDate().ms == ravi["createdAt"].asDate().ms);
    ravi2.unset("address");
    ravi2.save();
    CHECK(!User.findById(ravi._id()).orFail().has("address"));
    ravi2.set("age", -5);
    CHECK_THROWS(ravi2.save(), ValidationError);

    // immutable
    auto a2 = User.findById(a._id()).orFail();
    a2.set("device", "d2");
    CHECK(a2["device"].asString() == "d1");

    // updates
    auto r = User.updateOne({{"email", "sita@x.in"}}, {{"age", "32"}});  // auto $set + cast
    CHECK(r.matchedCount == 1 && r.modifiedCount == 1);
    CHECK(User.findOne({{"email", "sita@x.in"}}).orFail()["age"].asInt() == 32);
    r = User.updateMany({{"role", "user"}}, {{"$inc", {{"age", 1}}}, {"$addToSet", {{"tags", "bulk"}}}});
    CHECK(r.matchedCount == 3 && r.modifiedCount == 3);
    CHECK(User.find({{"tags", "bulk"}}).count() == 3);
    CHECK_THROWS(User.updateOne({{"email", "sita@x.in"}}, {{"age", -1}}, {.runValidators = true}), ValidationError);
    CHECK_THROWS(User.updateOne({{"email", "sita@x.in"}}, {{"age", "abc"}}), CastError);
    r = User.updateOne({{"email", "naya@x.in"}}, {{"name", "Naya"}}, {.upsert = true});
    CHECK(r.upsertedId.isObjectId());
    auto naya = User.findOne({{"email", "naya@x.in"}}).orFail();
    CHECK(naya["createdAt"].isDate() && naya["updatedAt"].isDate());
    CHECK(naya["role"].asString() == "user");  // upsert par defaults

    auto old = User.findOneAndUpdate({{"email", "mohan@x.in"}}, {{"role", "admin"}});
    CHECK(old && (*old)["role"].asString() == "user");
    auto fresh = User.findOneAndUpdate({{"email", "mohan@x.in"}}, {{"name", "Mohan Lal"}}, {.returnNew = true});
    CHECK(fresh && (*fresh)["name"].asString() == "Mohan Lal" && (*fresh)["role"].asString() == "admin");
    CHECK(fresh && !fresh->has("password"));
    auto byId = User.findByIdAndUpdate(a.id(), {{"$set", {{"age", 25}}}}, {.returnNew = true});
    CHECK(byId && (*byId)["age"].asInt() == 25);
    CHECK(!User.findOneAndUpdate({{"email", "koi@nahi.in"}}, {{"age", 1}}));

    // distinct + aggregate
    CHECK(User.distinct("role").size() == 2);
    CHECK(User.distinct("role", {{"age", {{"$lt", 20}}}}).size() == 1);
    auto agg = User.aggregate(
        Value::array({{{"$group", {{"_id", "$role"}, {"n", {{"$sum", 1}}}}}}, {{"$sort", {{"_id", 1}}}}}));
    CHECK(agg.size() == 2 && agg[0]["_id"].asString() == "admin" && agg[0]["n"].asInt() == 2 &&
          agg[1]["n"].asInt() == 3);

    // delete
    CHECK(User.deleteOne({{"email", "naya@x.in"}}) == 1);
    auto gone = User.findOneAndDelete({{"email", "mohan@x.in"}});
    CHECK(gone && (*gone)["name"].asString() == "Mohan Lal");
    CHECK(User.findByIdAndDelete(ravi.id()).has_value());
    a.deleteOne();
    CHECK(User.countDocuments() == 1);
    CHECK(User.deleteMany() == 1);

    // save() when the document was removed from the DB
    auto ghost = User.create({{"name", "Ghost"}, {"email", "ghost@x.in"}});
    User.deleteMany();
    ghost.set("age", 1);
    CHECK_THROWS(ghost.save(), NotFoundError);

    // typed struct
    UserRow u;
    u.name = "Typed";
    u.email = "typed@x.in";
    u.age = 9;
    u.tags = {"t"};
    u.address = {"Asansol", 713301};
    auto td = User.create(toValue(u));
    UserRow back = User.findById(td._id()).orFail().as<UserRow>();
    CHECK(back.id == td.id() && back.name == "Typed" && back.address.city == "Asansol" && back.tags.size() == 1);
    std::vector<UserRow> rows;
    for (auto &v : User.find().lean()) rows.push_back(v.as<UserRow>());
    CHECK(rows.size() == 1);
    User.dropCollection();
}

static void testHooksVirtualsMethods() {
    section("hooks + virtuals + methods + toJSON");
    std::vector<std::string> log;
    Schema s({{"first", String().required()},
              {"last", String()},
              {"password", String()},
              {"deleted", Boolean().def(false)},
              {"logins", Int().def(0)}},
             {.virtuals = true});
    s.pre("validate", [&](Document &) { log.push_back("pre-validate"); });
    s.post("validate", [&](Document &) { log.push_back("post-validate"); });
    s.pre("save", [&](Document &d) {
        log.push_back(d.isNew() ? "pre-save-new" : "pre-save");
        if (d.isModified("password")) d.set("password", "hash:" + d["password"].asString());
    });
    s.post("save", [&](Document &) { log.push_back("post-save"); });
    s.pre("deleteOne", [&](Document &) { log.push_back("pre-delete"); });
    s.post("deleteOne", [&](Document &) { log.push_back("post-delete"); });
    for (const char *op : {"find", "findOne", "countDocuments"})
        s.pre(op, [](QueryInfo &q) {
            if (!q.filter.contains("deleted")) q.filter["deleted"] = false;  // soft delete
        });
    s.pre("updateOne", [&](QueryInfo &q) {
        log.push_back("pre-updateOne");
        (*q.update)["$inc"]["logins"] = 1;
    });
    s.virtual_("fullName", [](const Document &d) { return Value(d["first"].asString() + " " + d["last"].asString()); });
    s.method("greet", [](Document &d, const Value &args) {
        return Value(args["hello"].asString() + ", " + d["first"].asString() + "!");
    });
    s.toJSON([](const Document &, Value &out) {
        out.erase("password");
        out.erase("__v");
    });
    auto P = model("Person", s);
    P.dropCollection();

    auto p = P.create({{"first", "Araham"}, {"last", "Abeddin"}, {"password", "1234"}});
    CHECK((log == std::vector<std::string>{"pre-validate", "post-validate", "pre-save-new", "post-save"}));
    CHECK(p["password"].asString() == "hash:1234");
    CHECK(p["fullName"].asString() == "Araham Abeddin");
    CHECK(p.call("greet", {{"hello", "Namaste"}}).asString() == "Namaste, Araham!");
    CHECK_THROWS(p.call("nahi"), Error);
    Value j = p.toJSON();
    CHECK(j["fullName"].asString() == "Araham Abeddin" && j["id"].asString() == p.id());
    CHECK(!j.contains("password") && !j.contains("__v"));
    CHECK(p.toObject().contains("password"));

    log.clear();
    p.set("last", "A.").save();
    CHECK(P.findById(p._id()).orFail()["password"].asString() == "hash:1234");  // not hashed again
    CHECK(log.size() == 4 && log[2] == "pre-save");

    P.create({{"first", "Hata"}, {"deleted", true}});
    CHECK(P.find().count() == 1);
    CHECK(P.find().exec().size() == 1);
    CHECK(P.find({{"deleted", true}}).exec().size() == 1);
    CHECK(!P.findOne({{"first", "Hata"}}).exec());

    log.clear();
    P.updateOne({{"_id", p._id()}}, {{"$set", {{"last", "Z"}}}});
    CHECK(log.size() == 1 && P.findById(p._id()).orFail()["logins"].asInt() == 1);

    log.clear();
    p.deleteOne();
    CHECK((log == std::vector<std::string>{"pre-delete", "post-delete"}));
    P.dropCollection();
}

static void testPopulate() {
    section("populate");
    auto Author = model("Author", Schema({{"name", String().required()}, {"secret", String().select(false)}}));
    auto Tag = model("Tag", Schema({{"label", String()}}));
    auto Post = model("Post", Schema({{"title", String().required()},
                                      {"author", Id().ref("Author")},
                                      {"tags", Array(Id().ref("Tag"))},
                                      {"comments", Array(Schema({{"by", Id().ref("Author")}, {"text", String()}}))},
                                      {"plain", String()}}));
    Author.dropCollection();
    Tag.dropCollection();
    Post.dropCollection();
    auto a1 = Author.create({{"name", "Premchand"}, {"secret", "x"}});
    auto a2 = Author.create({{"name", "Tagore"}});
    auto t1 = Tag.create({{"label", "hindi"}}), t2 = Tag.create({{"label", "classic"}});
    ObjectId missing = ObjectId::generate();
    auto p1 = Post.create({{"title", "Godaan"},
                           {"author", a1.id()},  // string id -> ObjectId cast
                           {"tags", Value::array({t1._id(), t2._id(), missing})},
                           {"comments", Value::array({{{"by", a2._id()}, {"text", "waah"}},
                                                      {{"by", a1._id()}, {"text", "dhanyavaad"}}})}});
    Post.create({{"title", "Gitanjali"}, {"author", a2._id()}});
    Post.create({{"title", "Anaam"}});
    Post.create({{"title", "Bhoot"}, {"author", missing}});
    CHECK(p1["author"].isObjectId());

    auto posts = Post.find().sort("title").populate("author").populate("tags").exec();
    CHECK(posts.size() == 4);
    CHECK(posts[0]["title"].asString() == "Anaam" && !posts[0].has("author"));
    CHECK(posts[1]["title"].asString() == "Bhoot" && posts[1]["author"].isNull());  // ref gayab -> null
    CHECK(posts[3]["author.name"].asString() == "Premchand");
    CHECK(!posts[3]["author"].contains("secret"));
    CHECK(posts[3]["tags"].size() == 2 && posts[3]["tags"].at(0)["label"].asString() == "hindi");

    auto one = Post.findById(p1._id()).populate("comments.by").orFail();
    CHECK(one["comments"].at(0)["by"]["name"].asString() == "Tagore");
    CHECK(one["comments"].at(1)["by"]["name"].asString() == "Premchand");
    CHECK(one["author"].isObjectId());
    one.populate("author");
    CHECK(one["author"]["name"].asString() == "Premchand");
    one.populate("author");  // populating twice is harmless
    CHECK(one["author"]["name"].asString() == "Premchand");

    auto lean = Post.findOne({{"title", "Gitanjali"}}).populate("author").lean();
    CHECK(lean && (*lean)["author"]["name"].asString() == "Tagore");
    CHECK_THROWS(Post.find().populate("plain").exec(), Error);

    // filter by ref: a string id is cast
    CHECK(Post.find({{"author", a2.id()}}).count() == 1);
    CHECK(Post.find({{"tags", t1.id()}}).count() == 1);
    Author.dropCollection();
    Tag.dropCollection();
    Post.dropCollection();
}

static void testDiscriminators() {
    section("discriminators");
    auto Device = model("Device", Schema({{"name", String().required()}, {"online", Boolean().def(false)}}));
    auto Sensor = Device.discriminator("Sensor", Schema({{"unit", String().required()}, {"value", Number()}}));
    auto Relay = Device.discriminator("Relay", Schema({{"channels", Int().def(1)}}));
    Device.dropCollection();
    CHECK(Sensor.collectionName() == "devices");

    Device.create({{"name", "generic"}});
    auto s = Sensor.create({{"name", "temp"}, {"unit", "C"}, {"value", 31.5}});
    Relay.create({{"name", "pump"}, {"channels", 4}});
    CHECK(s["__t"].asString() == "Sensor");
    CHECK_THROWS(Sensor.create({{"name", "no-unit"}}), ValidationError);
    CHECK(!Device({{"name", "x"}, {"unit", "C"}}).has("unit"));  // `unit` is not in the base schema

    CHECK(Device.countDocuments() == 3);
    CHECK(Sensor.countDocuments() == 1 && Relay.countDocuments() == 1);
    CHECK(Sensor.find().exec()[0]["unit"].asString() == "C");
    auto all = Device.find().sort("name").exec();
    CHECK(all[0].modelName() == "Device" && all[1].modelName() == "Relay" && all[2].modelName() == "Sensor");
    all[2].set("value", "40.25").save();  // loaded via the base model, cast with the Sensor schema
    CHECK(Sensor.findOne().orFail()["value"].asDouble() == 40.25);
    CHECK(Sensor.aggregate(Value::array({{{"$count", "n"}}}))[0]["n"].asInt() == 1);
    Sensor.updateMany({}, {{"online", true}});
    CHECK(Device.countDocuments({{"online", true}}) == 1);
    CHECK(Relay.deleteMany() == 1);
    CHECK(Device.countDocuments() == 2);
    CHECK(model("Sensor").name() == "Sensor");
    CHECK_THROWS(model("KoiNahi"), Error);
    Device.dropCollection();
}

static void testCustomIdIndexesMixed() {
    section("custom _id + indexes + strict off + options");
    auto Kv = model("Setting", Schema({{"_id", String()}, {"value", Mixed()}}, {.versionKey = ""}), "app_settings");
    Kv.dropCollection();
    CHECK(Kv.collectionName() == "app_settings");
    Kv.create({{"_id", "theme"}, {"value", {{"dark", true}, {"accent", "teal"}}}});
    auto th = Kv.findById("theme").orFail();
    CHECK(th["value.accent"].asString() == "teal" && !th.has("__v"));
    th.mut("value")["accent"] = "red";
    th.save();
    CHECK(Kv.findById("theme").orFail()["value.accent"].asString() == "red");
    CHECK_THROWS(Kv.create({{"_id", "theme"}, {"value", 1}}), DuplicateKeyError);
    Kv.updateOne({{"_id", "lang"}}, {{"value", "hi"}}, {.upsert = true});
    CHECK(Kv.findById("lang").orFail()["value"].asString() == "hi");
    Kv.dropCollection();

    auto U = model("UuidThing", Schema({{"_id", String().def([] { return Value(uuid()); })}, {"n", Int()}}));
    U.dropCollection();
    auto u1 = U.create({{"n", 1}}), u2 = U.create({{"n", 2}});
    CHECK(u1.id().size() == 36 && u1.id() != u2.id());
    U.dropCollection();

    Schema rs({{"device", String().index()}, {"at", DateTime()}, {"v", Number()}});
    rs.index({{"device", 1}, {"at", -1}}, {{"unique", true}});
    rs.index({{"at", 1}}, {{"expireAfterSeconds", 3600}});
    auto R = model("Reading", rs);
    R.dropCollection();
    Date now = Date::now();
    R.create({{"device", "d1"}, {"at", now}, {"v", 1}});
    CHECK_THROWS(R.create({{"device", "d1"}, {"at", now}, {"v", 2}}), DuplicateKeyError);
    auto idx = R.aggregate(Value::array({{{"$indexStats", Value::object()}}}));
    CHECK(idx.size() == 4);  // _id, device, device+at, at(TTL)
    R.dropCollection();

    auto Loose = model("Loose", Schema({{"a", Int()}}, {.strict = false}));
    Loose.dropCollection();
    auto l = Loose.create({{"a", "1"}, {"kuchBhi", {{"x", 1}}}});
    l.set("aurBhi", "haan").save();
    auto l2 = Loose.findById(l._id()).orFail();
    CHECK(l2["a"].asInt() == 1 && l2["kuchBhi.x"].asInt() == 1 && l2["aurBhi"].asString() == "haan");
    Loose.dropCollection();
}

static void testTransactions() {
    section("transactions");
    auto Acc = model("Account", Schema({{"owner", String().unique()}, {"balance", Int().min(0)}}));
    Acc.dropCollection();
    Acc.create({{"owner", "a"}, {"balance", 100}});
    Acc.create({{"owner", "b"}, {"balance", 0}});
    try {
        transaction([&] {
            Acc.updateOne({{"owner", "a"}}, {{"$inc", {{"balance", -40}}}});
            Acc.updateOne({{"owner", "b"}}, {{"$inc", {{"balance", 40}}}});
        });
    } catch (const Error &e) {
        std::cout << "  (skip: transactions ke liye replica set chahiye: " << e.what() << ")\n";
        Acc.dropCollection();
        return;
    }
    CHECK(Acc.findOne({{"owner", "a"}}).orFail()["balance"].asInt() == 60);
    CHECK(Acc.findOne({{"owner", "b"}}).orFail()["balance"].asInt() == 40);

    CHECK_THROWS(transaction([&] {
                     Acc.updateOne({{"owner", "a"}}, {{"$inc", {{"balance", -10}}}});
                     Acc.create({{"owner", "c"}, {"balance", 5}});
                     CHECK(Acc.countDocuments() == 3);  // visible inside the transaction
                     throw Error("beech me gadbad");
                 }),
                 Error);
    CHECK(Acc.findOne({{"owner", "a"}}).orFail()["balance"].asInt() == 60);  // rollback
    CHECK(Acc.countDocuments() == 2);
    Acc.dropCollection();
}

static void testThreads() {
    section("threads + async");
    auto C = model("Counter", Schema({{"_id", String()}, {"n", Int().def(0)}}));
    C.dropCollection();
    C.create({{"_id", "hits"}});
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 50; ++i) C.updateOne({{"_id", "hits"}}, {{"$inc", {{"n", 1}}}});
        });
    for (auto &t : ts) t.join();
    CHECK(C.findById("hits").orFail()["n"].asInt() == 400);
    auto fut = async([&] { return C.countDocuments(); });
    CHECK(fut.get() == 1);
    C.dropCollection();
}

/* ============================ v0.2 features ============================ */
static void testSchemaFeaturesOffline() {
    section("Schema v0.2: messages, kinds, Date min/max, Map, alias, getters (no DB)");
    Schema s(
        {
            {"name", String().required(true, "{PATH} is mandatory").alias("n")},
            {"age", Int().min(18, "{PATH} must be 18+, got {VALUE}").max(99)},
            {"role", String().enumOf({"user", "admin"}, "{VALUE} is not a role")},
            {"born", DateTime().min(*Date::parse("1900-01-01")).max(*Date::parse("2030-01-01"))},
            {"prices", Map(Number().min(0))},
            {"price", Number().get([](const Value &v) { return Value(v.asDouble() / 100.0); })},
            {"code", String().match("^[A-Z]{3}$")},
        },
        {.getters = true});
    auto M = model("FeatureThing", s);
    auto d =
        M({{"n", "Asha"}, {"age", 20}, {"prices", {{"inr", "100"}, {"usd", 2}}}, {"price", 1250}, {"code", "ABC"}});
    CHECK(d["name"].asString() == "Asha");   // alias -> real field
    CHECK(d.get("n").asString() == "Asha");  // read via alias
    CHECK(d["prices.inr"].isNumber() && d["prices.inr"].asDouble() == 100);
    CHECK(d.get("price").asDouble() == 12.5);  // getter
    CHECK(d["price"].asInt() == 1250);         // raw value untouched
    CHECK(d.toJSON()["price"].asDouble() == 12.5);
    d.validate();
    d.set("n", "Ravi");
    CHECK(d["name"].asString() == "Ravi" && d.isModified("name"));

    auto bad = M({{"age", 10}, {"role", "king"}, {"born", "1800-05-05"}, {"prices", {{"inr", -1}}}, {"code", "abcd"}});
    try {
        bad.validate();
        CHECK(false);
    } catch (const ValidationError &e) {
        CHECK(e.errors.at("name") == "name is mandatory");
        CHECK(e.kinds.at("name") == "required");
        CHECK(e.errors.at("age") == "age must be 18+, got 10");
        CHECK(e.kinds.at("age") == "min");
        CHECK(e.errors.at("role") == "king is not a role");
        CHECK(e.kinds.at("role") == "enum");
        CHECK(e.kinds.at("born") == "min");
        CHECK(e.errors.count("prices.inr"));
        CHECK(e.kinds.at("code") == "regexp");
    }
    CHECK_THROWS(M({{"prices", {{"$bad", 1}}}}).validate(), ValidationError);  // Map keys cannot start with '$'
    CHECK_THROWS(String().match("(unclosed"), Error);                          // invalid regex fails early
    CHECK(!Date::parse("2026-02-30"));                                         // invalid calendar date
    CHECK(Date::parse("2024-02-29").has_value());                              // leap year ok

    // plugin + static + remove
    Schema p({{"title", String()}, {"tmp", String()}});
    p.plugin([](Schema &sc, const Value &opt) { sc.add(opt["field"].asString(), Boolean().def(false)); },
             Value{{"field", "deleted"}});
    p.remove("tmp");
    CHECK(p.field("deleted") && !p.field("tmp"));
}

static void testUpdateCasting() {
    section("update casting: strict, immutable, $inc, $pull, runValidators, custom timestamps");
    Schema s({{"name", String().required()},
              {"code", String().immutable()},
              {"n", Int().def(0)},
              {"tags", Array(String().lowercase())},
              {"items", Array(Schema({{"sku", String()}, {"qty", Int()}}))},
              {"label", String().alias("l")}},
             {.timestamps = true, .createdAt = "created_at", .updatedAt = "updated_at"});
    auto T = model("UpdThing", s);
    T.dropCollection();
    auto d = T.create({{"name", "a"},
                       {"code", "X1"},
                       {"tags", Value::array({"A"})},
                       {"items", Value::array({{{"sku", "s1"}, {"qty", 1}}})}});
    CHECK(d.has("created_at") && d.has("updated_at") && !d.has("createdAt"));

    T.updateOne({{"_id", d._id()}}, {{"junk", 1}, {"code", "Y"}, {"$inc", {{"n", "5"}}}, {"l", "hi"}});
    auto r = T.findById(d._id()).lean().value();
    CHECK(!r.contains("junk"));           // strict: unknown path dropped
    CHECK(r["code"].asString() == "X1");  // immutable: not changed
    CHECK(r["n"].isInt() && r["n"].asInt() == 5);
    CHECK(r["label"].asString() == "hi");  // alias in update
    CHECK(r["updated_at"].isDate());

    T.updateOne({{"_id", d._id()}}, {{"$set", {{"items.$[].qty", "7"}}}});
    CHECK(T.findById(d._id()).lean().value()["items"].at(0)["qty"].asInt() == 7);  // positional path cast

    T.updateOne({{"_id", d._id()}}, {{"$pull", {{"tags", "A"}}}});  // "A" -> "a" (lowercase) -> pulled
    CHECK(T.findById(d._id()).lean().value()["tags"].empty());

    CHECK_THROWS(T.updateOne({{"_id", d._id()}}, {{"$inc", {{"name", 1}}}}), CastError);
    CHECK_THROWS(T.updateOne({{"_id", d._id()}}, {{"$inc", {{"n", "abc"}}}}), CastError);
    CHECK_THROWS(T.updateOne({{"_id", d._id()}}, {{"$unset", {{"name", ""}}}}, {.runValidators = true}),
                 ValidationError);
    auto up = T.updateOne({{"name", "new"}}, {{"code", "Z9"}}, {.upsert = true});
    CHECK(!up.upsertedId.isNull());
    auto created = T.findById(up.upsertedId).lean().value();
    CHECK(created["code"].asString() == "Z9");  // immutable on upsert -> $setOnInsert
    CHECK(created["n"].asInt() == 0);           // default on insert
    CHECK(created["created_at"].isDate());

    CHECK(model("Category", Schema()).collectionName() == "categories");
    CHECK(model("Person", Schema()).collectionName() == "people");
    CHECK(model("Box", Schema()).collectionName() == "boxes");
    CHECK(model("Status", Schema()).collectionName() == "status");
    T.dropCollection();
}

static void testQueryFeatures() {
    section("query: or/and/nor, elemMatch, size, all, collation, cursor, paginate, distinct");
    auto Q = model("QItem", Schema({{"name", String()},
                                    {"n", Int()},
                                    {"tags", Array(String())},
                                    {"parts", Array(Schema({{"k", String()}, {"v", Int()}}))}}));
    Q.dropCollection();
    std::vector<Value> rows;
    for (int i = 1; i <= 25; i++)
        rows.push_back({{"name", i % 2 ? "Odd" : "even"},
                        {"n", i},
                        {"tags", i % 3 ? Value::array({"a"}) : Value::array({"a", "b"})},
                        {"parts", Value::array({{{"k", "x"}, {"v", i}}})}});
    Q.insertMany(rows);

    CHECK(Q.find().or_({{{"n", 1}}, {{"n", "2"}}}).count() == 2);
    CHECK(Q.find().and_({{{"n", {{"$gte", 5}}}}, {{"n", {{"$lte", 6}}}}}).count() == 2);
    CHECK(Q.find().nor({{{"name", "Odd"}}}).count() == 12);
    CHECK(Q.where("parts").elemMatch({{"k", "x"}, {"v", {{"$gt", "20"}}}}).count() == 5);
    CHECK(Q.where("tags").size(2).count() == 8);
    CHECK(Q.where("tags").all({"a", "b"}).count() == 8);
    CHECK(Q.find().where({{"name", "Odd"}}).where("n").lt(5).count() == 2);
    CHECK(Q.find({{"name", "odd"}}).collation({{"locale", "en"}, {"strength", 2}}).count() == 13);
    CHECK(Q.find().sort("-n").limit(1).maxTimeMS(5000).comment("t").hint("_id_").exec()[0]["n"].asInt() == 25);

    int seen = 0;
    auto cur = Q.find({{"n", {{"$lte", 10}}}}).sort("n").batchSize(3).cursor();
    while (auto doc = cur.next()) {
        ++seen;
        CHECK(doc->isNew() == false);
    }
    CHECK(seen == 10);
    CHECK(!cur.next());
    int sum = 0;
    Q.find().forEach([&](Document &doc) { sum += static_cast<int>(doc["n"].asInt()); });
    CHECK(sum == 325);

    auto pg = Q.find().sort("n").paginate(3, 10);
    CHECK(pg.total == 25 && pg.pages == 3 && pg.docs.size() == 5 && !pg.hasNext && pg.hasPrev);
    CHECK(pg.docs[0]["n"].asInt() == 21);
    CHECK(Q.find({{"n", {{"$lte", 4}}}}).distinct("name").size() == 2);
    CHECK(Q.exists({{"n", 3}}) && !Q.exists({{"n", 300}}));
    CHECK_THROWS(Q.find({{"n", "abc"}}).count(), CastError);
    Q.dropCollection();
}

static void testPopulateFeatures() {
    section("populate options, nested populate, virtual populate, depopulate");
    auto Company = model("PCompany", Schema({{"name", String()}}));
    Schema writer({{"name", String()}, {"company", Id().ref("PCompany")}});
    writer.virtualPopulate("articles", {.ref = "PArticle", .localField = "_id", .foreignField = "writer"});
    writer.virtualPopulate("latest",
                           {.ref = "PArticle", .localField = "_id", .foreignField = "writer", .justOne = true});
    auto Writer = model("PWriter", writer);
    auto Article = model("PArticle", Schema({{"title", String()},
                                             {"n", Int()},
                                             {"writer", Id().ref("PWriter")},
                                             {"refs", Array(Id().ref("PWriter"))}}));
    Company.dropCollection();
    Writer.dropCollection();
    Article.dropCollection();
    auto c = Company.create({{"name", "Acme"}});
    auto w1 = Writer.create({{"name", "Ana"}, {"company", c._id()}});
    auto w2 = Writer.create({{"name", "Bo"}, {"company", c._id()}});
    for (int i = 1; i <= 3; i++)
        Article.create({{"title", "A" + std::to_string(i)},
                        {"n", i},
                        {"writer", w1._id()},
                        {"refs", Value::array({w2._id(), w1._id()})}});

    PopulateOptions po;
    po.path = "writer";
    po.select = "name company";
    PopulateOptions nested;
    nested.path = "company";
    po.populate.push_back(nested);
    auto arts = Article.find().sort("n").populate(po).exec();
    CHECK(arts.size() == 3);
    CHECK(arts[0]["writer.name"].asString() == "Ana");
    CHECK(arts[0]["writer.company.name"].asString() == "Acme");  // nested populate
    CHECK(arts[0].populated("writer"));

    PopulateOptions refs;
    refs.path = "refs";
    refs.sort = "name";
    refs.limit = 1;
    auto a = Article.findOne({{"n", 1}}).populate(refs).orFail();
    CHECK(a["refs"].size() == 1 && a["refs"].at(0)["name"].asString() == "Ana");

    PopulateOptions m;
    m.path = "writer";
    m.match = {{"name", "Nobody"}};
    CHECK(Article.findOne({{"n", 1}}).populate(m).orFail()["writer"].isNull());

    // virtual populate
    PopulateOptions va;
    va.path = "articles";
    va.sort = "-n";
    va.limit = 2;
    auto w = Writer.findById(w1._id()).populate(va).populate("latest").orFail();
    CHECK(w["articles"].size() == 2 && w["articles"].at(0)["n"].asInt() == 3);
    CHECK(w["latest"].isObject());
    CHECK(Writer.findById(w2._id()).populate("articles").orFail()["articles"].empty());

    // saving a populated document stores ids, not whole documents
    auto doc = Article.findOne({{"n", 2}}).populate("writer").orFail();
    doc.set("title", "changed");
    doc.mut("writer");  // even if the populated path is marked modified
    doc.save();
    auto raw = Article.findOne({{"n", 2}}).lean().value();
    CHECK(raw["writer"].isObjectId());
    doc.depopulate("writer");
    CHECK(doc["writer"].isObjectId() && !doc.populated("writer"));

    Company.dropCollection();
    Writer.dropCollection();
    Article.dropCollection();
}

static void testStaticsHooksDocs() {
    section("statics, post query hooks, aggregate hooks, document helpers, versioning");
    Schema s({{"email", String().lowercase()}, {"tags", Array(String())}, {"n", Int().def(0)}});
    s.static_("byEmail", [](const Model &M, const Value &email) {
        auto d = M.findOne({{"email", email}}).lean();
        return d ? *d : Value();
    });
    int postFind = 0, postUpdate = 0;
    s.post("find", [&](QueryInfo &, Value &result) {
        postFind++;
        for (auto &r : result.elements()) r["seen"] = true;
    });
    s.post("updateOne", [&](QueryInfo &q, Value &result) {
        postUpdate += static_cast<int>(result["matchedCount"].asInt());
        CHECK(q.update != nullptr);
    });
    s.pre("aggregate", [](QueryInfo &q) { q.filter.push_back(Value{{"$limit", 1}}); });
    auto S = model("SThing", s, "sthings");
    S.dropCollection();
    auto a = S.create({{"email", "A@X.COM"}});
    auto b = S.create({{"email", "b@x.com"}});
    CHECK(S.call("byEmail", "a@x.com")["_id"] == a._id());
    CHECK_THROWS(S.call("nope"), Error);
    auto rows = S.find().lean();
    CHECK(postFind == 1 && rows[0]["seen"].asBool());
    S.updateOne({{"_id", a._id()}}, {{"n", 1}});
    CHECK(postUpdate == 1);
    CHECK(S.aggregate(Value::array({Value{{"$match", Value::object()}}})).size() == 1);

    // document helpers
    auto a2 = S.findById(a._id()).orFail();
    CHECK(a2.equals(a) && !a2.equals(b));
    a2.updateOne({{"$inc", {{"n", 10}}}});
    CHECK(S.findById(a._id()).lean().value()["n"].asInt() == 11);

    // __v goes up when an array changes or increment() is called
    auto v = S.findById(b._id()).orFail();
    CHECK(v["__v"].asInt() == 0);
    v.push("tags", "x");
    v.save();
    CHECK(v["__v"].asInt() == 1 && S.findById(b._id()).lean().value()["__v"].asInt() == 1);
    v.increment().save();
    CHECK(S.findById(b._id()).lean().value()["__v"].asInt() == 2);

    v.overwrite({{"email", "c@x.com"}});
    v.save();
    auto ov = S.findById(b._id()).lean().value();
    CHECK(ov["email"].asString() == "c@x.com" && !ov.contains("tags") && ov["_id"] == b._id());

    // optimistic concurrency
    Schema oc({{"qty", Int()}}, {.optimisticConcurrency = true});
    auto O = model("OcThing", oc);
    O.dropCollection();
    auto o = O.create({{"qty", 1}});
    auto copy1 = O.findById(o._id()).orFail();
    auto copy2 = O.findById(o._id()).orFail();
    copy1.set("qty", 2).save();
    CHECK(copy1["__v"].asInt() == 1);
    copy2.set("qty", 3);
    CHECK_THROWS(copy2.save(), VersionError);
    CHECK(O.findById(o._id()).lean().value()["qty"].asInt() == 2);
    O.findById(o._id()).orFail().deleteOne();
    copy1.set("qty", 9);
    CHECK_THROWS(copy1.save(), NotFoundError);
    O.dropCollection();
    S.dropCollection();
}

static void testModelExtras() {
    section("replaceOne, findOneAndReplace, bulkWrite, watch, indexes, debug, reconnect");
    Schema s({{"sku", String().unique()}, {"qty", Int().min(0)}, {"note", String()}}, {.timestamps = true});
    auto B = model("BItem", s);
    B.dropCollection();
    B.create({{"sku", "a"}, {"qty", 1}, {"note", "x"}});

    auto rr = B.replaceOne({{"sku", "a"}}, {{"sku", "a"}, {"qty", "5"}, {"junk", 1}});
    CHECK(rr.matchedCount == 1);
    auto rep = B.findOne({{"sku", "a"}}).lean().value();
    CHECK(rep["qty"].asInt() == 5 && !rep.contains("note") && !rep.contains("junk") && rep["updatedAt"].isDate());
    CHECK_THROWS(B.replaceOne({{"sku", "a"}}, {{"$set", {{"qty", 1}}}}), Error);
    auto fr = B.findOneAndReplace({{"sku", "a"}}, {{"sku", "a"}, {"qty", 6}}, {.returnNew = true});
    CHECK(fr && (*fr)["qty"].asInt() == 6);

    auto bw = B.bulkWrite({
        {{"insertOne", {{"document", {{"sku", "b"}, {"qty", 2}}}}}},
        {{"insertOne", {{"document", {{"sku", "c"}, {"qty", 3}}}}}},
        {{"updateOne", {{"filter", {{"sku", "b"}}}, {"update", {{"$inc", {{"qty", 1}}}}}}}},
        {{"updateMany", {{"filter", Value::object()}, {"update", {{"note", "bulk"}}}}}},
        {{"updateOne", {{"filter", {{"sku", "z"}}}, {"update", {{"qty", 9}}}, {"upsert", true}}}},
        {{"deleteOne", {{"filter", {{"sku", "c"}}}}}},
    });
    CHECK(bw.insertedCount == 2 && bw.deletedCount == 1 && bw.upsertedCount == 1);
    CHECK(B.findOne({{"sku", "b"}}).lean().value()["qty"].asInt() == 3);
    CHECK(B.countDocuments({{"note", "bulk"}}) == 2);
    CHECK_THROWS(B.bulkWrite({{{"explode", Value::object()}}}), Error);
    CHECK_THROWS(B.bulkWrite({{{"insertOne", {{"document", {{"qty", -1}}}}}}}), ValidationError);

    // insertMany ordered=false keeps going after a duplicate
    try {
        B.insertMany({{{"sku", "a"}}, {{"sku", "u1"}}, {{"sku", "u2"}}}, {.ordered = false});
        CHECK(false);
    } catch (const DuplicateKeyError &) {
        CHECK(B.countDocuments({{"sku", {{"$in", Value::array({"u1", "u2"})}}}}) == 2);
    }

    // debug callback
    std::vector<std::string> ops;
    setDebug([&](const std::string &coll, const std::string &op, const Value &) { ops.push_back(coll + "." + op); });
    B.countDocuments();
    setDebug(false);
    CHECK(!ops.empty() && ops.back() == "bitems.countDocuments");

    // a failing automatic index does not break the model
    auto Dup = model("DupThing", Schema({{"k", String()}}), "dup_things");
    Dup.dropCollection();
    Dup.create({{"k", "same"}});
    Dup.create({{"k", "same"}});
    std::string indexErr;
    onIndexError([&](const std::string &model, const std::string &msg) { indexErr = model + ": " + msg; });
    auto Dup2 = model("DupThing2", Schema({{"k", String().unique()}}), "dup_things");
    CHECK(Dup2.countDocuments() == 2);  // works even though the unique index failed
    CHECK(indexErr.find("DupThing2") == 0);
    CHECK_THROWS(Dup2.createIndexes(), DuplicateKeyError);
    onIndexError(nullptr);

    // syncIndexes drops indexes that are no longer in the schema
    Schema withIdx({{"a", Int().index()}, {"b", Int().index()}, {"exp", DateTime().expires(3600)}});
    auto I1 = model("IdxThing", withIdx, "idx_things");
    I1.dropCollection();
    I1.createIndexes();
    bool ttl = false;
    for (auto &ix : I1.listIndexes()) ttl |= ix["expireAfterSeconds"].asInt() == 3600;
    CHECK(ttl);
    CHECK(I1.listIndexes().size() == 4);
    auto I2 = model("IdxThing2", Schema({{"a", Int().index()}}), "idx_things");
    I2.syncIndexes();
    CHECK(I2.listIndexes().size() == 2);  // _id_ + a_1
    I1.dropCollection();
    Dup.dropCollection();

    // change stream (replica set only)
    try {
        auto stream = B.watch(Value::array(), {.fullDocument = true, .maxAwaitMS = 500});
        B.create({{"sku", "w1"}, {"qty", 1}});
        std::optional<Value> ev;
        for (int i = 0; i < 10 && !ev; i++) ev = stream.next();
        CHECK(ev && (*ev)["operationType"].asString() == "insert" && (*ev)["fullDocument"]["sku"].asString() == "w1");
    } catch (const Error &e) {
        std::cout << "  (watch skipped: " << e.what() << ")\n";
    }

    // reconnect while other threads keep working
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0}, okOps{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++)
        ts.emplace_back([&] {
            while (!stop) {
                try {
                    B.countDocuments();
                    okOps++;
                } catch (const Error &) {
                    errors++;
                }
            }
        });
    const char *env = std::getenv("NEVLA_TEST_URI");
    std::string uri = env ? env : "mongodb://127.0.0.1:27017/?directConnection=true";
    for (int i = 0; i < 5; i++) connect(uri, "nevla_test");
    stop = true;
    for (auto &t : ts) t.join();
    CHECK(errors == 0 && okOps > 0);
    CHECK(databaseName() == "nevla_test");
    ping();
    B.dropCollection();
}

/* ============================ aggregation builder ============================ */
static void testAggregateBuilderOffline() {
    section("aggregate builder: pipeline + auto-cast (no DB)");
    auto Reading = model("AggReadingOffline", Schema({{"device", Id()},
                                                      {"value", Number()},
                                                      {"at", DateTime()},
                                                      {"tags", Array(String())},
                                                      {"temp", Number().alias("t")}}));
    std::string id = ObjectId::generate().str();
    auto a = Reading.aggregate()
                 .match({{"device", id}, {"at", {{"$gte", "2026-11-17"}}}, {"value", {{"$gt", "10"}}}})
                 .group("device", {avg("value"), max("value"), count(), sum("t").as("total")})
                 .sort("-avgValue")
                 .limit(5);
    Value p = a.pipeline();
    CHECK(p.size() == 4);
    const Value &m = p.at(0)["$match"];
    CHECK(m["device"].isObjectId() && m["device"].str() == id);  // string -> ObjectId
    CHECK(m["at"]["$gte"].isDate());                             // string -> Date
    CHECK(m["value"]["$gt"].isInt() && m["value"]["$gt"].asInt() == 10);
    const Value &g = p.at(1)["$group"];
    CHECK(g["_id"].asString() == "$device");
    CHECK(g["avgValue"]["$avg"].asString() == "$value");
    CHECK(g["maxValue"]["$max"].asString() == "$value");
    CHECK(g["count"]["$sum"].asInt() == 1);
    CHECK(g["total"]["$sum"].asString() == "$temp");  // alias resolved + .as() name
    CHECK(p.at(2)["$sort"]["avgValue"].asInt() == -1);
    CHECK(p.at(3)["$limit"].asInt() == 5);
    CHECK(a.toJson().find("$group") != std::string::npos);

    // several group keys, whole-collection group, names from dotted paths
    Value g2 = Reading.aggregate().group("device tags", {avg("reading.temp")}).pipeline().at(0)["$group"];
    CHECK(g2["_id"]["device"].asString() == "$device" && g2["_id"]["tags"].asString() == "$tags");
    CHECK(g2.contains("avgReadingTemp"));
    CHECK(Reading.aggregate().group("", {count()}).pipeline().at(0)["$group"]["_id"].isNull());

    // after $group the schema no longer applies: no casting
    Value after = Reading.aggregate().group("device", {count()}).match({{"count", {{"$gt", "3"}}}}).pipeline();
    CHECK(after.at(1)["$match"]["count"]["$gt"].isString());

    // project / unwind / addFields / count / stage
    Value q = Reading.aggregate().unwind("tags").project("value tags -_id").addFields({{"x", 1}}).count("n").pipeline();
    CHECK(q.at(0)["$unwind"].asString() == "$tags");
    CHECK(q.at(1)["$project"]["value"].asInt() == 1 && q.at(1)["$project"]["_id"].asInt() == 0);
    CHECK(q.at(3)["$count"].asString() == "n");
    CHECK(Reading.aggregate().unwind("tags", true).pipeline().at(0)["$unwind"]["preserveNullAndEmptyArrays"].asBool());
    CHECK(Reading.aggregate().stage({{"$match", {{"device", id}}}}).pipeline().at(0)["$match"]["device"].isObjectId());

    // mistakes are caught early with a clear message
    CHECK_THROWS(Reading.aggregate().match({{"value", "abc"}}), CastError);
    CHECK_THROWS(Reading.aggregate().group("device", {avg("value"), avg("value")}), Error);
    CHECK_THROWS(Reading.aggregate().limit(0), Error);
    CHECK_THROWS(Reading.aggregate().stage({{"notAStage", 1}}), Error);
    CHECK_THROWS(Reading.aggregate().sort(""), Error);
    CHECK_THROWS(Reading.aggregate().count("a.b"), Error);
}

struct DeviceStat {
    std::string id;
    double avgValue = 0, maxValue = 0;
    int count = 0;
};
NEVLA_STRUCT(DeviceStat, id, avgValue, maxValue, count)

static void testAggregateBuilderDb() {
    section("aggregate builder: real data");
    auto Dev = model("AggDevice", Schema({{"name", String()}, {"room", String()}}));
    auto Reading = model(
        "AggReading",
        Schema({{"device", Id().ref("AggDevice")}, {"value", Number()}, {"at", DateTime()}, {"tags", Array(String())}},
               {.versionKey = ""}));
    Dev.dropCollection();
    Reading.dropCollection();
    auto d1 = Dev.create({{"name", "temp-1"}, {"room", "lab"}});
    auto d2 = Dev.create({{"name", "temp-2"}, {"room", "hall"}});
    std::vector<Value> rows;
    for (int i = 0; i < 10; i++) {
        rows.push_back({{"device", d1.id()},
                        {"value", 20 + i},
                        {"at", "2026-11-17T10:0" + std::to_string(i) + ":00Z"},
                        {"tags", Value::array({"t", i % 2 ? "odd" : "even"})}});
        rows.push_back({{"device", d2.id()},
                        {"value", 30 + i},
                        {"at", "2026-11-18T10:0" + std::to_string(i) + ":00Z"},
                        {"tags", Value::array({"t"})}});
    }
    Reading.insertMany(rows);

    // string id + date string in $match work without ObjectId(...) / Date::parse(...)
    auto stats = Reading.aggregate()
                     .match({{"device", d1.id()}, {"at", {{"$gte", "2026-11-17"}}}})
                     .group("device", {avg("value"), max("value"), min("value"), count()})
                     .exec();
    CHECK(stats.size() == 1);
    CHECK(stats[0]["avgValue"].asDouble() == 24.5 && stats[0]["maxValue"].asInt() == 29);
    CHECK(stats[0]["minValue"].asInt() == 20 && stats[0]["count"].asInt() == 10);
    CHECK(stats[0]["_id"] == d1._id());

    // all devices, sorted, into a struct
    auto typed =
        Reading.aggregate().group("device", {avg("value"), max("value"), count()}).sort("-avgValue").exec<DeviceStat>();
    CHECK(typed.size() == 2 && typed[0].id == d2.id() && typed[0].avgValue == 34.5 && typed[0].count == 10);

    // the same pipeline written by hand: $match is cast now too
    auto raw = Reading.aggregate(Value::array({{{"$match", {{"device", d2.id()}}}}, {{"$count", "n"}}}));
    CHECK(raw.size() == 1 && raw[0]["n"].asInt() == 10);

    // lookup by model name + unwind + project
    auto joined = Reading.aggregate()
                      .match({{"value", {{"$gte", 38}}}})
                      .lookup("AggDevice", "device", "_id", "dev")
                      .unwind("dev")
                      .project({{"_id", 0}, {"value", 1}, {"room", "$dev.room"}})
                      .sort("value")
                      .exec();
    CHECK(joined.size() == 2 && joined[0]["room"].asString() == "hall" && joined[1]["value"].asInt() == 39);

    // unwind array + group by the element
    auto byTag = Reading.aggregate().unwind("tags").group("tags", {count()}).sort("-count _id").exec();
    CHECK(byTag.size() == 3 && byTag[0]["_id"].asString() == "t" && byTag[0]["count"].asInt() == 20);

    // whole collection, first(), count(), skip/limit
    auto total = Reading.aggregate().group("", {sum("value"), count()}).first();
    CHECK(total && (*total)["sumValue"].asInt() == 245 + 345 && (*total)["count"].asInt() == 20);
    CHECK(Reading.aggregate().match({{"value", {{"$lt", 25}}}}).count().first().value()["count"].asInt() == 5);
    CHECK(Reading.aggregate().sort("value").skip(18).limit(5).exec().size() == 2);
    CHECK(!Reading.aggregate().match({{"value", 1000}}).first());
    CHECK(Reading.aggregate().sample(3).exec().size() == 3);

    // push / addToSet / first / last with sort
    auto pushed = Reading.aggregate()
                      .match({{"device", d1.id()}})
                      .sort("at")
                      .group("device", {first("value"), last("value"), push("value").as("all"), addToSet("tags")})
                      .first()
                      .value();
    CHECK(pushed["firstValue"].asInt() == 20 && pushed["lastValue"].asInt() == 29 && pushed["all"].size() == 10);

    // options + raw stage ($facet)
    auto facet =
        Reading.aggregate()
            .stage({{"$facet",
                     {{"low", Value::array({{{"$match", {{"value", {{"$lt", 30}}}}}}, {{"$count", "n"}}})},
                      {"high", Value::array({{{"$match", {{"value", {{"$gte", 30}}}}}}, {{"$count", "n"}}})}}}})
            .options({.allowDiskUse = true, .maxTimeMS = 5000})
            .exec();
    CHECK(facet.size() == 1 && facet[0]["low"].at(0)["n"].asInt() == 10 && facet[0]["high"].at(0)["n"].asInt() == 10);

    // hooks see the builder's pipeline too
    Schema hs({{"v", Int()}});
    hs.pre("aggregate", [](QueryInfo &q) { q.filter.push_back(Value{{"$limit", 1}}); });
    auto H = model("AggHooked", hs);
    H.dropCollection();
    H.insertMany({{{"v", 1}}, {{"v", 2}}, {{"v", 3}}});
    CHECK(H.aggregate().sort("v").exec().size() == 1);
    H.dropCollection();

    // discriminators: the builder only sees its own type
    auto Base = model("AggBase", Schema({{"n", Int()}}));
    auto Kid = Base.discriminator("AggKid", Schema({{"extra", String()}}));
    Base.dropCollection();
    Base.create({{"n", 1}});
    Kid.create({{"n", 2}, {"extra", "x"}});
    CHECK(Kid.aggregate().group("", {sum("n")}).first().value()["sumN"].asInt() == 2);
    CHECK(Base.aggregate().group("", {sum("n")}).first().value()["sumN"].asInt() == 3);
    Base.dropCollection();

    CHECK_THROWS(Reading.aggregate(Value::object()), Error);  // a pipeline must be an array
    Dev.dropCollection();
    Reading.dropCollection();
}

/* ============================ v0.4: formulas, buckets, shortcuts, pipeline populate ============================ */
static void testFormulasOffline() {
    section("formula strings (no DB)");
    CHECK(expr("price * qty * (1 - discount)").toJson() ==
          R"({"$multiply":["$price","$qty",{"$subtract":[1,"$discount"]}]})");
    CHECK(expr("if(temp > 30, 'hot', 'ok')").toJson() == R"({"$cond":[{"$gt":["$temp",30]},"hot","ok"]})");
    CHECK(expr("round(avg(readings), 2)").toJson() == R"({"$round":[{"$avg":"$readings"},2]})");
    CHECK(expr("year(at, 'Asia/Kolkata')")["$year"]["timezone"].asString() == "Asia/Kolkata");
    CHECK(expr("'$5 off'")["$literal"].asString() == "$5 off");
    CHECK(expr("a % 3 == 0 and not b")["$and"].size() == 2);
    CHECK(expr("-x")["$multiply"].at(0).asInt() == -1);
    CHECK(expr("-2.5").asDouble() == -2.5);
    CHECK(expr("now()").asString() == "$$NOW");
    CHECK(expr("date('2026-11-17')").isDate());
    CHECK(expr("x in [1, 2]")["$in"].at(1).size() == 2);

    CHECK(formulaFilter("temp > 30 && room == 'lab'").toJson() == R"({"temp":{"$gt":30},"room":"lab"})");
    CHECK(formulaFilter("age >= 18 && age < 60").toJson() == R"({"age":{"$gte":18,"$lt":60}})");
    CHECK(formulaFilter("18 <= age").toJson() == R"({"age":{"$gte":18}})");  // constant on the left
    CHECK(formulaFilter("used > limit").toJson() == R"({"$expr":{"$gt":["$used","$limit"]}})");
    CHECK(formulaFilter("active && !deleted").toJson() == R"({"active":true,"deleted":{"$ne":true}})");
    CHECK(formulaFilter("role in ['a', 'b'] || score >= 9.5")["$or"].size() == 2);
    CHECK(formulaFilter("role not in ['x']")["role"]["$nin"].size() == 1);
    CHECK(formulaFilter("x = 5").toJson() == R"({"x":5})");  // single '=' means equality
    CHECK(formulaFilter("exists(device)")["device"]["$exists"].asBool());
    CHECK(formulaFilter("a > 1 && a > 2").contains("$and"));
    CHECK(formulaFilter("a > b && c > d")["$and"].size() == 2);  // two $expr parts are not merged into one
    CHECK(formulaFilter("!(c == 3)")["$nor"].size() == 1);

    // helpful errors with the column
    auto errAt = [](const char *f) -> size_t {
        try {
            expr(f);
        } catch (const FormulaError &e) {
            return e.column;
        }
        return 0;
    };
    CHECK(errAt("price * * qty") == 9);
    CHECK(errAt("price @ 2") == 7);
    CHECK(errAt("foo(1)") == 1);
    CHECK(errAt("'abc") == 1);
    CHECK(errAt("a < b < c") == 7);
    CHECK(errAt("round()") == 1);
    CHECK(errAt("") == 1);
    CHECK(errAt("max(") == 5);
    CHECK(errAt("date('2026-02-30')") == 6);
    CHECK(errAt("1x") == 2);
    CHECK(errAt("(a + b") == 7);
    try {
        expr("foo(1)");
    } catch (const FormulaError &e) {
        CHECK(std::string(e.what()).find("Available: abs") != std::string::npos);
    }

    // formulas in the builder and in find()
    auto Shop = model("FormulaShop",
                      Schema({{"price", Number()}, {"qty", Int()}, {"at", DateTime()}, {"tag", String().alias("t")}}));
    Value p = Shop.aggregate()
                  .match("qty > '2' && at >= '2026-11-17'")  // cast: '2' -> 2, date text -> Date
                  .addField("total", "price * qty")
                  .group("", {sum("price * qty").as("revenue"), avg("total")})
                  .pipeline();
    CHECK(p.at(0)["$match"]["qty"]["$gt"].isInt() && p.at(0)["$match"]["at"]["$gte"].isDate());
    CHECK(p.at(1)["$addFields"]["total"]["$multiply"].size() == 2);
    CHECK(p.at(2)["$group"]["revenue"]["$sum"]["$multiply"].size() == 2);
    CHECK_THROWS(Shop.aggregate().group("", {sum("price * qty")}), Error);  // a formula needs .as()
    CHECK(Shop.find("qty >= 2 && t == 'x'").filter()["qty"]["$gte"].asInt() == 2);
    CHECK_THROWS(Shop.find("qty >>> 2"), FormulaError);

    // bucketByTime / populate / toShell shapes
    Value b = Shop.aggregate()
                  .bucketByTime("at", "15m", {avg("price")}, {.by = "tag", .timezone = "Asia/Kolkata"})
                  .pipeline();
    const Value &trunc = b.at(0)["$group"]["_id"]["time"]["$dateTrunc"];
    CHECK(trunc["unit"].asString() == "minute" && trunc["binSize"].asInt() == 15 &&
          trunc["timezone"].asString() == "Asia/Kolkata");
    CHECK(b.at(0)["$group"]["_id"]["tag"].asString() == "$tag");
    CHECK(b.at(b.size() - 1)["$sort"]["time"].asInt() == 1);
    CHECK(Shop.aggregate()
              .bucketByTime("at", "1M")
              .pipeline()
              .at(0)["$group"]["_id"]["time"]["$dateTrunc"]["unit"]
              .asString() == "month");
    CHECK(Shop.aggregate()
              .bucketByTime("at", "2 hours")
              .pipeline()
              .at(0)["$group"]["_id"]["time"]["$dateTrunc"]["binSize"]
              .asInt() == 2);
    CHECK(Shop.aggregate().bucketByTime("at", "1h").pipeline().at(0)["$group"].contains("count"));  // default: count
    CHECK_THROWS(Shop.aggregate().bucketByTime("at", "5 parsecs"), Error);
    CHECK_THROWS(Shop.aggregate().bucketByTime("at", "0m"), Error);
    CHECK_THROWS(Shop.aggregate().bucketByTime("at", "1h", {}, {.by = "time"}), Error);
    CHECK_THROWS(Shop.aggregate().group("tag", {count()}).populate("x"), Error);  // populate after group
    CHECK_THROWS(Shop.aggregate().populate("price"), Error);                      // not a ref
    std::string sh = Shop.aggregate().match({{"at", "2026-11-17"}, {"_id", "6afdd514ff181d6c4f026fe2"}}).toShell();
    CHECK(sh.find("db.getCollection(\"formulashops\").aggregate([") == 0);
    CHECK(sh.find("ISODate(\"2026-11-17T00:00:00.000Z\")") != std::string::npos);
    CHECK(sh.find("ObjectId(\"6afdd514ff181d6c4f026fe2\")") != std::string::npos);
}

static void testV04Db() {
    section("formulas, bucketByTime, shortcuts, pipeline populate, explain (real data)");
    auto Company = model("V4Company", Schema({{"name", String()}}));
    Schema authorSchema({{"name", String()}, {"company", Id().ref("V4Company")}, {"pw", String().select(false)}});
    authorSchema.virtualPopulate("posts", {.ref = "V4Post", .localField = "_id", .foreignField = "author"});
    authorSchema.virtualPopulate("firstPost",
                                 {.ref = "V4Post", .localField = "_id", .foreignField = "author", .justOne = true});
    auto Author = model("V4Author", authorSchema);
    auto Tag = model("V4Tag", Schema({{"label", String()}}));
    Schema postSchema({{"title", String()},
                       {"author", Id().ref("V4Author")},
                       {"tags", Array(Id().ref("V4Tag"))},
                       {"price", Number()},
                       {"qty", Int()},
                       {"at", DateTime()}},
                      {.versionKey = ""});
    postSchema.index({{"qty", 1}});
    auto Post = model("V4Post", postSchema);
    for (auto M : {Company, Author, Tag, Post}) M.dropCollection();

    auto acme = Company.create({{"name", "Acme"}});
    auto ana = Author.create({{"name", "Ana"}, {"company", acme._id()}, {"pw", "secret"}});
    auto bo = Author.create({{"name", "Bo"}});
    auto cy = Author.create({{"name", "Cy"}});  // no posts
    auto tz = Tag.create({{"label", "z"}}), ta = Tag.create({{"label", "a"}});
    // 10:05, 10:12, 10:50 (gap at 10:15 and 10:30), 11:20 ...
    const char *times[] = {"2026-11-18T10:05:00Z", "2026-11-18T10:12:00Z", "2026-11-18T10:50:00Z",
                           "2026-11-18T11:20:00Z", "2026-11-18T19:00:00Z", "2026-11-19T02:00:00Z"};
    for (int i = 0; i < 6; i++)
        Post.create({{"title", "P" + std::to_string(i)},
                     {"author", i < 4 ? ana._id() : bo._id()},
                     {"tags", Value::array({tz._id(), ObjectId::generate(), ta._id()})},  // middle id does not exist
                     {"price", 10 + i},
                     {"qty", i},
                     {"at", times[i]}});

    // formulas in queries and writes
    CHECK(Post.find("qty >= 2 && price < 14").count() == 2);
    CHECK(Post.countDocuments("title == 'P1' || qty == 5") == 2);
    CHECK(Post.find("price > qty * 3").count() == 5);  // field vs field -> $expr (P5: 15 > 15 is false)
    CHECK(Post.findOne("qty == 3").lean().value()["title"].asString() == "P3");
    CHECK(Post.exists("title == 'P0'") && !Post.exists("title == 'nope'"));
    CHECK(Post.updateMany("qty >= 4", {{"$inc", {{"price", 1}}}}).modifiedCount == 2);
    CHECK(Post.find("at >= date('2026-11-18T19:00:00Z')").count() == 2);
    CHECK(Post.find("at >= '2026-11-18T19:00:00Z'").count() == 2);  // text compared with a Date field: cast
    CHECK(Post.deleteMany("qty > 100") == 0);

    // builder with formulas
    auto rev =
        Post.aggregate().match("qty >= 1").group("", {sum("price * qty").as("revenue"), count()}).first().value();
    CHECK(rev["count"].asInt() == 5 && rev["revenue"].asDouble() == 11 * 1 + 12 * 2 + 13 * 3 + 15 * 4 + 16 * 5);
    auto withTotal = Post.aggregate().addField("total", "price * qty").match("total > 50").sort("-total").exec();
    CHECK(withTotal.size() == 2 && withTotal[0]["total"].asInt() == 80);
    auto labels = Post.aggregate()
                      .addField("label", "if(price >= 13, 'costly', 'cheap')")
                      .group("label", {count()})
                      .sort("_id")
                      .exec();
    CHECK(labels.size() == 2 && labels[0]["_id"].asString() == "cheap" && labels[0]["count"].asInt() == 3);

    // time buckets
    auto q = Post.aggregate()
                 .match("at < date('2026-11-18T12:00:00Z')")
                 .bucketByTime("at", "15m", {count(), avg("price")})
                 .exec();
    CHECK(q.size() == 3 && q[0]["time"].asDate().iso() == "2026-11-18T10:00:00.000Z" && q[0]["count"].asInt() == 2);
    CHECK(q[0].items()[0].first == "time");  // time is the first field
    auto filled = Post.aggregate()
                      .match("at < date('2026-11-18T12:00:00Z')")
                      .bucketByTime("at", "15m", {count(), avg("price")}, {.fillGaps = true})
                      .exec();
    CHECK(filled.size() == 6);                                                 // 10:00 10:15 10:30 10:45 11:00 11:15
    CHECK(filled[1]["count"].asInt() == 0 && filled[1]["avgPrice"].isNull());  // empty bucket: count 0
    auto daysIst = Post.aggregate().bucketByTime("at", "1d", {count()}, {.timezone = "Asia/Kolkata"}).exec();
    CHECK(daysIst.size() == 2 && daysIst[0]["count"].asInt() == 4);          // 19:00Z and 02:00Z are Nov 19 in India
    CHECK(daysIst[0]["time"].asDate().iso() == "2026-11-17T18:30:00.000Z");  // local midnight
    auto perAuthor = Post.aggregate().bucketByTime("at", "1d", {count()}, {.by = "author"}).exec();
    CHECK(perAuthor.size() == 3 && perAuthor[0].contains("author"));

    // one-line reports
    Value byAuthor = Post.countBy("author");
    CHECK(byAuthor[ana.id()].asInt() == 4 && byAuthor[bo.id()].asInt() == 2 && byAuthor.items()[0].first == ana.id());
    CHECK(Post.countBy("author", "qty >= 4").size() == 1);
    CHECK(Post.sumBy("author", "price")[bo.id()].asInt() == 15 + 16);
    CHECK(Post.avgBy("author", "qty")[ana.id()].asDouble() == 1.5);
    Value st = Post.stats("price");
    CHECK(st["count"].asInt() == 6 && st["min"].asInt() == 10 && st["max"].asInt() == 16 && st["sum"].asInt() == 77);
    CHECK(Post.stats("price", "qty > 100")["count"].asInt() == 0 && Post.stats("price", "qty > 100")["avg"].isNull());
    auto latest = Post.latestPerGroup("author", "at");
    CHECK(latest.size() == 2 && latest[0]["title"].asString() == "P5" && latest[1]["title"].asString() == "P3");

    // populate inside the pipeline
    PopulateOptions pa;
    pa.path = "author";
    pa.select = "name company";
    PopulateOptions pc;
    pc.path = "company";
    pa.populate.push_back(pc);
    auto rows = Post.aggregate().match("qty <= 1").populate(pa).populate("tags").sort("qty").exec();
    CHECK(rows.size() == 2 && rows[0]["author"]["name"].asString() == "Ana");
    CHECK(rows[0]["author"]["company"]["name"].asString() == "Acme");  // nested
    CHECK(!rows[0]["author"].contains("pw"));                          // select(false) stays hidden
    CHECK(rows[0]["tags"].size() == 2 &&
          rows[0]["tags"].at(0)["label"].asString() == "z");  // order kept, missing dropped
    CHECK(!rows[0].contains("__nv_pop"));
    auto hidden = Post.aggregate().match("qty == 0").populate("author").first().value();
    CHECK(hidden["author"]["name"].asString() == "Ana" && !hidden["author"].contains("pw"));
    PopulateOptions nobody;
    nobody.path = "author";
    nobody.match = "name == 'Nobody'";  // filter may be a formula too
    CHECK(Post.aggregate().match("qty == 0").populate(nobody).first().value()["author"].isNull());
    PopulateOptions sorted;
    sorted.path = "tags";
    sorted.sort = "label";
    sorted.limit = 1;
    CHECK(Post.aggregate().match("qty == 0").populate(sorted).first().value()["tags"].at(0)["label"].asString() == "a");

    // virtual populate in the pipeline + group on the result
    PopulateOptions vp;
    vp.path = "posts";
    vp.sort = "-qty";
    vp.limit = 2;
    auto authors =
        Author.aggregate().populate(vp).populate("firstPost").addField("postCount", "size(posts)").sort("name").exec();
    CHECK(authors.size() == 3 && authors[0]["posts"].size() == 2 && authors[0]["posts"].at(0)["qty"].asInt() == 3);
    CHECK(authors[0]["firstPost"].isObject() && authors[2]["firstPost"].isNull() &&
          authors[2]["postCount"].asInt() == 0);

    // explain / plan
    CHECK(Post.aggregate().match("qty > 1").plan().find("IXSCAN(qty_1)") != std::string::npos);
    CHECK(Post.aggregate().match("price > 1").plan().find("COLLSCAN") != std::string::npos);
    CHECK(Post.aggregate().match("qty > 1").explain().isObject());

    for (auto M : {Company, Author, Tag, Post}) M.dropCollection();
}

/* ============================ v0.5: slow query log, strictQuery ============================ */
static void testSlowAndStrict() {
    section("slow query log + strictQuery");
    auto S = model("SlowThing", Schema({{"device", String()},
                                        {"at", DateTime()},
                                        {"n", Int().alias("count")},
                                        {"addr", Object(Schema({{"city", String()}}))},
                                        {"meta", Mixed()}},
                                       {.timestamps = true}));
    S.dropCollection();
    S.create({{"device", "a"}, {"at", Date::now()}, {"n", 1}});

    std::vector<SlowQuery> slow;
    std::mutex mu;
    setSlowQueryThreshold(15, [&](const SlowQuery &q) {
        std::lock_guard<std::mutex> lk(mu);
        slow.push_back(q);
    });
    S.countDocuments();  // fast: not reported
    // $where + sleep makes the server take ~40 ms
    auto rows =
        S.find({{"device", "a"}, {"at", {{"$gte", "2020-01-01"}}}, {"$where", "sleep(40) || true"}}).sort("-at").lean();
    CHECK(rows.size() == 1);
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(slow.size() == 1);
        if (!slow.empty()) {
            CHECK(slow[0].collection == "slowthings" && slow[0].op == "find" && slow[0].ms >= 15);
            CHECK(slow[0].suggestion == "consider an index {device: 1, at: -1}");
            CHECK(slow[0].args["filter"]["device"].asString() == "a");
        }
    }
    Value sleepFn{
        {"$function",
         Value{{"body", "function() { sleep(40); return true; }"}, {"args", Value::array()}, {"lang", "js"}}}};
    S.aggregate().match({{"device", "a"}, {"$expr", sleepFn}}).exec();
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(slow.size() == 2 && slow.back().op == "aggregate" &&
              slow.back().suggestion == "consider an index {device: 1}");
    }
    setSlowQueryThreshold(0);
    S.find({{"$where", "sleep(40) || true"}}).lean();
    CHECK(slow.size() == 2);  // off again

    // strictQuery
    CHECK(S.find({{"devise", "a"}}).count() == 0);  // default: unknown path silently matches nothing (like Mongoose)
    auto Strict = model("StrictThing", Schema({{"email", String()},
                                               {"n", Int().alias("count")},
                                               {"addr", Object(Schema({{"city", String()}}))},
                                               {"meta", Mixed()}},
                                              {.timestamps = true, .strictQuery = true}));
    Strict.dropCollection();
    Strict.create({{"email", "a@b.c"}, {"n", 2}, {"addr", {{"city", "Durgapur"}}}});
    try {
        Strict.find({{"emial", "a@b.c"}}).count();
        CHECK(false);
    } catch (const Error &e) {
        std::string w = e.what();
        CHECK(w.find("unknown path \"emial\"") != std::string::npos &&
              w.find("Did you mean \"email\"?") != std::string::npos);
    }
    CHECK_THROWS(Strict.countDocuments("addr.cty == 'Durgapur'"), Error);
    CHECK_THROWS(Strict.updateMany({{"$or", Value::array({{{"emal", 1}}})}}, {{"n", 3}}), Error);  // nested in $or too
    CHECK_THROWS(Strict.deleteMany({{"zzz", 1}}), Error);
    // known paths, aliases, nested, Mixed sub-paths, timestamps, _id, __v and operators are fine
    CHECK(Strict.countDocuments({{"email", "a@b.c"},
                                 {"count", 2},
                                 {"addr.city", "Durgapur"},
                                 {"meta.any.thing", {{"$exists", false}}},
                                 {"createdAt", {{"$exists", true}}},
                                 {"__v", 0},
                                 {"$comment", "x"}}) == 1);
    CHECK(Strict.findById(ObjectId::generate()).lean() == std::nullopt);
    setStrictQuery(true);  // global switch
    CHECK_THROWS(S.find({{"devise", "a"}}).count(), Error);
    setStrictQuery(false);
    S.dropCollection();
    Strict.dropCollection();
}

/* ============================ v0.5: typed models, co_await ============================ */
struct TAddress {
    std::string city;
    int pin = 0;
};
NEVLA_STRUCT(TAddress, city, pin)
struct TPet {
    std::string name, kind;
};
NEVLA_STRUCT(TPet, name, kind)
struct TUser {
    std::string id, name, email;
    int age = 0;
    double score = 0;
    bool active = true;
    std::vector<std::string> tags;
    std::optional<std::string> nick;
    TAddress address;
    Date joined;
    std::map<std::string, int> marks;
    std::vector<TPet> pets;
};
NEVLA_STRUCT(TUser, id, name, email, age, score, active, tags, nick, address, joined, marks, pets)

static void testTypedModels() {
    section("typed models (member pointers)");
    Schema gen = schemaOf<TUser>({.timestamps = true});
    CHECK(!gen.field("id") && gen.field("name")->type == FieldType::String && gen.field("age")->type == FieldType::Int);
    CHECK(gen.field("score")->type == FieldType::Number && gen.field("active")->type == FieldType::Boolean);
    CHECK(gen.field("joined")->type == FieldType::Date && gen.field("marks")->type == FieldType::Map);
    CHECK(gen.field("address")->type == FieldType::Object && gen.path("address.pin")->type == FieldType::Int);
    CHECK(gen.field("pets")->type == FieldType::Array && gen.field("pets")->schema_ && gen.path("pets.0.kind"));
    CHECK(gen.field("tags")->type == FieldType::Array && gen.field("nick")->type == FieldType::String);
    CHECK(TypedModel<TUser>::path(&TUser::id) == "_id" && TypedModel<TUser>::path(&TUser::email) == "email");

    // the generated schema + extra rules
    Schema s = schemaOf<TUser>({.timestamps = true});
    s.add("email", String().required().lowercase().unique());
    s.add("age", Int().min(0).max(150));
    TypedModel<TUser> Users("TUser", s);
    Users.deleteMany(Value::object());
    Users.model().createIndexes();

    TUser a;
    a.name = "Asha";
    a.email = "ASHA@X.IN";
    a.age = 21;
    a.score = 7.5;
    a.tags = {"cpp", "iot"};
    a.address = {"Durgapur", 713209};
    a.joined = *Date::parse("2026-01-02");
    a.marks = {{"dsp", 90}, {"vlsi", 80}};
    a.pets = {{"Tom", "cat"}};
    TUser saved = Users.create(a);
    CHECK(saved.id.size() == 24 && saved.email == "asha@x.in" && saved.address.city == "Durgapur");
    CHECK(saved.marks.at("dsp") == 90 && saved.pets.size() == 1 && saved.pets[0].kind == "cat" && !saved.nick);

    TUser b = a, c = a;
    b.name = "Bo", b.email = "bo@x.in", b.age = 35, b.score = 9, b.tags = {"java"}, b.address = {"Kolkata", 700001};
    b.nick = "bobby";
    c.name = "Cy", c.email = "cy@x.in", c.age = 70, c.score = 4, c.active = false;
    auto many = Users.insertMany({b, c});
    CHECK(many.size() == 2 && many[0].id.size() == 24);

    CHECK(Users.find().where(&TUser::age).gte(18).lt(60).count() == 2);  // both conditions kept
    CHECK(Users.find().where(&TUser::address, &TAddress::city).equals("Kolkata").first()->name == "Bo");
    CHECK(Users.find().where(&TUser::tags).contains("cpp").count() == 2);
    CHECK(Users.find().where(&TUser::name).regex("^b", "i").count() == 1);
    CHECK(Users.find().where(&TUser::nick).exists(true).count() == 3);  // nullopt is stored as null
    CHECK(Users.find("nick == null").count() == 2);
    CHECK(Users.find().where(&TUser::nick).equals("bobby").count() == 1);
    CHECK(Users.find().where(&TUser::age).in({21, 70}).count() == 2);
    CHECK(Users.find().where(&TUser::active).equals(false).count() == 1);
    CHECK(Users.find().where("score > 5 && age < 60").count() == 2);  // formula + typed together
    auto sorted = Users.find().sort(&TUser::active, -1).sort(&TUser::age, -1).exec();
    CHECK(sorted.size() == 3 && sorted[0].name == "Bo" && sorted[2].name == "Cy");
    CHECK(Users.find().sort(&TUser::score).limit(1).exec()[0].name == "Cy");
    CHECK(Users.findOne({{"email", "cy@x.in"}})->age == 70);
    CHECK(Users.findById(saved.id)->name == "Asha" && !Users.findById(ObjectId::generate()));

    // typed updates
    auto r = Users.update(saved.id)
                 .set(&TUser::age, 22)
                 .inc(&TUser::score, 1.5)
                 .push(&TUser::tags, std::string("pro"))
                 .exec();
    CHECK(r.modifiedCount == 1);
    TUser now = *Users.findById(saved.id);
    CHECK(now.age == 22 && now.score == 9.0 && now.tags.size() == 3 && now.tags[2] == "pro");
    Users.update(saved.id).pull(&TUser::tags, std::string("cpp")).set(&TUser::nick, std::string("ash")).exec();
    now = *Users.findById(saved.id);
    CHECK(now.tags.size() == 2 && now.nick == "ash");
    CHECK(Users.updateMany("age >= 30").set(&TUser::active, true).exec().matchedCount == 2);
    CHECK(Users.find().where(&TUser::active).equals(false).count() == 0);
    Users.update(saved.id).unset(&TUser::nick).exec();
    CHECK(!Users.findById(saved.id)->nick);

    // save(): changed fields are validated and stored
    now.address.city = "Asansol";
    now.age = 23;
    TUser after = Users.save(now);
    CHECK(after.address.city == "Asansol" && after.age == 23 && Users.findById(saved.id)->age == 23);
    now.age = 500;
    CHECK_THROWS(Users.save(now), ValidationError);  // schema rules still apply
    TUser fresh;
    fresh.name = "New";
    fresh.email = "new@x.in";
    CHECK(Users.save(fresh).id.size() == 24);  // no id -> insert
    TUser dupe = a;
    CHECK_THROWS(Users.create(dupe), DuplicateKeyError);  // unique email
    TUser noEmail;
    noEmail.name = "x";
    CHECK_THROWS(Users.create(noEmail), ValidationError);

    CHECK(Users.deleteById(saved.id) && !Users.deleteById(saved.id));
    CHECK(Users.count() == 3);
    auto stats = Users.aggregate().group("", {avg("age")}).first();
    CHECK(stats && (*stats)["avgAge"].asDouble() > 0);

    // schema generated from the struct alone
    TypedModel<TAddress> Places("TPlace");
    Places.deleteMany(Value::object());
    Places.create({"Durgapur", 713209});
    CHECK(Places.find().where(&TAddress::pin).gt(700000).first()->city == "Durgapur");
    Places.deleteMany(Value::object());
    Users.deleteMany(Value::object());
}

static Task<int64_t> countAdultsTask(Model U) {
    int64_t n = co_await U.find("age >= 18").countAsync();
    co_return n;
}

// Note for GCC 13: build Values in variables before co_await (braced lists inside co_await crash GCC 13)
static Task<std::string> asyncFlow(Model U, std::vector<std::thread::id> *threads) {
    threads->push_back(std::this_thread::get_id());
    Value newUser{{"name", "co"}, {"age", 40}};
    auto d = co_await U.createAsync(newUser);
    threads->push_back(std::this_thread::get_id());
    auto found = co_await U.findById(d._id()).execAsync();
    Value byId{{"_id", d._id()}}, setAge{{"age", 41}}, byName{{"name", "co2"}};
    co_await U.updateOneAsync(byId, setAge);
    auto rows = co_await U.find(byId).leanAsync();
    auto agg = co_await U.aggregate().match(byId).execAsync();
    found->set("name", "co2");
    co_await found->saveAsync();
    int64_t adults = co_await countAdultsTask(U);  // a Task inside a Task
    auto one = co_await U.findOne(byName).leanAsync();
    int64_t removed = co_await U.deleteOneAsync(byId);
    int64_t seven = co_await asyncCall([] { return 7; });
    co_return std::to_string(rows[0]["age"].asInt()) + "/" + std::to_string(agg.size()) + "/" + std::to_string(adults) +
        "/" + (*one)["name"].asString() + "/" + std::to_string(removed) + "/" + std::to_string(seven);
}

static Task<void> failingFlow(Model U) {
    Value bad{{"name", ""}};
    co_await U.createAsync(bad);  // required name -> ValidationError
}

static Task<int> sumFlow(Model U, int rounds) {
    int total = 0;
    for (int i = 0; i < rounds; i++) {
        Value doc{{"name", "bulk"}, {"age", i}};
        co_await U.createAsync(doc);
        total += static_cast<int>(co_await U.countDocumentsAsync(doc) > 0);
    }
    co_return total;
}

static void testAsync() {
    section("co_await / Task / Async");
    auto U = model("AsyncUser", Schema({{"name", String().required()}, {"age", Int()}}));
    U.dropCollection();
    U.create({{"name", "a"}, {"age", 20}});
    U.create({{"name", "b"}, {"age", 10}});

    CHECK(countAdultsTask(U).get() == 1);
    std::vector<std::thread::id> threads;
    std::string flow = asyncFlow(U, &threads).get();
    CHECK(flow == "41/1/2/co2/1/7");
    if (flow != "41/1/2/co2/1/7") std::cout << "  asyncFlow: " << flow << "\n";
    CHECK(threads.size() == 2 && threads[0] != threads[1]);  // the work ran on a pool thread

    try {
        failingFlow(U).get();
        CHECK(false);
    } catch (const ValidationError &e) {
        CHECK(e.errors.count("name"));
    }
    // Async::get() runs right here, without a coroutine
    CHECK(U.find("age >= 0").countAsync().get() == 2);

    // many coroutines from many threads at the same time
    std::vector<std::thread> ts;
    std::atomic<int> ok{0};
    for (int t = 0; t < 6; t++) ts.emplace_back([&] { ok += sumFlow(U, 25).get(); });
    for (auto &t : ts) t.join();
    CHECK(ok == 150 && U.countDocuments({{"name", "bulk"}}) == 150);

    // a typed query too
    TypedModel<TAddress> P("AsyncPlace");
    P.deleteMany(Value::object());
    P.create({"Kolkata", 700001});
    auto task = [](TypedModel<TAddress> &m) -> Task<size_t> {
        auto rows = co_await m.find().where(&TAddress::city).equals("Kolkata").execAsync();
        co_return rows.size();
    };
    CHECK(task(P).get() == 1);
    P.deleteMany(Value::object());
    U.dropCollection();
}

/* ============================ v0.5: time-series, batch writer, fromQuery, schema export ============================
 */
static void testTimeSeries() {
    section("time-series collections");
    CHECK_THROWS(model("TsBad1", Schema({{"v", Number()}}, {.timeseries = {.timeField = "at"}})),
                 Error);  // no such field
    CHECK_THROWS(model("TsBad2", Schema({{"at", String()}}, {.timeseries = {.timeField = "at"}})),
                 Error);  // not a date
    CHECK_THROWS(
        model("TsBad3", Schema({{"at", DateTime()}}, {.timeseries = {.timeField = "at", .granularity = "days"}})),
        Error);
    Schema s(
        {{"device", String().required()}, {"value", Number().required()}, {"at", DateTime().required()}},
        {.versionKey = "",
         .timeseries = {
             .timeField = "at", .metaField = "device", .granularity = "seconds", .expireAfterSeconds = 86400 * 30}});
    auto Ts = model("TsReading", s, "ts_readings");
    Ts.dropCollection();
    std::vector<Value> rows;
    for (int i = 0; i < 300; i++)
        rows.push_back({{"device", i % 3 ? "d1" : "d2"}, {"value", i}, {"at", Date{Date::now().ms - i * 1000}}});
    Ts.insertMany(rows);
    Value info = Ts.collectionInfo();
    CHECK(info["type"].asString() == "timeseries");
    CHECK(info["options"]["timeseries"]["timeField"].asString() == "at" &&
          info["options"]["timeseries"]["metaField"].asString() == "device");
    CHECK(info["options"]["expireAfterSeconds"].asInt() == 86400 * 30);
    CHECK(Ts.countDocuments() == 300 && Ts.countBy("device")["d1"].asInt() == 200);
    CHECK(Ts.find("device == 'd2' && value >= 150").count() == 50);
    auto buckets = Ts.aggregate().bucketByTime("at", "1m", {count()}, {.by = "device"}).exec();
    int64_t total = 0;
    for (auto &b : buckets) total += b["count"].asInt();
    CHECK(total == 300);
    Ts.dropCollection();
    Ts.create({{"device", "d3"}, {"value", 1}, {"at", Date::now()}});  // dropped -> created again as time-series
    CHECK(Ts.collectionInfo()["type"].asString() == "timeseries");
    Ts.createCollection();  // already there: no error
    auto Plain = model("TsPlain", Schema({{"x", Int()}}));
    Plain.dropCollection();
    Plain.createCollection();
    CHECK(Plain.collectionInfo()["type"].asString() == "collection");
    Ts.dropCollection();
    Plain.dropCollection();
}

static void testBatchWriter() {
    section("batch writer");
    auto R = model("BwReading", Schema({{"device", String().required()}, {"value", Number().min(0)}, {"seq", Int()}},
                                       {.timestamps = true, .versionKey = ""}));
    R.dropCollection();
    {
        auto w = R.batchWriter({.maxDocs = 500, .maxDelayMs = 50});
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; t++)
            ts.emplace_back([&w, t] {
                for (int i = 0; i < 5000; i++)
                    w.add({{"device", "d" + std::to_string(t)}, {"value", i}, {"seq", t * 5000 + i}});
            });
        for (auto &t : ts) t.join();
        CHECK_THROWS(w.add({{"value", -1}}), ValidationError);  // checked right away, never queued
        w.flush();
        BatchStats st = w.stats();
        CHECK(st.queued == 40000 && st.written == 40000 && st.failed == 0 && st.batches >= 80);
        CHECK(R.countDocuments() == 40000 && R.distinct("seq").size() == 40000);
        CHECK(R.findOne().lean().value()["createdAt"].isDate());  // timestamps applied at add()
    }
    // the delay timer writes without flush()
    {
        auto w = R.batchWriter({.maxDocs = 1000, .maxDelayMs = 30});
        for (int i = 0; i < 3; i++) w.add({{"device", "late"}, {"value", i}});
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(R.countDocuments({{"device", "late"}}) == 3);
    }
    // the destructor writes what is left
    {
        auto w = R.batchWriter({.maxDocs = 1000, .maxDelayMs = 60000});
        for (int i = 0; i < 10; i++) w.add({{"device", "end"}, {"value", i}});
    }
    CHECK(R.countDocuments({{"device", "end"}}) == 10);
    // backpressure: a tiny queue still accepts everything
    {
        auto w = R.batchWriter({.maxDocs = 5, .maxDelayMs = 5, .maxQueue = 10});
        for (int i = 0; i < 2000; i++) w.add({{"device", "bp"}, {"value", i}});
        w.flush();
        CHECK(w.stats().written == 2000 && R.countDocuments({{"device", "bp"}}) == 2000);
    }
    // duplicate keys: the rest of the batch is still written, the error is reported
    auto U = model("BwUnique", Schema({{"code", String().unique()}}));
    U.dropCollection();
    U.createIndexes();
    U.create({{"code", "x1"}});
    {
        auto w = U.batchWriter({.maxDocs = 100, .maxDelayMs = 20});
        std::atomic<int64_t> lost{0};
        std::string msg;
        w.onError([&](const std::string &m, int64_t n) {
            lost += n;
            msg = m;
        });
        for (int i = 0; i < 50; i++) w.add({{"code", i % 10 == 0 ? std::string("x1") : "c" + std::to_string(i)}});
        w.flush();
        CHECK(lost == 5 && w.stats().failed == 5 && w.stats().written == 45 && msg.find("E11000") != std::string::npos);
        CHECK(U.countDocuments() == 46);
    }
    U.dropCollection();
    R.dropCollection();
}

static void testFromQuery() {
    section("fromQuery (REST parameters)");
    auto P = model("ApiUser", Schema({{"name", String()},
                                      {"age", Int()},
                                      {"role", String()},
                                      {"city", String()},
                                      {"joined", DateTime()},
                                      {"password", String().select(false)}}));
    P.dropCollection();
    const char *names[] = {"Asha", "Bo", "Cy", "Dev", "Esha", "a.b"};
    for (int i = 0; i < 6; i++)
        P.create({{"name", names[i]},
                  {"age", 15 + i * 5},
                  {"role", i % 2 ? "admin" : "user"},
                  {"city", i < 3 ? "Kolkata" : "Durgapur"},
                  {"joined", "2026-0" + std::to_string(i + 1) + "-01"},
                  {"password", "x"}});
    QueryParamOptions opt{.allow = {"name", "age", "role", "city", "joined"}, .maxLimit = 3, .defaultSort = "name"};

    auto params = parseQueryString("?age%5Bgte%5D=20&role=admin&sort=-age,name&page=1&limit=2&fields=name,age");
    CHECK(params.at("age[gte]") == "20" && params.at("fields") == "name,age");
    Page pg = P.fromQuery(params, opt);
    CHECK(pg.total == 3 && pg.docs.size() == 2 && pg.pages == 2 && pg.hasNext);
    CHECK(pg.docs[0]["name"].asString() == "a.b" && pg.docs[0]["age"].asInt() == 40);  // sorted -age
    CHECK(!pg.docs[0].has("role") && !pg.docs[0].has("password"));                     // only the selected fields
    Value json = pg.toJSON();
    CHECK(json["docs"].size() == 2 && json["total"].asInt() == 3 && json["hasNext"].asBool());

    CHECK(P.fromQuery({{"city[in]", "Kolkata,Delhi"}}, opt).total == 3);
    CHECK(P.fromQuery({{"city[nin]", "Kolkata"}}, opt).total == 3);
    CHECK(P.fromQuery({{"name[contains]", "SH"}}, opt).total == 2);  // case-insensitive
    CHECK(P.fromQuery({{"name[contains]", "."}}, opt).total == 1);   // "." is literal, not "any character"
    CHECK(P.fromQuery({{"name[startsWith]", "a"}}, opt).total == 2);
    CHECK(P.fromQuery({{"joined[lt]", "2026-03-01"}}, opt).total == 2);  // cast to Date
    CHECK(P.fromQuery({{"age[ne]", "15"}, {"age[lte]", "30"}}, opt).total == 3);
    CHECK(P.fromQuery({{"role[exists]", "true"}}, opt).total == 6);
    CHECK(P.fromQuery({{"limit", "50"}}, opt).docs.size() == 3);       // clamped to maxLimit
    CHECK(P.fromQuery({}, opt).docs[0]["name"].asString() == "Asha");  // defaultSort
    CHECK(P.fromQuery({{"page", "3"}, {"limit", "2"}}, opt).docs.size() == 2);

    auto rejects = [&](std::map<std::string, std::string> q, const std::string &param) {
        try {
            P.fromQuery(q, opt);
        } catch (const QueryParamError &e) {
            return e.param == param;
        }
        return false;
    };
    CHECK(rejects({{"password", "x"}}, "password"));          // not allowed
    CHECK(rejects({{"$where", "1"}}, "$where"));              // injection attempt
    CHECK(rejects({{"age[gt]", "abc"}}, "age"));              // cannot be cast
    CHECK(rejects({{"age[between]", "1"}}, "age[between]"));  // unknown operator
    CHECK(rejects({{"name[regex]", ".*"}}, "name[regex]"));   // regex off by default
    CHECK(rejects({{"page", "0"}}, "page"));
    CHECK(rejects({{"page", "two"}}, "page"));
    CHECK(rejects({{"sort", "password"}}, "sort"));
    CHECK(rejects({{"fields", "password"}}, "fields"));
    CHECK(rejects({{"role[exists]", "maybe"}}, "role[exists]"));
    CHECK(rejects({{"age[gt", "1"}}, "age[gt"));
    QueryParamOptions loose = opt;
    loose.rejectUnknown = false;
    loose.allowRegex = true;
    CHECK(P.fromQuery({{"password", "x"}, {"utm_source", "ad"}}, loose).total == 6);  // ignored
    CHECK(P.fromQuery({{"name[regex]", "^[AB]"}}, loose).total == 2);
    CHECK_THROWS(P.fromQuery({{"name[regex]", "(("}}, loose), QueryParamError);  // invalid regex
    P.dropCollection();
}

static void testSchemaExport() {
    section("schema export: $jsonSchema, TypeScript, OpenAPI, syncValidator");
    Schema s({{"name", String().required().minLength(2).maxLength(40)},
              {"age", Int().min(0).max(150)},
              {"score", Number()},
              {"role", String().enumOf({"user", "admin"}).def("user")},
              {"tags", Array(String())},
              {"address", Object(Schema({{"city", String().required()}, {"pin", Int()}}))},
              {"prices", Map(Number())},
              {"owner", Id()},
              {"seen", DateTime()},
              {"extra", Mixed()}},
             {.timestamps = true});
    Value js = s.toJsonSchema();
    CHECK(js["bsonType"].asString() == "object" && js["required"].size() == 2);  // name + _id
    CHECK(js["properties"]["name"]["bsonType"].asString() == "string" &&
          js["properties"]["name"]["minLength"].asInt() == 2);
    CHECK(js["properties"]["age"]["bsonType"].size() == 3 &&
          js["properties"]["age"]["maximum"].asInt() == 150);  // int,long,null
    CHECK(js["properties"]["role"]["enum"].size() == 3);       // user, admin, null
    CHECK(js["properties"]["address"]["properties"]["city"]["bsonType"].asString() == "string");
    CHECK(js["properties"]["address"]["required"].at(0).asString() == "city");
    CHECK(js["properties"]["prices"]["additionalProperties"]["bsonType"].size() == 5);
    CHECK(js["properties"]["createdAt"]["bsonType"].at(0).asString() == "date");
    CHECK(js["properties"]["extra"].empty());

    std::string ts = s.toTypeScript("User");
    CHECK(ts.find("export interface User {") == 0);
    CHECK(ts.find("  _id: string;") != std::string::npos && ts.find("  name: string;") != std::string::npos);
    CHECK(ts.find("  age?: number;") != std::string::npos &&
          ts.find("  role: \"user\" | \"admin\";") != std::string::npos);
    CHECK(ts.find("  tags?: string[];") != std::string::npos &&
          ts.find("  prices?: Record<string, number>;") != std::string::npos);
    CHECK(ts.find("    city: string;") != std::string::npos && ts.find("  extra?: unknown;") != std::string::npos);
    CHECK(ts.find("  createdAt: string;") != std::string::npos && ts.find("  __v?: number;") != std::string::npos);

    Value oa = s.toOpenAPI();
    CHECK(oa["type"].asString() == "object" && oa["properties"]["age"]["type"].asString() == "integer");
    CHECK(oa["properties"]["seen"]["format"].asString() == "date-time" && oa["properties"]["_id"]["readOnly"].asBool());
    CHECK(oa["properties"]["role"]["default"].asString() == "user" && oa["required"].size() == 1);

    // MongoDB enforces the schema itself now: even a schemaless writer is rejected
    auto Strict = model("ExportUser", s, "export_users");
    Strict.dropCollection();
    Strict.syncValidator();
    Strict.create({{"name", "Asha"}, {"age", 30}, {"address", {{"city", "Durgapur"}}}});
    auto Raw = model("ExportRaw", Schema({{"name", Mixed()}, {"age", Mixed()}}, {.strict = false}), "export_users");
    CHECK_THROWS(Raw.create({{"name", "A"}, {"age", 30}}), Error);     // too short
    CHECK_THROWS(Raw.create({{"name", "Bob"}, {"age", "x"}}), Error);  // wrong type
    Raw.create({{"name", "Bob"}, {"age", 31}});                        // valid
    CHECK(Strict.countDocuments() == 2);
    CHECK(Strict.collectionInfo()["options"]["validator"]["$jsonSchema"]["required"].size() == 2);
    CHECK_THROWS(Strict.syncValidator("sometimes"), Error);
    Strict.dropCollection();
}

int main() {
    testValue();
    testSchemaOffline();
    testSchemaFeaturesOffline();
    testAggregateBuilderOffline();
    testFormulasOffline();

    const char *env = std::getenv("NEVLA_TEST_URI");
    std::string uri = env ? env : "mongodb://127.0.0.1:27017/?directConnection=true";
    CHECK_THROWS(model("OfflineThing").countDocuments(), Error);  // before connect()
    try {
        connect(uri, "nevla_test");
    } catch (const Error &e) {
        std::cout << "Could not connect to MongoDB (" << e.what() << ")\n";
        return 2;
    }
    CHECK(connected());
    testCrud();
    testHooksVirtualsMethods();
    testPopulate();
    testDiscriminators();
    testCustomIdIndexesMixed();
    testTransactions();
    testThreads();
    testUpdateCasting();
    testQueryFeatures();
    testPopulateFeatures();
    testStaticsHooksDocs();
    testModelExtras();
    testAggregateBuilderDb();
    testV04Db();
    testSlowAndStrict();
    testTypedModels();
    testAsync();
    testTimeSeries();
    testBatchWriter();
    testFromQuery();
    testSchemaExport();
    disconnect();
    CHECK(!connected());

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
