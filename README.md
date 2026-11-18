# nevla

**A Mongoose-style ODM for MongoDB in modern C++ (C++20).**

Schemas, validation, casting, defaults, middleware (hooks), populate, virtuals, discriminators, transactions,
change streams and a chainable query builder — the API you know from Node.js
[Mongoose](https://mongoosejs.com), on top of the official MongoDB C++ driver (`mongocxx`).
Plus what Mongoose does not have: compile-time checked typed models, `co_await`, an in-memory database for
tests, field encryption, multi-tenancy, migrations and more.

> *nevla* (नेवला) is the Hindi word for *mongoose*.

[![CI](https://github.com/<your-username>/nevla/actions/workflows/ci.yml/badge.svg)](https://github.com/<your-username>/nevla/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

```cpp
#include <nevla/nevla.hpp>
using namespace nevla;

int main() {
    connect("mongodb://127.0.0.1:27017/shop");

    Schema userSchema({
        {"name",  String().required().trim().minLength(2)},
        {"email", String().required().lowercase().unique().match(R"(^\S+@\S+\.\S+$)")},
        {"age",   Int().min(13, "{PATH} must be at least 13, got {VALUE}")},
        {"role",  String().enumOf({"user", "admin"}).def("user")},
    }, {.timestamps = true});

    auto User = model("User", userSchema);   // collection "users"

    auto u = User.create({{"name", " Asha "}, {"email", "ASHA@X.IN"}, {"age", "24"}});  // trimmed, lowercased, cast
    auto adults = User.find().where("age").gte(18).sort("-createdAt").limit(10).exec();
    u.set("age", 25).save();                 // sends only {$set: {age: 25, updatedAt: ...}}
}
```

Hindi/Hinglish guide: [docs/README.hinglish.md](docs/README.hinglish.md)

---

## Contents

1. [Why nevla](#why-nevla)
2. [Requirements and installation](#requirements-and-installation)
3. [Using nevla in your project](#using-nevla-in-your-project)
4. [Connecting](#connecting)
5. [Value: dynamic JSON/BSON data](#value-dynamic-jsonbson-data)
6. [Schemas and field types](#schemas-and-field-types)
7. [Validation](#validation)
8. [Casting, defaults, setters, getters, aliases](#casting-defaults-setters-getters-aliases)
9. [Models and documents](#models-and-documents)
10. [Querying](#querying)
11. [Cursors and pagination](#cursors-and-pagination)
12. [Updating](#updating)
13. [Deleting](#deleting)
14. [Bulk writes](#bulk-writes)
15. [Aggregation builder](#aggregation-builder) · [Time buckets](#time-buckets-for-charts-bucketbytime) · [Populate in pipelines](#populate-inside-a-pipeline) · [One-line reports](#one-line-reports) · [Formula strings](#formula-strings) · [Distinct and counting](#distinct-and-counting)
16. [Sub-documents, arrays and maps](#sub-documents-arrays-and-maps)
17. [Populate](#populate)
18. [Middleware (hooks)](#middleware-hooks)
19. [Virtuals, methods, statics, plugins, toJSON](#virtuals-methods-statics-plugins-tojson)
20. [Indexes](#indexes)
21. [Custom `_id`](#custom-_id)
22. [Discriminators](#discriminators)
23. [Versioning and optimistic concurrency](#versioning-and-optimistic-concurrency)
24. [Transactions](#transactions)
25. [Change streams](#change-streams)
26. [Typed structs with `NEVLA_STRUCT`](#typed-structs-with-nevla_struct)

**Extras that Mongoose does not have**

27. [Typed models (member pointers)](#typed-models-member-pointers)
28. [co_await and event loops](#co_await-and-event-loops) · [Using nevla with Drogon](#using-nevla-with-drogon) · [GCC 13 tip](#gcc-13-no--inside-a-co_await-expression)
29. [In-memory database (`memory://`)](#in-memory-database-memory)
30. [Typo protection: `strictQuery`](#typo-protection-strictquery) · [Slow query log](#slow-query-log)
31. [Time-series collections](#time-series-collections)
32. [Batch writer](#batch-writer)
33. [REST query parameters: `fromQuery`](#rest-query-parameters-fromquery)
34. [Schema export and the MongoDB validator](#schema-export-and-the-mongodb-validator)
35. [Built-in plugins: soft delete, history, multi-tenant](#built-in-plugins-soft-delete-history-multi-tenant)
36. [Field encryption](#field-encryption)
37. [Migrations](#migrations)
38. [findById cache](#findbyid-cache)

**Reference**

39. [Errors](#errors)
40. [Threads, async and debugging](#threads-async-and-debugging)
41. [Schema options](#schema-options)
42. [Mongoose → nevla cheat sheet](#mongoose--nevla-cheat-sheet)
43. [Differences from Mongoose](#differences-from-mongoose)
44. [Project layout, tests and examples](#project-layout-tests-and-examples)
45. [Contributing, changelog, license](#contributing-changelog-license)

---

## Why nevla

The official `mongocxx` driver is powerful but low-level: you build BSON by hand, there is no schema, no
validation, no defaults, no relations. Every application ends up re-writing the same glue.

nevla gives C++ the developer experience of Mongoose:

| You get | Example |
|---|---|
| Schemas with types, validation and defaults | `Int().min(0).max(150)`, `String().enumOf({...})` |
| Automatic casting | `"24"` → `24`, `"2026-01-31"` → `Date`, hex string → `ObjectId` |
| Change tracking | `doc.set(...).save()` only sends the changed paths |
| Chainable queries | `User.find().where("age").gte(18).sort("-age").limit(10).exec()` |
| Relations | `populate("author")`, nested populate, virtual populate |
| Easy aggregation | `aggregate().match(...).group("device", {avg("value")}).exec()` with automatic casting |
| Formulas | `find("age >= 18 && role == 'admin'")`, `addField("total", "price * qty")` |
| Dashboards | `bucketByTime("at", "15m", ...)`, `countBy("status")`, `stats("value")`, `latestPerGroup(...)` |
| Middleware | `pre/post` hooks for `save`, `validate`, `find`, `updateOne`, `aggregate`, ... |
| Real-world features | transactions, change streams, bulk writes, cursors, pagination, optimistic concurrency |
| C++ comfort | `NEVLA_STRUCT` maps your structs to documents at compile time |
| Typed models | `Devices.find().where(&Device::temp).gte(30)`: a typo is a compile error |
| `co_await` | `co_await User.find(...).countAsync()` in `drogon::Task<>` or any coroutine; resumes on your IO loop |
| Tests without a server | `connect("memory://test")`: a built-in, MongoDB-compatible engine |
| Safety | `strictQuery` ("Did you mean `email`?"), `syncValidator()`, field encryption (AES-256-GCM) |
| SaaS / IoT building blocks | multi-tenant, soft delete, history, migrations, time-series, batch writer, `fromQuery`, cache, slow log |

It is **thread-safe**, uses a **connection pool**, and is tested against a real MongoDB server
(1,658 checks, including 930 query results compared between MongoDB and the in-memory engine; clean under
AddressSanitizer / UBSan and ThreadSanitizer).

**Is it a new driver?** No. nevla is built on top of the official **mongocxx** driver, which does the wire
protocol, connection pooling, TLS, authentication and replica-set handling. nevla adds the ODM layer above it.
The only engine of its own is the optional in-memory database for tests (`memory://`).

---

## Requirements and installation

- A C++20 compiler: **g++ 11+** or **clang 14+**
- **CMake 3.16+**
- **mongocxx 3.8+** (the MongoDB C++ driver; tested with 3.10.2)
- **OpenSSL 1.1+ / 3** (`libssl-dev`; used for field encryption)
- A MongoDB server (4.4+; transactions and change streams need a replica set or Atlas; time-series
  collections need 5.0+; `bucketByTime` with `fillGaps` and `dateDiff` in formulas need 5.1+).
  Not needed at all with `memory://`.
- Optional: **Drogon** (only for `<nevla/drogon.hpp>` and example 08)

### 1. Install the MongoDB C++ driver (Ubuntu 22.04 / 24.04)

```bash
sudo apt update
sudo apt install -y build-essential cmake curl libmongoc-dev libbson-dev libssl-dev

curl -OL https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.10.2/mongo-cxx-driver-r3.10.2.tar.gz
tar xzf mongo-cxx-driver-r3.10.2.tar.gz
cd mongo-cxx-driver-r3.10.2/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build . -j$(nproc)
sudo cmake --install .
sudo ldconfig
cd ../..
```

macOS: `brew install mongo-cxx-driver`. Windows: `vcpkg install mongo-cxx-driver`.

### 2. Build nevla

```bash
git clone https://github.com/<your-username>/nevla.git
cd nevla
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# optional: run the tests (needs MongoDB on 127.0.0.1:27017) ...
./build/nevla_tests
# ... or without any server
NEVLA_TEST_URI=memory://t ./build/nevla_tests
# optional: install system-wide
sudo cmake --install build
```

CMake options: `-DNEVLA_BUILD_TESTS=OFF -DNEVLA_BUILD_EXAMPLES=OFF` (both default to `ON` only when nevla is
the top-level project).

### A MongoDB server for development

```bash
# Docker, single-node replica set (transactions + change streams work)
docker run -d --name mongo -p 27017:27017 mongo:7 --replSet rs0
docker exec mongo mongosh --quiet --eval 'rs.initiate()'
```

Use `mongodb://127.0.0.1:27017/?directConnection=true` to connect to it.

---

## Using nevla in your project

**A. `add_subdirectory` (vendored copy)**

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp CXX)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(third_party/nevla)
add_executable(myapp main.cc)
target_link_libraries(myapp PRIVATE nevla::nevla)
```

**B. `FetchContent`**

```cmake
include(FetchContent)
FetchContent_Declare(nevla GIT_REPOSITORY https://github.com/<your-username>/nevla.git GIT_TAG v0.5.0)
FetchContent_MakeAvailable(nevla)
target_link_libraries(myapp PRIVATE nevla::nevla)
```

**C. After `cmake --install`**

```cmake
find_package(nevla 0.5 CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE nevla::nevla)
```

Then a single include:

```cpp
#include <nevla/nevla.hpp>
using namespace nevla;
```

> With `-Wextra`, g++ warns *"missing initializer"* for designated initializers such as `{.timestamps = true}`.
> That is a GCC quirk, not a bug; add `-Wno-missing-field-initializers` if it bothers you.

---

## Connecting

```cpp
connect("mongodb://127.0.0.1:27017", "mydb");          // URI + database name
connect("mongodb://127.0.0.1:27017/mydb");             // database from the URI
connect("mongodb+srv://user:pass@cluster0.mongodb.net/mydb?retryWrites=true&w=majority");  // Atlas
connect("memory://test");                               // built-in in-memory database (tests, demos)

connected();          // bool
databaseName();       // "mydb"
ping();               // throws nevla::Error if the server is unreachable
disconnect();
```

- `connect()` pings the server and throws `nevla::Error` right away if it is unreachable.
- There is a connection **pool** inside, so every model can be used from many threads at once.
- `connect()` may be called again (reconnect / switch database). Operations that are already running keep
  the old pool alive until they finish — no crash, no dangling pointers.
- No database in the URI and none passed → `"test"`.
- Models may be created before `connect()`; database operations need a connection.

---

## Value: dynamic JSON/BSON data

`Value` is nevla's JSON/BSON type: null, bool, int64, double, string, `Date`, `ObjectId`, array and object.
Objects keep insertion order (like BSON).

```cpp
Value v = {{"name", "Asha"}, {"age", 21}, {"tags", Value::array({"iot", "cpp"})},
           {"address", {{"city", "Durgapur"}, {"pin", 713209}}}};

v["name"].asString();         // "Asha"
v["age"].asInt();             // 21
v.path("address.city")->str();
v["missing"].isNull();        // true (const access never creates keys)
v["score"] = 9.5;             // non-const access creates the key
v.pathRef("meta.seen") = true;
v.erasePath("address.pin");

for (auto &[key, val] : v.items()) { /* object members */ }
for (auto &e : v["tags"].elements()) { /* array items */ }

std::string json = v.toJson(2);                  // pretty JSON
Value back = Value::parse(R"({"a": 1, "when": {"$date": "2026-01-31T10:00:00Z"}})");  // Extended JSON ok
```

| Need | Write |
|---|---|
| object | `{{"k", 1}, {"k2", "x"}}` or `Value::object()` |
| array | `Value::array({1, 2, 3})` (always an array) |
| date | `Date::now()`, `Date::parse("2026-01-31T10:20:30+05:30")` (validates calendar dates) |
| ObjectId | `ObjectId::generate()`, `ObjectId("665f...")`, `ObjectId::isValid(s)` |
| type checks | `isNull() isBool() isInt() isDouble() isNumber() isString() isDate() isObjectId() isArray() isObject()` |
| lenient getters | `asBool() asInt() asDouble() asString() str() asDate() asObjectId()` |
| typed | `v.as<int>()`, `v.as<std::vector<std::string>>()`, `v.as<std::map<std::string, double>>()`, `v.as<MyStruct>()` |

> A brace list whose items are all `{"string", value}` pairs is an object. For an *array* of such pairs use
> `Value::array({...})`.

---

## Schemas and field types

```cpp
Schema productSchema({
    {"title",    String().required().trim().maxLength(120)},
    {"price",    Number().min(0)},
    {"stock",    Int().def(0)},
    {"active",   Boolean().def(true)},
    {"launched", DateTime()},
    {"shop",     Id().ref("Shop")},                         // ObjectId reference (populate)
    {"tags",     Array(String().lowercase())},
    {"variants", Array(Schema({{"sku", String().required()}, {"qty", Int().min(0)}}))},
    {"dims",     Object(Schema({{"w", Number()}, {"h", Number()}}))},
    {"prices",   Map(Number().min(0))},                     // {"inr": 499, "usd": 6}
    {"extra",    Mixed()},                                  // anything, no casting
}, {.timestamps = true});
```

| Type | Stores | Cast from |
|---|---|---|
| `String()` | string | numbers, bools, dates, ObjectIds |
| `Number()` | int or double | numeric strings (`"12.5"`), bools |
| `Int()` | int64 | whole numbers, `"42"`, `3.0` (rejects `3.5`) |
| `Boolean()` | bool | `"true"/"false"`, `"1"/"0"`, `"yes"/"no"`, 0/1 |
| `DateTime()` | Date | ISO strings, epoch milliseconds |
| `Id()` | ObjectId | 24-character hex strings |
| `Array(Field)` | array of a type | a single value becomes a one-item array |
| `Array(Schema)` | array of sub-documents | objects |
| `Object(Schema)` | nested sub-document | object |
| `Map(Field)` | object with any keys, each value cast to `Field` | object |
| `Mixed()` | anything | — |

Field options (all chainable):

| Option | Meaning |
|---|---|
| `required(true, "msg")` | must be present and not null/empty string |
| `def(value)` / `def([]{ return ...; })` | default value / default function |
| `enumOf({...}, "msg")` | allowed values |
| `min(n, "msg")`, `max(n, "msg")` | numbers; also `min(Date)`, `max(Date)` for dates |
| `minLength(n, "msg")`, `maxLength(n, "msg")` | string length |
| `match("regex", "msg")` | ECMAScript regex, compiled once (an invalid regex throws immediately) |
| `trim()`, `lowercase()`, `uppercase()` | string transforms |
| `validate(fn, "msg")` | custom validator `bool(const Value&)` (several allowed; an exception counts as failure) |
| `set(fn)` | setter: transform before storing |
| `get(fn)` | getter: transform when reading with `doc.get()` / `toJSON()` (with `getters` option) |
| `alias("n")` | alternative name usable in `get/set`, filters, updates and `select` |
| `unique()`, `index()`, `sparse()`, `expires(seconds)` | indexes (see [Indexes](#indexes)) |
| `select(false)` | hidden from queries unless requested with `"+field"` |
| `immutable()` | cannot change after the first save (also stripped from updates) |
| `ref("Model")` | for `populate()` |

Messages can use `{PATH}` and `{VALUE}` placeholders: `Int().min(18, "{PATH} must be 18+, got {VALUE}")`.

Schemas can be extended: `schema.add("field", Field)`, `schema.add(otherSchema)`, `schema.remove("field")`.

---

## Validation

Validation runs automatically on `save()` / `create()` / `insertMany()` (and on updates with
`{.runValidators = true}`). Call `doc.validate()` yourself if you want.

```cpp
try {
    User.create({{"name", "A"}, {"email", "bad"}, {"age", "abc"}});
} catch (const ValidationError &e) {
    e.what();                    // "User validation failed: age: ..., email: ..., name: ..."
    e.errors.at("email");        // message per path
    e.kinds.at("age");           // "cast"
    e.kinds.at("name");          // "minlength"
}
```

`kinds` follow Mongoose: `required`, `enum`, `min`, `max`, `minlength`, `maxlength`, `regexp`,
`user defined`, `cast`.

Like Mongoose, a value that **cannot be cast** (`"abc"` for an `Int`) does not throw when you build or `set()` a
document; it is reported by `validate()` / `save()` as a `ValidationError` with kind `"cast"`.
Filters and updates with values that cannot be cast throw `CastError` immediately.

Required fields inside a nested `Object(Schema)` are validated even when the whole object is missing.

---

## Casting, defaults, setters, getters, aliases

```cpp
Schema s({
    {"name",  String().trim().alias("n")},
    {"slug",  String().set([](const Value &v) { return Value(slugify(v.asString())); })},
    {"paise", Int().get([](const Value &v) { return Value(v.asDouble() / 100); })},   // stored in paise
    {"code",  String().uppercase().def([] { return Value(randomCode()); })},
}, {.getters = true});

auto doc = M({{"n", "  Asha  "}, {"paise", "1250"}});
doc["name"];         // "Asha"     (raw stored value, alias resolved)
doc.get("n");        // "Asha"
doc["paise"];        // 1250       (raw)
doc.get("paise");    // 12.5       (getter applied)
doc.toJSON();        // getters applied because of {.getters = true}
```

- Defaults are applied when a document is built and, for upserts, through `$setOnInsert`.
- Setters run on build, `set()`, and updates.
- `strict` (default on): paths that are not in the schema are dropped — from documents **and** from updates.

---

## Models and documents

```cpp
auto User = model("User", userSchema);              // collection "users"
auto Person = model("Person", personSchema);        // collection "people" (Mongoose-style pluralization)
auto Log = model("Log", logSchema, "app_logs");     // explicit collection
auto again = model("User");                         // look up a registered model
modelNames();                                       // {"Log", "Person", "User"}

Document d = User({{"name", "Asha"}});   // new, not saved yet
d.isNew();                              // true
d.save();                               // insert
auto created = User.create({{"name", "Ravi"}});          // build + validate + insert
auto many = User.insertMany({{{"name", "A"}}, {{"name", "B"}}}, {.ordered = false});
auto doc = User.hydrate(rawValueFromSomewhere);          // Value -> Document (isNew = false)
```

Reading a document:

```cpp
doc["name"].asString();             // raw value, reference (null if missing)
doc.get("name");                    // with virtuals, aliases and getters
doc.get<int>("age");
doc.has("address.city");
doc.id();  doc._id();
doc.toObject();  doc.toJSON();  doc.json(2);
doc.as<UserRow>();                  // into a NEVLA_STRUCT
```

Changing a document:

```cpp
doc.set("age", 25);                         // cast + setter + change tracking
doc.set({{"age", 26}, {"role", "admin"}});
doc.unset("nickname");
doc.push("tags", "cpp");
doc.mut("address").pathRef("city") = "Kolkata";   // in-place edit (marks the path modified)
doc.markModified("extra");
doc.overwrite({{"name", "New"}});           // replace every field (keeps _id, version, createdAt)
doc.isModified("age");  doc.modifiedPaths();
doc.save();                                 // $set / $unset of changed paths only
doc.updateOne({{"$inc", {{"visits", 1}}}}); // Model.updateOne({_id}, ...)
doc.deleteOne();
doc.equals(other);                          // same _id
```

---

## Querying

```cpp
auto users = User.find({{"role", "admin"}}).exec();              // std::vector<Document>
auto rows  = User.find().lean();                                 // std::vector<Value> (faster, read-only)
auto one   = User.findOne({{"email", "a@x.in"}}).exec();         // std::optional<Document>
auto byId  = User.findById("665f1c...").orFail();                // throws NotFoundError if missing
```

The query builder (all chainable):

```cpp
User.find()
    .where("age").gte(18).lt(60)
    .where("role").in({"user", "admin"})
    .where("tags").all({"iot", "cpp"})
    .where("tags").size(2)
    .where("email").regex("@example\\.com$", "i")
    .where("deletedAt").exists(false)
    .where({{"active", true}})                      // or an object of conditions
    .or_({{{"plan", "pro"}}, {{"credits", {{"$gt", 0}}}}})
    .sort("-createdAt name")                        // or {{"createdAt", -1}}
    .skip(20).limit(10)
    .select("name email -_id")                      // "-password", "+secret" (select(false) field)
    .populate("company")
    .collation({{"locale", "en"}, {"strength", 2}}) // case-insensitive
    .hint("email_1").maxTimeMS(2000).batchSize(100).comment("admin list")
    .exec();

User.where("age").gte(18).count();                  // model-level where()
```

| Method | MongoDB |
|---|---|
| `equals gt gte lt lte ne in nin all size mod exists regex elemMatch` | same-named operators |
| `or_ and_ nor` | `$or $and $nor` (`or`/`and` are C++ keywords) |
| `text("words")` | `$text` (needs a text index) |

Filters are cast with the schema: `{{"age", "18"}}` becomes `18`, `{{"_id", "665f..."}}` becomes an `ObjectId`,
`$in` arrays, `$elemMatch` on sub-document arrays and aliases are handled too.

Any filter can also be a **formula string** (see [Formula strings](#formula-strings)):

```cpp
User.find("age >= 18 && role in ['admin', 'editor']").sort("-age").exec();
User.countDocuments("plan == 'pro' || credits > 0");
User.updateMany("lastSeen < date('2026-01-01')", {{"active", false}});
User.deleteMany("used > limit");                    // field compared with field -> $expr
```

---

## Cursors and pagination

```cpp
// Stream huge result sets without loading them into memory
auto cur = Log.find({{"level", "error"}}).sort("-at").batchSize(500).cursor();
while (auto doc = cur.next()) process(*doc);           // or cur.nextLean() for Values

Log.find().forEach([](Document &d) { /* ... */ });

// Pagination
Page p = Post.find({{"published", true}}).sort("-createdAt").paginate(/*page*/ 2, /*perPage*/ 20);
p.docs;  p.total;  p.page;  p.pages;  p.hasNext;  p.hasPrev;
```

---

## Updating

```cpp
User.updateOne({{"_id", id}}, {{"name", "Asha"}});                   // plain fields -> $set
User.updateOne({{"_id", id}}, {{"$inc", {{"visits", 1}}}, {"$push", {{"tags", "new"}}}});
User.updateMany({{"role", "guest"}}, {{"active", false}});
User.updateOne({{"email", "x@y.z"}}, {{"name", "X"}}, {.upsert = true, .runValidators = true});

auto after = User.findOneAndUpdate({{"_id", id}}, {{"$inc", {{"n", 1}}}},
                                   {.returnNew = true, .select = "n"});
User.findByIdAndUpdate(id, {{"role", "admin"}});
User.replaceOne({{"_id", id}}, {{"name", "Only this"}, {"email", "a@b.c"}});
User.findOneAndReplace({{"_id", id}}, replacement, {.returnNew = true});
```

What nevla does to every update (Mongoose behaviour):

- plain keys are wrapped in `$set`; aliases are resolved;
- values are cast for `$set`, `$setOnInsert`, `$min`, `$max`, `$inc`, `$mul`, `$push`, `$addToSet`, `$pull`,
  `$pullAll` — including positional paths like `items.$.qty`, `items.$[].qty`, `items.0.qty` and map keys;
- unknown paths are removed (`strict`), `immutable` fields are removed (or moved to `$setOnInsert` on upsert);
- `$inc` / `$mul` on a non-number path throws `CastError`;
- `runValidators`: validators for `$set` / `$setOnInsert`, `required` for `$unset`;
- `timestamps`: `updatedAt` is set, `createdAt` is never changed by an update and is set on upsert;
- upserts get schema defaults and the discriminator key through `$setOnInsert`.

`UpdateResult` = `{matchedCount, modifiedCount, upsertedId}`.

---

## Deleting

```cpp
User.deleteOne({{"email", "a@x.in"}});       // returns deleted count
User.deleteMany({{"active", false}});
User.findOneAndDelete({{"_id", id}});        // std::optional<Document>
User.findByIdAndDelete(id);
doc.deleteOne();                             // runs deleteOne document hooks
```

---

## Bulk writes

Operations use the same shape as Mongoose's `Model.bulkWrite()`:

```cpp
auto r = Product.bulkWrite({
    {{"insertOne",  {{"document", {{"sku", "A1"}, {"qty", 5}}}}}},
    {{"updateOne",  {{"filter", {{"sku", "B2"}}}, {"update", {{"$inc", {{"qty", 1}}}}}, {"upsert", true}}}},
    {{"updateMany", {{"filter", {{"qty", 0}}}, {"update", {{"soldOut", true}}}}}},
    {{"replaceOne", {{"filter", {{"sku", "C3"}}}, {"replacement", {{"sku", "C3"}, {"qty", 1}}}}}},
    {{"deleteOne",  {{"filter", {{"sku", "D4"}}}}}},
    {{"deleteMany", {{"filter", {{"discontinued", true}}}}}},
}, /*ordered*/ true);
r.insertedCount; r.matchedCount; r.modifiedCount; r.deletedCount; r.upsertedCount;
```

Inserted documents are validated; updates and filters are cast exactly like `updateOne()`.

---

## Aggregation builder

An aggregation **pipeline** sends your documents through a list of **stages**. Each stage does one job
(filter, group, sort, ...) and hands its result to the next one. All the work happens inside MongoDB, so
only the small final result comes back to your program.

```
all readings ──► match (filter) ──► group (avg / max / count) ──► sort ──► limit ──► result
```

Writing a pipeline by hand means many braces, `"$field"` strings and building `ObjectId`/`Date` yourself.
nevla's builder makes it read like a `find()` query:

```cpp
auto top = Point.aggregate()
    .match({{"device", deviceId}, {"at", {{"$gte", "2026-11-17"}}}})   // plain strings: cast for you
    .group("device", {avg("value"), max("value"), count()})            // -> {_id, avgValue, maxValue, count}
    .sort("-avgValue")
    .limit(5)
    .exec();                                                           // std::vector<Value>

top[0]["_id"];        // the device id (the group key)
top[0]["avgValue"];   // average of "value"
```

The same pipeline written by hand, for comparison:

```cpp
Point.aggregate(Value::array({
    {{"$match", {{"device", ObjectId(deviceId)}, {"at", {{"$gte", *Date::parse("2026-11-17")}}}}}},
    {{"$group", {{"_id", "$device"}, {"avgValue", {{"$avg", "$value"}}}, {"maxValue", {{"$max", "$value"}}},
                 {"count", {{"$sum", 1}}}}}},
    {{"$sort", {{"avgValue", -1}}}},
    {{"$limit", 5}},
}));
```

### Automatic casting (not in Mongoose)

Mongoose never casts aggregation pipelines, so a string id in `$match` silently matches nothing.
nevla casts every `$match` with the schema **while the documents still have the schema's shape**:

- 24-hex string → `ObjectId`, `"2026-11-17"` → `Date`, `"10"` → `10` for number fields, aliases → real names;
- works in the builder **and** in hand-written `aggregate(Value::array({...}))` pipelines;
- stops after `group`, `project`, `addFields`, `replaceRoot`, `count` or a raw `stage(...)` (other than
  `$match/$sort/$limit/$skip/$sample/$unwind/$lookup`), because the fields no longer match the schema;
- a value that cannot be cast throws `CastError` right away (`match({{"value", "hot"}})`), instead of
  quietly returning nothing.

### Grouping

```cpp
.group("device", {avg("value"), min("value"), max("value"), sum("value"), count()})
.group("device room", {count()})           // several keys -> _id: {device, room}
.group("", {sum("amount")})                // the whole collection -> _id: null
.group("dev.room", {avg("value")})         // a nested path (e.g. after lookup + unwind)
```

| Accumulator | MongoDB | Output field |
|---|---|---|
| `count()` | `{$sum: 1}` | `count` |
| `sum("value")` | `{$sum: "$value"}` | `sumValue` |
| `avg("value")` | `{$avg: "$value"}` | `avgValue` |
| `min("value")`, `max("value")` | `$min`, `$max` | `minValue`, `maxValue` |
| `first("value")`, `last("value")` | `$first`, `$last` (use `sort()` before) | `firstValue`, `lastValue` |
| `push("value")`, `addToSet("tag")` | `$push`, `$addToSet` | `pushValue`, `addToSetTag` |
| `stdDevPop("value")` | `$stdDevPop` | `stdDevPopValue` |
| `accumulator("$top", expr, "name")` | anything else | `name` |

Names come from the operation + the field in camelCase (`avg("reading.temp")` → `avgReadingTemp`).
Choose your own with `.as()`: `avg("value").as("meanTemp")`. Two accumulators with the same name are an error.

### All builder stages

| Builder | Stage |
|---|---|
| `match(filter)` / `match("temp > 30 && room == 'lab'")` | `$match` (cast, see above; formulas allowed) |
| `group(by, {accumulators})` | `$group` |
| `sort("-a b")` / `sort({{"a", -1}})` | `$sort` |
| `limit(n)`, `skip(n)`, `sample(n)` | `$limit`, `$skip`, `$sample` |
| `project("a b -_id")` / `project({{"total", expr}})` | `$project` |
| `addFields({{"total", expr}})` / `addField("total", "price * qty")` | `$addFields` |
| `bucketByTime("at", "15m", {avg("value")}, {...})` | time buckets for charts (see below) |
| `populate("author")` / `populate(PopulateOptions)` | `$lookup` from the schema's `ref` (see below) |
| `unwind("tags")` / `unwind("tags", true)` | `$unwind` (`true` keeps documents with empty arrays) |
| `lookup("Author", "author", "_id", "authorDoc")` | `$lookup` (a model name or a collection name) |
| `count("n")` | `$count` |
| `replaceRoot("doc")` | `$replaceRoot` |
| `stage({{"$facet", {...}}})` | any other stage, as is |
| `options({.allowDiskUse = true, .maxTimeMS = 5000, .collation = ..., .hint = ...})` | command options |

Running it:

```cpp
std::vector<Value> rows = agg.exec();
std::optional<Value> one = agg.first();              // adds $limit 1
std::vector<DeviceStat> typed = agg.exec<DeviceStat>();   // NEVLA_STRUCT; the group key "_id" -> member "id"
std::cout << agg.toJson();                           // the pipeline as JSON
std::cout << agg.toShell();                          // db.getCollection("readings").aggregate([... ObjectId(..), ISODate(..)])
std::cout << agg.plan();                             // "IXSCAN(device_1_at_-1)" or "COLLSCAN" (no index!)
Value plan = agg.explain();                          // MongoDB's full query plan
Value stages = agg.pipeline();
```

`toShell()` prints the pipeline exactly as you would type it in `mongosh` (with `ObjectId("...")` and
`ISODate("...")`), so you can paste it into mongosh / Compass to try it by hand. `plan()` tells you in one line
whether MongoDB uses an index: `COLLSCAN` means it reads the whole collection — add an index.

A full example: device report with a join.

```cpp
struct RoomStat {
    std::string id;          // the room (group key)
    double avgValue = 0;
    int64_t count = 0;
};
NEVLA_STRUCT(RoomStat, id, avgValue, count)

auto rooms = Reading.aggregate()
    .match({{"at", {{"$gte", "2026-11-01"}}}})
    .lookup("Device", "device", "_id", "dev")       // join the device of every reading
    .unwind("dev")
    .group("dev.room", {avg("value"), count()})
    .sort("-avgValue")
    .exec<RoomStat>();
```

Good to know:

- Put `match()` as early as possible: it can use indexes and every later stage gets less data.
- Discriminator models add a `$match` on their type automatically (`pipeline()` shows it).
- `pre("aggregate")` / `post("aggregate")` hooks see the builder's pipeline too.
- A read that the server interrupts because a collection was created or dropped during the query
  (`QueryPlanKilled`) is retried automatically (not for `$out` / `$merge`, not inside transactions).
- On Windows, define `NOMINMAX` before including `<windows.h>` so `min()` / `max()` are not macros.

### Time buckets for charts: `bucketByTime`

One row per time slot — exactly what a dashboard chart needs:

```cpp
auto rows = Reading.aggregate()
    .match({{"device", id}, {"at", {{"$gte", "2026-11-18"}}}})
    .bucketByTime("at", "15m", {avg("value"), min("value"), max("value"), count()})
    .exec();
// [{time: 2026-11-18T10:00Z, avgValue: 24.1, minValue: 22, maxValue: 26, count: 12},
//  {time: 2026-11-18T10:15Z, ...}, ...]          sorted by time, "time" is the first field
```

| Option | Meaning |
|---|---|
| interval | `"30s"`, `"15m"`, `"1h"`, `"6h"`, `"1d"`, `"1w"`, `"1M"` (month), `"1q"`, `"1y"`; also `"2 hours"` |
| accumulators | as in `group()`; none = `count()` |
| `.by = "device"` | one series per device: rows get a `device` field too (`"device room"` for several) |
| `.timezone = "Asia/Kolkata"` | days / weeks / months start at local midnight instead of UTC |
| `.fillGaps = true` | add the empty slots too (`count`/`sum` = 0, others null) so the chart has no holes (MongoDB 5.1+) |

```cpp
// hourly average per device for the last day, in Indian time, with empty hours filled in
Reading.aggregate()
    .match("at >= date('2026-11-17T00:00:00+05:30')")
    .bucketByTime("at", "1h", {avg("value")}, {.by = "device", .timezone = "Asia/Kolkata", .fillGaps = true})
    .exec();
```

Uses MongoDB's `$dateTrunc` (MongoDB 5.0+).

### Populate inside a pipeline

`populate()` in the builder uses the schema's `ref` (or a `virtualPopulate`) and writes the `$lookup` for you —
no collection names, no `localField` / `foreignField` to remember:

```cpp
// readings per room, the room comes from the referenced device
Reading.aggregate().populate("device").group("device.room", {avg("value"), count()}).exec();

PopulateOptions author;
author.path = "author";
author.select = "name company";               // select(false) fields stay hidden
author.match = "active == true";              // object or formula; no match -> null
PopulateOptions company;
company.path = "company";
author.populate.push_back(company);           // nested
Post.aggregate().match("published").populate(author).populate("tags").exec();

Author.aggregate().populate("posts").addField("postCount", "size(posts)").sort("-postCount").exec();  // virtual
```

- single refs become the document or `null`; arrays of ids keep their order and drop ids that no longer exist
  (same as `Query.populate()`); `sort` / `limit` work per document;
- it must come **before** `group()` / `project()` / `addFields()` (the schema is needed);
- paths inside arrays of sub-documents (`comments.by`) are not supported here — use `find().populate()`;
- needs MongoDB 5.0+ (`$lookup` with `localField` + `pipeline`).

### One-line reports

The most common dashboard questions, each in one call (filters may be objects or formulas):

```cpp
Order.countBy("status");                          // {"paid": 120, "pending": 8}        biggest first
Order.countBy("status", "createdAt >= date('2026-11-01')");
Order.sumBy("customer", "amount");                // {"asha": 5400, "ravi": 1200}
Reading.avgBy("device", "value", "value > 0");    // {"<deviceId>": 24.5, ...}
Reading.stats("value");                           // {count, sum, avg, min, max, stdDev}  (numbers only)
Reading.latestPerGroup("device", "at");           // the newest reading of every device (whole documents)
```

Keys are text (`ObjectId` → hex, `null` → `"null"`). `stats()` on no data returns `count: 0` and nulls.

---

## Formula strings

Write expressions and conditions like normal maths / C++ instead of nested MongoDB objects.
`nevla::expr()` makes an aggregation expression, `nevla::formulaFilter()` makes a query filter — and every place
that takes a filter or an expression accepts a formula string directly:

```cpp
Post.find("published && views >= 100 && tag in ['cpp', 'iot']").exec();
Order.aggregate()
    .match("status == 'paid' && createdAt >= date('2026-11-01')")
    .addField("total", "price * qty * (1 - discount)")
    .group("customer", {sum("total"), sum("price * qty").as("gross"), count()})
    .exec();
User.aggregate().addField("label", "if(age >= 18, 'adult', 'minor')").group("label", {count()}).exec();
```

| | Write |
|---|---|
| values | `12` `3.5` `1e3` `'text'` `"text"` `true` `false` `null` `[1, 2]` `date('2026-11-17')` `now()` |
| fields | `price` `address.city` `items.0.qty` (or `$price`) |
| maths | `+ - * / %` and unary `-` |
| compare | `== != < <= > >=` (a single `=` also means equality), `x in [..]`, `x not in [..]` |
| logic | `&& \|\| !` or `and or not` |
| functions | `abs ceil floor round(x, places) trunc sqrt pow exp ln log10 log(x, base)` |
| | `min max sum avg` (several values, or one array field), `size(array)` |
| | `concat lower upper trim len substr(s, start, count)` |
| | `if(cond, a, b)` `ifNull(a, b)` `exists(field)` |
| | `year month day hour minute second dayOfWeek` (date[, 'Asia/Kolkata']), `dateDiff(a, b, 'day')` |
| | `toInt toDouble toString toDate toBool` |

**Filters stay fast.** A part like `field <op> constant` becomes a normal filter condition, so it uses indexes
and gets the schema's casting (`"at >= '2026-11-17'"` compares with a real `Date`). Only parts that compare
fields with each other or use maths/functions become `{$expr: ...}`:

```cpp
formulaFilter("temp > 30 && room == 'lab'")   // {temp: {$gt: 30}, room: "lab"}
formulaFilter("age >= 18 && age < 60")        // {age: {$gte: 18, $lt: 60}}
formulaFilter("used > limit")                 // {$expr: {$gt: ["$used", "$limit"]}}
formulaFilter("active && !deleted")           // {active: true, deleted: {$ne: true}}
```

**Clear errors.** A mistake throws `nevla::FormulaError` (a `nevla::Error`) with the column:

```
formula error at column 9: unexpected '*'
  price * * qty
          ^
```

Unknown functions list the available ones; `date('2026-02-30')` is rejected; `a < b < c` asks for
`a < b && b < c`. Use `+` for numbers and `concat()` for text. A text literal starting with `$` is text,
not a field (`'$5 off'`).

An accumulator with a formula needs a name: `sum("price * qty").as("revenue")`.

> **Changed in 0.4:** a string passed where a filter is expected is parsed as a formula. Before 0.4 it was
> ignored, so `User.find("anything")` matched every document. Now `find("age >= 18")` is a real filter, and
> a plain word is a field test: `find("active")` means `{active: true}`. Code that passed a stray string
> now gets the filter it wrote, or a `FormulaError`. Turn on `strictQuery` to also catch misspelled fields.

---

## Distinct and counting

```cpp
Order.countDocuments({{"status", "paid"}});
Order.find({{"status", "paid"}}).count();
Order.estimatedDocumentCount();
Order.exists({{"_id", id}});
Order.distinct("customer", {{"status", "paid"}});
Order.find({{"status", "paid"}}).distinct("customer");
```

---

## Sub-documents, arrays and maps

```cpp
Schema orderSchema({
    {"items",   Array(Schema({{"sku", String().required()}, {"qty", Int().def(1).min(1)}}))},
    {"address", Object(Schema({{"city", String().required()}, {"pin", Int()}}))},
    {"stock",   Map(Int().min(0))},
});

auto o = Order({{"items", Value::array({{{"sku", "A"}}})}, {"address", {{"city", "Kolkata"}}},
                {"stock", {{"dgp", 4}, {"kol", "2"}}}});
o["items"].at(0)["qty"];     // 1 (sub-document default)
o["stock.kol"];              // 2 (cast)
o.push("items", {{"sku", "B"}, {"qty", 3}});
o.set("address.pin", "700001");

Order.updateOne({{"items.sku", "A"}}, {{"$set", {{"items.$.qty", "5"}}}});   // positional, cast to 5
```

Map keys must not be empty, start with `$` or contain `.`.

---

## Populate

```cpp
Schema postSchema({
    {"title",    String()},
    {"author",   Id().ref("Author")},
    {"tags",     Array(Id().ref("Tag"))},
    {"comments", Array(Schema({{"by", Id().ref("Author")}, {"text", String()}}))},
});

Post.find().populate("author").populate("tags").populate("comments.by").exec();
doc.populate("author");
doc.populated("author");     // true
doc.depopulate("author");    // back to the ObjectId
```

With options (select, match, sort, per-document limit, nested populate, model override):

```cpp
PopulateOptions author;
author.path = "author";
author.select = "name company";
author.match = {{"active", true}};          // not matching -> null (arrays: dropped)
PopulateOptions company;
company.path = "company";
author.populate.push_back(company);         // populate inside the populated author

PopulateOptions tags;
tags.path = "tags";
tags.sort = "label";
tags.limit = 3;

Post.find().populate(author).populate(tags).exec();
```

**Virtual populate** (the "one-to-many from the other side"):

```cpp
Schema authorSchema({{"name", String()}});
authorSchema.virtualPopulate("posts", {.ref = "Post", .localField = "_id", .foreignField = "author"});
authorSchema.virtualPopulate("latestPost", {.ref = "Post", .localField = "_id", .foreignField = "author",
                                            .justOne = true});

auto a = Author.findById(id).populate("posts").orFail();
a["posts"].size();
```

Saving a populated document stores ids, never the embedded documents.

---

## Middleware (hooks)

```cpp
// Document middleware: "validate", "save", "deleteOne"
userSchema.pre("save", [](Document &d) {
    if (d.isModified("password")) d.set("password", hash(d["password"].asString()));
});
userSchema.post("save", [](Document &d) { audit("saved", d.id()); });

// Query middleware: "find", "findOne", "countDocuments", "updateOne", "updateMany", "replaceOne",
// "findOneAndUpdate", "findOneAndReplace", "findOneAndDelete", "deleteOne", "deleteMany", "aggregate"
userSchema.pre("find", [](QueryInfo &q) { q.filter["deletedAt"] = nullptr; });          // soft delete
userSchema.pre("updateOne", [](QueryInfo &q) { (*q.update)["$set"]["touched"] = true; });
userSchema.pre("aggregate", [](QueryInfo &q) {                                          // q.filter = pipeline
    q.filter.push_back(Value{{"$limit", 1000}});
});

// post query hooks receive (and may change) the result
userSchema.post("find", [](QueryInfo &q, Value &result) { metrics("find", result.size()); });
userSchema.post("updateOne", [](QueryInfo &, Value &r) { log(r["modifiedCount"].asInt()); });
```

Post-hook results: an array (`find`, `aggregate`), a document or null (`findOne`, `findOneAnd*`), a count
(`countDocuments`) or `{matchedCount, modifiedCount, upsertedId}` / `{deletedCount}` for writes.

---

## Virtuals, methods, statics, plugins, toJSON

```cpp
userSchema.virtual_("fullName", [](const Document &d) {
    return Value(d["first"].asString() + " " + d["last"].asString());
});
doc.get("fullName");

userSchema.method("isAdult", [](Document &d, const Value &) { return Value(d["age"].asInt() >= 18); });
doc.call("isAdult");

userSchema.static_("findByEmail", [](const Model &M, const Value &email) {
    auto d = M.findOne({{"email", email}}).lean();
    return d ? *d : Value();
});
User.call("findByEmail", "a@x.in");

// Plugins: reusable schema features
void timestampsPlugin(Schema &s) { /* add fields, hooks, methods ... */ }
void softDelete(Schema &s, const Value &opts) { s.add(opts["field"].asString(), DateTime()); }
userSchema.plugin(timestampsPlugin);
userSchema.plugin(softDelete, {{"field", "deletedAt"}});

// toJSON transform
userSchema.toJSON([](const Document &, Value &out) { out.erase("password"); });
```

`virtual` is a C++ keyword, hence `virtual_`; `static` likewise, hence `static_`.
With `{.virtuals = true}` `toJSON()` also includes virtuals, aliases and `id`.

---

## Indexes

```cpp
{"email",    String().unique()}
{"phone",    String().unique().sparse()}             // unique only when present
{"city",     String().index()}
{"expireAt", DateTime().expires(0)}                  // TTL index (seconds)

schema.index({{"userId", 1}, {"createdAt", -1}});                  // compound
schema.index({{"title", "text"}, {"body", "text"}});               // text
schema.index({{"at", 1}}, {{"expireAfterSeconds", 7 * 24 * 3600}});
```

- With `autoIndex` (default) indexes are created before the model's first operation. If that fails (for example
  duplicates already exist for a new unique index) **the model keeps working**; the error goes to the
  `onIndexError` handler (stderr by default):

  ```cpp
  nevla::onIndexError([](const std::string &model, const std::string &msg) { log.warn(model, msg); });
  ```

- `Model.createIndexes()` creates them now and throws on failure.
- `Model.syncIndexes()` creates missing indexes and drops indexes that are no longer in the schema.
- `Model.listIndexes()` returns the collection's indexes.

---

## Custom `_id`

```cpp
auto Setting = model("Setting", Schema({{"_id", String()}, {"value", Mixed()}}, {.versionKey = ""}));
Setting.create({{"_id", "theme"}, {"value", "dark"}});

auto Device = model("Device", Schema({{"_id", String().def([] { return Value(uuid()); })}, {"name", String()}}));
```

Without an `_id` field the schema uses an auto-generated `ObjectId`; 24-hex strings in filters are cast.

---

## Discriminators

Several document types in one collection (Mongoose discriminators):

```cpp
auto Device = model("Device", Schema({{"name", String().required()}, {"online", Boolean().def(false)}}));
auto Sensor = Device.discriminator("Sensor", Schema({{"unit", String().required()}, {"value", Number()}}));
auto Relay  = Device.discriminator("Relay",  Schema({{"state", Boolean().def(false)}}));

Sensor.create({{"name", "Temp"}, {"unit", "C"}});      // stored with __t: "Sensor"
Sensor.find().exec();                                  // only sensors
for (auto &d : Device.find().exec()) d.modelName();    // "Sensor" / "Relay": each cast with its own schema
```

---

## Versioning and optimistic concurrency

The version key (`__v`, configurable or `""` to disable) starts at 0 and is incremented when an array path
is saved or when you call `doc.increment()`.

With `{.optimisticConcurrency = true}` every `save()` checks the version it loaded:

```cpp
Schema s({{"qty", Int()}}, {.optimisticConcurrency = true});
auto a = Item.findById(id).orFail();
auto b = Item.findById(id).orFail();
a.set("qty", 2).save();
b.set("qty", 3).save();      // throws VersionError: somebody else saved it meanwhile
```

`save()` of a document that was deleted meanwhile throws `NotFoundError`.

---

## Transactions

Needs a replica set or Atlas.

```cpp
nevla::transaction([&] {
    Account.updateOne({{"_id", from}}, {{"$inc", {{"balance", -100}}}}, {.runValidators = true});
    Account.updateOne({{"_id", to}}, {{"$inc", {{"balance", 100}}}});
    Ledger.create({{"from", from}, {"to", to}, {"amount", 100}});
});
```

- Every nevla call **on the same thread** inside the lambda uses the transaction automatically.
- If the lambda throws, everything is rolled back and the exception is re-thrown.
- The lambda may run more than once (the driver retries transient errors) — keep it free of side effects
  outside the database.
- Nested `transaction()` calls join the outer transaction.

---

## Change streams

Needs a replica set or Atlas.

```cpp
auto stream = Order.watch(Value::array({{{"$match", {{"operationType", "insert"}}}}}),
                          {.fullDocument = true, .maxAwaitMS = 1000});
while (running) {
    if (auto ev = stream.next())                         // nullopt = nothing within maxAwaitMS
        notify((*ev)["fullDocument"]);
}
```

---

## Typed structs with `NEVLA_STRUCT`

```cpp
struct Address {
    std::string city;
    int pin = 0;
};
NEVLA_STRUCT(Address, city, pin)

struct Student {
    std::string id;                        // "id" <-> MongoDB "_id"
    std::string name;
    int roll = 0;
    double cgpa = 0;
    std::vector<std::string> subjects;
    std::optional<std::string> hostel;     // nullopt when missing
    std::map<std::string, int> marks;      // maps work too
    Address address;                       // nested struct
    Date admitted;
};
NEVLA_STRUCT(Student, id, name, roll, cgpa, subjects, hostel, marks, address, admitted)

auto doc = StudentModel.create(toValue(s));      // struct -> validated document
Student back = doc.as<Student>();                // document -> struct (id filled)
for (auto &v : StudentModel.find().lean()) list.push_back(v.as<Student>());
StudentModel.findById(back.id).orFail().set(toValue(back)).save();
```

Write the macro outside the struct, in the same namespace. Add your own types with
`to_value(Value&, const T&)` / `from_value(const Value&, T&)` overloads in the type's namespace.

---

## Typed models (member pointers)

`NEVLA_STRUCT` maps a struct; `TypedModel<T>` turns it into a model whose queries use **member pointers**, so
a typo or a wrong type is a **compile error**, not a silent empty result.

```cpp
#include <nevla/nevla.hpp>

struct Device {
    std::string id, name, room;          // "id" <-> "_id"
    double temp = 0;
    bool online = false;
    std::vector<std::string> tags;
    std::optional<std::string> note;     // missing / null <-> std::nullopt
};
NEVLA_STRUCT(Device, id, name, room, temp, online, tags, note)

TypedModel<Device> Devices("Device");                   // schema generated from the struct

Device d = Devices.create({.name = "temp-1", .room = "lab", .temp = 24.5});
std::vector<Device> hot = Devices.find()
    .where(&Device::temp).gte(30).lt(60)                // chain conditions on one field
    .where(&Device::room).in({"lab", "hall"})
    .sort(&Device::temp, -1).limit(10).exec();
std::optional<Device> one = Devices.findById(d.id);
int64_t n = Devices.find().where(&Device::online).equals(true).count();
Devices.update(d.id).set(&Device::temp, 26.0).push(&Device::tags, "calibrated").exec();
Devices.deleteById(d.id);
```

| Mistake | Result |
|---|---|
| `.where(&Device::tmep)` | compile error: no member `tmep` |
| `.where(&Device::temp).gte("hot")` | compile error: `temp` is a `double` |
| `.update(id).inc(&Device::name, 1)` | compile error: `$inc` on a string |

Want validation rules too? Start from the generated schema and add to it:

```cpp
Schema s = schemaOf<Device>({.timestamps = true});
s.add("name", String().required().trim());
s.add("room", String().enumOf({"lab", "hall", "roof"}));
TypedModel<Device> Devices("Device", s);
Devices.model();    // the untyped Model, for everything else (aggregate, fromQuery, plugins' statics ...)
```

Conditions: `equals ne gt gte lt lte in nin exists contains regex`; nested members:
`where(&User::address, &Address::city)`; anything else: `where("formula string")` or `where(Value)`.

---

## co_await and event loops

Every operation has an `...Async()` form that returns `nevla::Async<T>`. The database work runs on nevla's
thread pool; the coroutine continues when the result is there, so an event-loop thread is **never blocked**.

```cpp
Task<int64_t> dashboard(Model Users) {
    auto adults = co_await Users.find("age >= 18").countAsync();
    auto rows   = co_await Users.find("role == 'admin'").sort("-createdAt").limit(5).leanAsync();
    auto doc    = co_await Users.createAsync(body);          // ValidationError is thrown here, as usual
    co_await Users.updateOneAsync(filter, update);
    co_return adults;
}
int64_t n = dashboard(User).get();       // outside any coroutine: run it and wait
```

Available: `exec/lean/countAsync` on queries, `findOne(...).execAsync/leanAsync`, `aggregate().execAsync`,
`createAsync`, `insertManyAsync`, `updateOne/ManyAsync`, `deleteOne/ManyAsync`, `countDocumentsAsync`,
`doc.saveAsync()`, typed `execAsync()`, and `asyncCall([&] { ... })` for any blocking code.

- **Any coroutine type** can `co_await` them: `drogon::Task<>`, Boost.Asio / cppcoro awaitables, or
  `nevla::Task<T>` (a small task type for code that has none). No adapter needed.
- `TenantScope` / `ActorScope` (see plugins) travel with the operation onto the pool thread.
- A `transaction()` does not span `co_await`s: a transaction belongs to one thread.
- Pool size: `setAsyncThreads(n)` before the first async call (default: `max(4, cores)`).

### Where does the coroutine continue? (event-loop servers)

By default, after a `co_await` the coroutine continues on the **nevla pool thread** that did the work. In an
event-loop server (Drogon, Asio) the rest of your handler must run on **its own IO loop**. Tell nevla how to get
back there, once, at startup:

```cpp
nevla::setResumeOn([]() -> nevla::Resumer {        // called on the thread that does the co_await
    auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    if (!loop) return {};                           // not a loop thread: continue on the pool (default)
    return [loop](std::function<void()> resume) { loop->queueInLoop(std::move(resume)); };
});
```

### Using nevla with Drogon

`<nevla/drogon.hpp>` (header-only, only include it in a Drogon app) does the above for you:

```cpp
#include <drogon/drogon.h>
#include <nevla/drogon.hpp>
#include <nevla/nevla.hpp>
using namespace drogon;

Task<HttpResponsePtr> createUser(HttpRequestPtr req) {
    try {
        auto body = nevla::Value::parse(std::string(req->body()));
        auto doc = co_await nevla::model("User").createAsync(body);   // I/O on the nevla pool ...
        auto r = HttpResponse::newHttpResponse();                     // ... and we are back on this IO loop
        r->setStatusCode(k201Created);
        r->setContentTypeCode(CT_APPLICATION_JSON);
        r->setBody(doc.toJSON().toJson());
        co_return r;
    } catch (const nevla::ValidationError &e) {
        auto r = HttpResponse::newHttpResponse();
        r->setStatusCode(k400BadRequest);
        r->setContentTypeCode(CT_APPLICATION_JSON);
        r->setBody(nevla::obj("errors", e.toJSON()).toJson());   // {"errors": {"name": "Path `name` is required."}}
        co_return r;
    }
}

int main() {
    nevla::useDrogonLoop();                                      // co_await returns to the same IO loop
    const char *uri = std::getenv("MONGODB_URI");
    nevla::connect(uri ? uri : "mongodb://127.0.0.1:27017", "iot");
    nevla::model("User", nevla::Schema({{"name", nevla::String().required()}}));
    app().registerHandler("/users", &createUser, {Post});
    app().addListener("0.0.0.0", 8080).setThreadNum(4).run();
}
```

`examples/08_drogon.cc` is a complete app; it is built when Drogon is installed, and `08_drogon --check`
(also a CTest test) fires 400 concurrent requests and verifies that every handler resumed on its own IO loop.
Without `useDrogonLoop()` the same check finds 400+ handlers on the wrong thread.

### GCC 13: no `{{...}}` inside a `co_await` expression

GCC 13 has an internal compiler error (*"in build_special_member_call"*) when a braced list such as
`{{"name", "x"}}` appears inside a `co_await` expression. Wrapping it in a function (`doc({...})`) does not
help; the braced list itself triggers it. Two fixes:

```cpp
// 1. obj() / arr(): the same Value, built without braces
auto u = co_await User.createAsync(nevla::obj("name", "Asha", "age", 21));
auto n = co_await User.find(nevla::obj("age", nevla::obj("$gte", 18))).countAsync();
auto t = co_await Post.find(nevla::obj("tags", nevla::obj("$in", nevla::arr("cpp", "iot")))).leanAsync();

// 2. a formula string, or a Value built on the line before
auto m = co_await User.find("age >= 18").countAsync();
Value filter{{"age", {{"$gte", 18}}}};
auto k = co_await User.find(filter).countAsync();
```

GCC 14+, Clang and MSVC accept braces everywhere.

---

## In-memory database (`memory://`)

```cpp
connect("memory://test");     // no MongoDB server: the database lives inside your process
```

The same nevla code runs on a built-in, MongoDB-compatible engine: CRUD, every query operator nevla uses,
updates (incl. positional `$`, `$push` with `$each/$sort/$slice`, `$addToSet`, upserts), unique indexes,
`$jsonSchema` validators, aggregation (`$match $group $sort $project $addFields $lookup $unwind $facet`
`$count $sample $replaceRoot $dateTrunc $densify` and the usual expression operators), populate, transactions (snapshot + rollback), plugins,
migrations, the batch writer and `co_await`.

- **Use it for unit tests and demos.** Your test suite needs no MongoDB, no Docker, no download.
  nevla's own suite runs on it: `NEVLA_TEST_URI=memory://t ./build/nevla_tests`.
- It is checked against real MongoDB: the test suite runs 930 random queries, updates and pipelines on both
  and compares the results.
- Not supported (a clear `nevla::Error`, never a wrong answer): change streams (`watch()`), collation,
  `$where`, `$text`, geo queries, `$lookup` with `let`, `$bucket`, `$setWindowFields`, and named time zones
  other than fixed offsets (`Asia/Kolkata` and `+05:30` work). `explain()`/`plan()` report `"MEMORY"`.
- One global lock per database: correct under any number of threads, but not a production database.
  Many concurrent readers that scan large collections slow writers down.
- Data is gone when the process exits. Every `connect("memory://name")` with the same name in one process
  shares the data.

---

## Typo protection: `strictQuery`

Mongoose silently ignores a filter path that is not in the schema, so `{emial: x}` matches nothing (or
everything) without a word. nevla can refuse it:

```cpp
Schema s({...}, {.strictQuery = true});      // this model
nevla::setStrictQuery(true);                 // or every model

User.find({{"emial", "a@x.in"}}).exec();
// nevla::Error: unknown path "emial" in a filter on model "User" (strictQuery). Did you mean "email"?
```

Operators, `_id`, the version key, timestamps, aliases, the discriminator key and paths inside `Mixed`
fields are always allowed.

---

## Slow query log

```cpp
nevla::setSlowQueryThreshold(100, [](const nevla::SlowQuery &q) {
    std::cerr << q.collection << "." << q.op << " took " << q.ms << " ms " << q.args.toJson()
              << " -> " << q.suggestion << "\n";
});
// readings.find took 412 ms {"device":"d1","at":{"$gte":...}} -> consider an index {device: 1, at: 1}
nevla::setSlowQueryThreshold(0);    // off
```

Without a callback it prints to stderr. The suggestion follows the ESR rule (equality fields, then sort,
then range) and is only a hint: check it with `aggregate().plan()` / `explain()`.

---

## Time-series collections

For sensor readings, metrics and logs (MongoDB 5.0+). Declare it in the schema; the collection is created as a
time-series collection on first use:

```cpp
Schema readingSchema({
    {"device", String().required()},
    {"value",  Number().required()},
    {"at",     DateTime().required()},
}, {.timeseries = {.timeField = "at", .metaField = "device", .granularity = "seconds",
                   .expireAfterSeconds = 90 * 86400}});          // keep 90 days
auto Reading = model("Reading", readingSchema);

Reading.create({{"device", "d1"}, {"value", 24.5}, {"at", Date::now()}});
Reading.aggregate().match("device == 'd1'").bucketByTime("at", "15m", {avg("value")}).exec();
Reading.collectionInfo()["type"];                                 // "timeseries"
```

`model()` checks the options (time field must be a `DateTime`, granularity must be valid, ...).
If the collection already exists as a normal collection, nevla does not convert it.

---

## Batch writer

Thousands of small inserts per second (MQTT telemetry, logs) are slow one by one. The batch writer queues
them and writes them with `insertMany` in the background:

```cpp
auto writer = Reading.batchWriter({.maxDocs = 500, .maxDelayMs = 200});
writer.onError([](const std::string &msg, int64_t lost) { std::cerr << lost << " lost: " << msg << "\n"; });

writer.add({{"device", id}, {"value", 30.5}, {"at", Date::now()}});  // validated NOW (throws ValidationError)
...
writer.flush();                       // wait until everything queued is written
writer.stats();                       // {queued, written, failed, batches}
```

- A batch is written when `maxDocs` are queued or the oldest one has waited `maxDelayMs`.
- `add()` blocks when `maxQueue` documents are waiting (backpressure, no unbounded memory).
- `ordered = false` (default): one bad document (e.g. duplicate key) does not stop the rest.
- Casting, defaults and validation run in `add()`, in your thread. Copies share one writer; the last copy
  flushes and stops it.

---

## REST query parameters: `fromQuery`

Turn `GET /users?age[gte]=18&city[in]=Kolkata,Durgapur&sort=-createdAt&page=2&limit=20&fields=name,email`
into a safe, schema-cast query with pagination:

```cpp
auto params = nevla::parseQueryString(req->query());     // URL-decoded map
Page page = User.fromQuery(params, {
    .allow       = {"age", "role", "city", "createdAt"},   // filterable (and sortable / selectable)
    .defaultSort = "-createdAt",
});
return page.toJSON();     // {docs, total, page, perPage, pages, hasNext, hasPrev}
```

| Parameter | Means |
|---|---|
| `field=v` | equality (cast with the schema: `"18"` → `18`, `"2026-11-01"` → `Date`) |
| `field[eq\|ne\|gt\|gte\|lt\|lte]=v` | comparison |
| `field[in\|nin]=a,b,c` | in a list |
| `field[exists]=true\|false` | field present |
| `field[contains]=txt` / `field[startsWith]=txt` | case-insensitive text match (regex characters escaped) |
| `field[regex]=...` | only with `.allowRegex = true` (a user regex can be very slow) |
| `sort=-age,name` `page=2` `limit=20` `fields=name,email` | sort, page, page size (≤ `maxLimit`), projection |

Anything not allowed (unknown field or operator, sorting / selecting a field that is not allowed, a value that
cannot be cast, `page=0`) throws `QueryParamError` with `.param`, so you can answer 400. A `limit` above
`maxLimit` is lowered to `maxLimit`. Hidden fields (`select(false)`) stay hidden.

---

## Schema export and the MongoDB validator

One schema, many consumers:

```cpp
userSchema.toJsonSchema();        // JSON Schema as a Value (the same one syncValidator() stores)
userSchema.toOpenAPI();           // OpenAPI 3 component schema (for Swagger)
userSchema.toTypeScript("User");  // "export interface User { _id: string; name: string; age?: number; ... }"

User.syncValidator();             // store the schema as MongoDB's own $jsonSchema validator
nevla::syncValidators();          // every model (e.g. in a "db:sync" deploy step)
```

With the validator stored, even other programs (mongosh, a Python script) cannot insert a document that
breaks the types / required / enum / min / max rules. `syncValidator("moderate")` leaves existing invalid
documents updatable; `action = "warn"` only logs. Time-series models and discriminator children are skipped
by `syncValidators()` (a child shares its base model's collection; call it on the base).

---

## Built-in plugins: soft delete, history, multi-tenant

```cpp
schema.plugin(nevla::plugins::softDelete);     // {{"field", "deletedAt"}}
schema.plugin(nevla::plugins::history);        // {{"collection", "nevla_history"}}
schema.plugin(nevla::plugins::multiTenant);    // {{"field", "tenantId"}}
```

**Soft delete.** `doc.call("softDelete")`, `doc.call("restore")`, `doc.call("isDeleted")`,
`User.call("softDelete", filter)`. Every `find`, `count`, `update`, `aggregate` hides deleted documents;
`{{"$withDeleted", true}}` in a filter shows them.

**History (audit log).** Every create / save / delete, and every `updateOne/Many` through the model, writes an
entry `{model, docId, op, changes, by, at}` to `nevla_history`. `changes` holds the new value of each modified
path (`null` = removed); `updateOne/Many` entries keep the filter and update instead. Encrypted fields are
masked.

```cpp
nevla::ActorScope who("asha@x.in");            // who is doing it (this request / thread)
User.call("history", id);                      // [{op: "update", changes: {age: 22}, by: "asha@x.in", at: ...}, ...]
```

**Multi-tenant.** Every query, insert, update and aggregate of the thread is scoped to the tenant:

```cpp
nevla::TenantScope t(req->getHeader("X-Tenant"));   // e.g. in an auth middleware
User.find().exec();                                 // only this tenant's users; inserts get tenantId
{ nevla::AllTenantsScope admin; User.countDocuments(); }   // maintenance code: everything
```

No tenant set → `nevla::Error`. A forgotten scope never turns into a silent cross-tenant query.
The tenant field is required, indexed and immutable.

---

## Field encryption

```cpp
nevla::setEncryptionKey(std::getenv("NEVLA_KEY"));   // 64 hex characters (32 bytes), or a passphrase

Schema s({
    {"aadhaar", String().encrypted()},       // random IV: strongest, cannot be searched
    {"token",   String().encrypted(true)},   // deterministic: equality filters still work
});
User.create({{"aadhaar", "1234 5678 9012"}, {"token", "tok-42"}});
User.findOne({{"token", "tok-42"}});         // works (deterministic)
```

- AES-256-GCM (OpenSSL); stored as the string `"nvenc:1:<base64>"`. Reads decrypt automatically.
- Deterministic fields support equality and `$in` only; a range filter on an encrypted field throws.
- Without the key, reading an encrypted field throws: nothing is ever returned as garbage.
- Keep the key outside the database (environment, secret manager). Lose it and the data is gone.

---

## Migrations

```cpp
nevla::migration("2026-11-20-add-plan",
    [] { User.updateMany(Value::object(), {{"plan", "free"}}); },          // up
    [] { User.updateMany(Value::object(), {{"$unset", {{"plan", ""}}}}); });  // down (optional)

nevla::migrate();            // runs the ones not run yet, in id order -> {"2026-11-20-add-plan"}
nevla::rollback();           // undoes the last one
nevla::migrationStatus();    // [{id, applied, at, ms}, ...]
```

- Applied ids are recorded in `nevla_migrations`; running `migrate()` again does nothing.
- A lock document stops two servers that start at the same time from running a migration twice.
- `{.transaction = true}` runs each one in a transaction (replica set).
- Ids sort as text: start them with a date.

---

## findById cache

```cpp
User.cache({.ttlMs = 30000, .maxEntries = 10000});
User.findById(id).exec();      // served from memory after the first read
User.cacheStats();             // {hits, misses, size}
```

Every write through nevla (save, update, delete, bulkWrite) clears the cached entries of that model. The
TTL is for other processes that may change the data. The cache is skipped inside transactions and for
models with `findOne` middleware (softDelete / multiTenant / your own hooks), because those must always run.

---

## Errors

All errors derive from `nevla::Error` (a `std::runtime_error`).

| Error | When |
|---|---|
| `ValidationError` | schema validation failed; `.errors` (path → message) and `.kinds` (path → kind) |
| `CastError` | a filter / update value cannot be cast; `.path` |
| `DuplicateKeyError` | unique index or duplicate `_id` (MongoDB E11000) |
| `NotFoundError` | `orFail()` found nothing; `save()` on a document that no longer exists |
| `VersionError` | `optimisticConcurrency` conflict on `save()` |
| `FormulaError` | a formula string has a mistake; `.column` (1-based) |
| `QueryParamError` | `fromQuery()` got a parameter that is not allowed or cannot be cast; `.param` |
| `Error` | everything else: not connected, bad JSON, server errors |

```cpp
try {
    User.create(body);
} catch (const ValidationError &e) {      // 400, body: e.toJSON() -> {"email": "Path `email` is required."}
} catch (const QueryParamError &e) {      // 400
} catch (const CastError &e) {            // 400
} catch (const DuplicateKeyError &e) {    // 409
} catch (const NotFoundError &e) {        // 404
} catch (const VersionError &e) {         // 409
} catch (const Error &e) {                // 500
}
```

---

## Threads, async and debugging

- `Model` objects and the connection are **thread-safe**; use them from any number of threads.
- A single `Document` object must not be modified by two threads at the same time.
- The normal calls are blocking. In coroutines use the `...Async()` forms and `co_await` them
  (see [co_await and event loops](#co_await-and-event-loops)); otherwise offload with `nevla::async`:

  ```cpp
  auto fut = nevla::async([&] { return User.find({{"active", true}}).lean(); });
  auto users = fut.get();
  ```

  In event-loop servers (Drogon, Boost.Asio, ...) never call the blocking forms on the loop thread.
- Slow queries: `setSlowQueryThreshold(ms, fn)` (see [Slow query log](#slow-query-log)).

- Debug logging, like `mongoose.set('debug', true)`:

  ```cpp
  nevla::setDebug(true);     // prints "nevla: users.find({...})" to stderr
  nevla::setDebug([](const std::string &coll, const std::string &op, const Value &args) { /* your logger */ });
  nevla::setDebug(false);
  ```

---

## Schema options

```cpp
Schema s({...}, {
    .timestamps            = false,        // createdAt / updatedAt
    .collection            = "",           // default: pluralized model name
    .strict                = true,         // drop unknown paths in documents AND updates
    .versionKey            = "__v",        // "" disables
    .discriminatorKey      = "__t",
    .autoIndex             = true,         // create indexes before the first operation
    .virtuals              = false,        // toJSON(): virtuals, aliases, "id"
    .createdAt             = "createdAt",  // timestamp field names
    .updatedAt             = "updatedAt",
    .optimisticConcurrency = false,        // VersionError on conflicting save()
    .getters               = false,        // toJSON() applies field getters
    .strictQuery           = false,        // unknown filter path -> Error ("Did you mean ...?")
    .timeseries            = {},           // {.timeField, .metaField, .granularity, .expireAfterSeconds}
});
```

C++ designated initializers must keep this order; write only the ones you need:
`{.timestamps = true, .virtuals = true}`.

---

## Mongoose → nevla cheat sheet

| Mongoose (JavaScript) | nevla (C++) |
|---|---|
| `mongoose.connect(uri)` | `connect(uri)` |
| `new Schema({ name: { type: String, required: true } })` | `Schema({{"name", String().required()}})` |
| `{ type: [String] }` | `Array(String())` |
| `{ type: Map, of: Number }` | `Map(Number())` |
| `mongoose.model('User', schema)` | `model("User", schema)` |
| `new User({...})` | `User({...})` |
| `await User.create({...})` | `User.create({...})` |
| `await User.find({...}).sort('-age').limit(5)` | `User.find({...}).sort("-age").limit(5).exec()` |
| `.lean()` | `.lean()` |
| `User.findById(id).orFail()` | `User.findById(id).orFail()` |
| `doc.name = 'x'; await doc.save()` | `doc.set("name", "x").save()` |
| `doc.name` | `doc["name"]` / `doc.get("name")` |
| `User.updateOne(f, u, { upsert: true })` | `User.updateOne(f, u, {.upsert = true})` |
| `User.findOneAndUpdate(f, u, { new: true })` | `User.findOneAndUpdate(f, u, {.returnNew = true})` |
| `.populate({ path, select, match, options: { sort, limit }, populate })` | `PopulateOptions{...}` |
| `schema.pre('save', function () {...})` | `schema.pre("save", [](Document &d) {...})` |
| `schema.post('find', function (docs) {...})` | `schema.post("find", [](QueryInfo &, Value &docs) {...})` |
| `schema.virtual('x').get(fn)` | `schema.virtual_("x", fn)` |
| `schema.virtual('posts', { ref, localField, foreignField })` | `schema.virtualPopulate("posts", {...})` |
| `schema.methods.x = fn` / `schema.statics.x = fn` | `schema.method("x", fn)` / `schema.static_("x", fn)` |
| `schema.plugin(fn, opts)` | `schema.plugin(fn, opts)` |
| `Model.discriminator('Sensor', schema)` | `Model.discriminator("Sensor", schema)` |
| `Model.aggregate().match({...}).group({...})` | `Model.aggregate().match({...}).group("field", {avg("x")})` |
| `{ $expr: { $gt: ['$used', '$limit'] } }` | `"used > limit"` (formula string) |
| `$group` by `$dateTrunc` + `$densify` | `bucketByTime("at", "15m", {...}, {.fillGaps = true})` |
| `Model.bulkWrite([...])` | `Model.bulkWrite({...})` |
| `Model.watch()` | `Model.watch()` |
| `Query.cursor()` | `Query.cursor()` |
| `mongoose.startSession()` + `withTransaction` | `transaction([&] {...})` |
| `mongoose.set('debug', true)` | `setDebug(true)` |
| `mongoose.set('strictQuery', true)` (silently drops the path) | `setStrictQuery(true)` (throws, suggests the right name) |
| `await User.find(...)` in an `async` function | `co_await User.find(...).execAsync()` in a coroutine |
| `mongodb-memory-server` (downloads a mongod) | `connect("memory://test")` |
| `mongoose-delete`, `mongoose-history`, tenant plugins | `plugins::softDelete`, `plugins::history`, `plugins::multiTenant` |
| `mongoose-encryption` / CSFLE setup | `String().encrypted()` + `setEncryptionKey(...)` |
| `migrate-mongo` | `migration(id, up, down)` + `migrate()` |
| `mongoose-to-swagger`, `ts-mongoose` | `schema.toOpenAPI()`, `schema.toTypeScript("User")` |
| hand-written `req.query` parsing | `Model.fromQuery(params, {.allow = {...}})` |

---

## Differences from Mongoose

C++ syntax:

- No property access (`doc.name`): use `doc["name"]` / `doc.get("name")` and `doc.set("name", v)`.
- No `await`: calls are blocking; in a coroutine use `co_await ...Async()`.
- `virtual_`, `static_`, `or_`, `and_` because of C++ keywords.

Behaviour:

- Sub-documents do not get their own `_id`.
- Query `countDocuments()` ignores `skip`/`limit`.
- One connection (one database) at a time; use the database's `$lookup`/`aggregate` for cross-db work.
- Not implemented yet: `Decimal128`, `Buffer`/binary, `BigInt` types; `populate` across databases;
  `Model.translateAliases()`.

---

## Project layout, tests and examples

```
nevla/
├── include/nevla/
│   ├── nevla.hpp        # include this
│   ├── value.hpp        # Value, Date, ObjectId
│   ├── schema.hpp       # Schema, Field, errors
│   ├── model.hpp        # connect, Model, Document, Query, Cursor, ChangeStream, Aggregate
│   ├── formula.hpp      # expr(), formulaFilter(), FormulaError
│   ├── typed.hpp        # NEVLA_STRUCT
│   ├── typed_model.hpp  # TypedModel<T>, schemaOf<T>()
│   ├── async.hpp        # Async<T>, Task<T>, setResumeOn()
│   ├── plugins.hpp      # softDelete, history, multiTenant, TenantScope, setEncryptionKey
│   └── drogon.hpp       # useDrogonLoop() (optional, header-only)
├── src/                 # value, schema, model, formula, async, plugins, crypto, memory (.cc)
├── examples/
│   ├── 01_quickstart.cc     # CRUD
│   ├── 02_blog.cc           # populate, hooks, virtuals, methods
│   ├── 03_iot.cc            # custom _id, discriminators, TTL, aggregate, transaction
│   ├── 04_typed_struct.cc   # NEVLA_STRUCT
│   ├── 05_features.cc       # statics, plugins, Map, getters, virtual populate, paginate, cursor, bulkWrite
│   ├── 06_iot_dashboard.cc  # end-to-end: 8 writers + 5 live dashboards, every number verified
│   ├── 07_offline_api.cc    # v0.5 tour on memory:// (typed model, plugins, encryption, migrations, ...)
│   └── 08_drogon.cc         # Drogon app with co_await (built when Drogon is installed)
├── tests/
│   ├── test.cc          # 1,658 checks on MongoDB / 708 on memory://
│   └── compile_fail/    # typed-model mistakes that must NOT compile
├── docs/README.hinglish.md
├── .github/workflows/ci.yml
├── CHANGELOG.md  CONTRIBUTING.md  LICENSE  CMakeLists.txt
```

```bash
./build/nevla_tests                                         # "1658 passed, 0 failed"
NEVLA_TEST_URI=memory://t ./build/nevla_tests               # "708 passed, 0 failed" (no server)
NEVLA_TEST_URI="mongodb://host:27017/?replicaSet=rs0" ./build/nevla_tests
cd build && ctest       # both suites, 4 compile-fail checks, the Drogon resume check (7 tests)
./build/07_offline_api                # runs anywhere
./build/08_drogon --check             # 400 concurrent HTTP requests, every co_await back on its IO loop
./build/05_features
./build/06_iot_dashboard 12500        # 100,000 readings from 8 threads, checks every aggregate
```

Tested on Ubuntu 24.04, g++ 13, mongocxx 3.10.2, MongoDB 8 (single-node replica set), Drogon 1.9, also
under AddressSanitizer + UndefinedBehaviorSanitizer and ThreadSanitizer. The MongoDB suite includes a
differential test: 930 random queries / updates / pipelines run on MongoDB and on `memory://`, and the results
must be identical.

---

## Contributing, changelog, license

- Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).
- Release notes: [CHANGELOG.md](CHANGELOG.md).
- Not affiliated with MongoDB, Inc. or the Mongoose project.
- License: [MIT](LICENSE).
