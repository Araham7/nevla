// nevla tests: asli MongoDB chahiye.
//   NEVLA_TEST_URI="mongodb://127.0.0.1:27017" ./nevla_tests
// Transactions wala test replica set par hi chalta hai (standalone par skip ho jata hai).
#include <nevla/nevla.hpp>

#include <cstdlib>
#include <iostream>
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
#define CHECK_THROWS(expr, Type)                                                                     \
    do {                                                                                             \
        bool nv_ok = false;                                                                          \
        try {                                                                                        \
            expr;                                                                                    \
        } catch (const Type &) {                                                                     \
            nv_ok = true;                                                                            \
        } catch (const std::exception &e) {                                                          \
            std::cout << "  (galat exception: " << e.what() << ")\n";                                \
        }                                                                                            \
        if (nv_ok)                                                                                   \
            ++passed;                                                                                \
        else {                                                                                       \
            ++failed;                                                                                \
            std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #expr " ne " #Type " nahi feka\n"; \
        }                                                                                            \
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

/* ============================ bina DB wale ============================ */
static void testValue() {
    section("Value");
    Value v = {{"name", "Araham"}, {"age", 24}, {"ok", true}, {"pi", 3.5}, {"tags", Value::array({"a", "b"})},
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
    section("Schema: cast + defaults + validation (bina DB)");
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

    auto d = M({{"name", "  Araham  "}, {"email", "A@B.COM"}, {"code", "ab"}, {"age", "24"}, {"active", "false"},
                {"joined", "2026-01-02T03:04:05.000Z"}, {"slug", "x"}, {"tags", Value::array({"IoT", "ESP"})},
                {"address", {{"city", "Durgapur"}, {"pin", "713402"}}},
                {"items", Value::array({{{"sku", "A1"}}, {{"sku", "B2"}, {"qty", 3}}})},
                {"any", {{"kuch", "bhi"}}}, {"faltu", 1}});
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
    d.validate();  // sab theek

    CHECK_THROWS(M({{"age", "abc"}}), CastError);
    CHECK_THROWS(M({{"joined", "kal"}}), CastError);
    CHECK_THROWS(M({{"address", 5}}), CastError);

    auto bad = M({{"name", "A"}, {"email", "galat"}, {"age", 200}, {"role", "raja"}, {"even", 3},
                  {"address", {{"pin", 1}}}, {"items", Value::array({{{"qty", 0}}})}});
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
        CHECK(e.errors.size() == 2 && e.errors.count("name") && e.errors.count("address.city"));  // nested ke required bhi
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
    CHECK(!uv.contains("_id"));  // khaali id nahi jata
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

    auto a = User.create({{"name", " Araham "}, {"email", "ARAHAM@X.IN"}, {"age", 24}, {"password", "chhupa"},
                          {"tags", Value::array({"iot", "cpp"})}, {"address", {{"city", "Durgapur"}, {"pin", 713402}}},
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

    // findById: string id bhi chalti hai, select(false) field nahi aata
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
    auto agg = User.aggregate(Value::array({{{"$group", {{"_id", "$role"}, {"n", {{"$sum", 1}}}}}},
                                            {{"$sort", {{"_id", 1}}}}}));
    CHECK(agg.size() == 2 && agg[0]["_id"].asString() == "admin" && agg[0]["n"].asInt() == 2 && agg[1]["n"].asInt() == 3);

    // delete
    CHECK(User.deleteOne({{"email", "naya@x.in"}}) == 1);
    auto gone = User.findOneAndDelete({{"email", "mohan@x.in"}});
    CHECK(gone && (*gone)["name"].asString() == "Mohan Lal");
    CHECK(User.findByIdAndDelete(ravi.id()).has_value());
    a.deleteOne();
    CHECK(User.countDocuments() == 1);
    CHECK(User.deleteMany() == 1);

    // save() jab document DB se gayab ho
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
    Schema s({{"first", String().required()}, {"last", String()}, {"password", String()}, {"deleted", Boolean().def(false)},
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
    CHECK(P.findById(p._id()).orFail()["password"].asString() == "hash:1234");  // dobara hash nahi
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
    auto p1 = Post.create({{"title", "Godaan"}, {"author", a1.id()},  // string id -> ObjectId cast
                           {"tags", Value::array({t1._id(), t2._id(), missing})},
                           {"comments", Value::array({{{"by", a2._id()}, {"text", "waah"}}, {{"by", a1._id()}, {"text", "dhanyavaad"}}})}});
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
    one.populate("author");  // dobara: kuch nahi bigadta
    CHECK(one["author"]["name"].asString() == "Premchand");

    auto lean = Post.findOne({{"title", "Gitanjali"}}).populate("author").lean();
    CHECK(lean && (*lean)["author"]["name"].asString() == "Tagore");
    CHECK_THROWS(Post.find().populate("plain").exec(), Error);

    // ref se filter: string id cast hoti hai
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
    CHECK(!Device({{"name", "x"}, {"unit", "C"}}).has("unit"));  // base schema me unit nahi

    CHECK(Device.countDocuments() == 3);
    CHECK(Sensor.countDocuments() == 1 && Relay.countDocuments() == 1);
    CHECK(Sensor.find().exec()[0]["unit"].asString() == "C");
    auto all = Device.find().sort("name").exec();
    CHECK(all[0].modelName() == "Device" && all[1].modelName() == "Relay" && all[2].modelName() == "Sensor");
    all[2].set("value", "40.25").save();  // base se mila, par Sensor schema se cast
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
                     CHECK(Acc.countDocuments() == 3);  // transaction ke andar dikhta hai
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

int main() {
    testValue();
    testSchemaOffline();

    const char *env = std::getenv("NEVLA_TEST_URI");
    std::string uri = env ? env : "mongodb://127.0.0.1:27017/?directConnection=true";
    CHECK_THROWS(model("OfflineThing").countDocuments(), Error);  // connect se pehle
    try {
        connect(uri, "nevla_test");
    } catch (const Error &e) {
        std::cout << "MongoDB se connect nahi hua (" << e.what() << ")\n";
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
    disconnect();
    CHECK(!connected());

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
