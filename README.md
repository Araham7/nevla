# nevla

**A Mongoose-style ODM for MongoDB in modern C++ (C++20).**

Schemas, validation, casting, defaults, middleware (hooks), populate, virtuals, discriminators, transactions,
change streams and a chainable query builder — the API you know from Node.js
[Mongoose](https://mongoosejs.com), on top of the official MongoDB C++ driver (`mongocxx`).

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
15. [Aggregation builder](#aggregation-builder) · [Distinct and counting](#distinct-and-counting)
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
27. [Errors](#errors)
28. [Threads, async and debugging](#threads-async-and-debugging)
29. [Schema options](#schema-options)
30. [Mongoose → nevla cheat sheet](#mongoose--nevla-cheat-sheet)
31. [Differences from Mongoose](#differences-from-mongoose)
32. [Project layout, tests and examples](#project-layout-tests-and-examples)
33. [Contributing, changelog, license](#contributing-changelog-license)

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
| Middleware | `pre/post` hooks for `save`, `validate`, `find`, `updateOne`, `aggregate`, ... |
| Real-world features | transactions, change streams, bulk writes, cursors, pagination, optimistic concurrency |
| C++ comfort | `NEVLA_STRUCT` maps your structs to documents at compile time |

It is **thread-safe**, uses a **connection pool**, and is tested against a real MongoDB server
(including AddressSanitizer / UBSan runs).

---

## Requirements and installation

- A C++20 compiler: **g++ 11+** or **clang 14+**
- **CMake 3.16+**
- **mongocxx 3.8+** (the MongoDB C++ driver; tested with 3.10.2)
- A MongoDB server (4.4+; transactions and change streams need a replica set or Atlas)

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

# optional: run the tests (needs MongoDB on 127.0.0.1:27017)
./build/nevla_tests
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
FetchContent_Declare(nevla GIT_REPOSITORY https://github.com/<your-username>/nevla.git GIT_TAG v0.3.0)
FetchContent_MakeAvailable(nevla)
target_link_libraries(myapp PRIVATE nevla::nevla)
```

**C. After `cmake --install`**

```cmake
find_package(nevla 0.3 CONFIG REQUIRED)
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
| `match(filter)` | `$match` (cast, see above) |
| `group(by, {accumulators})` | `$group` |
| `sort("-a b")` / `sort({{"a", -1}})` | `$sort` |
| `limit(n)`, `skip(n)`, `sample(n)` | `$limit`, `$skip`, `$sample` |
| `project("a b -_id")` / `project({{"total", expr}})` | `$project` |
| `addFields({{"total", expr}})` | `$addFields` |
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
std::cout << agg.toJson();                           // the pipeline that will be sent (paste into mongosh)
Value stages = agg.pipeline();
```

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

## Errors

All errors derive from `nevla::Error` (a `std::runtime_error`).

| Error | When |
|---|---|
| `ValidationError` | schema validation failed; `.errors` (path → message) and `.kinds` (path → kind) |
| `CastError` | a filter / update value cannot be cast; `.path` |
| `DuplicateKeyError` | unique index or duplicate `_id` (MongoDB E11000) |
| `NotFoundError` | `orFail()` found nothing; `save()` on a document that no longer exists |
| `VersionError` | `optimisticConcurrency` conflict on `save()` |
| `Error` | everything else: not connected, bad JSON, server errors |

```cpp
try {
    User.create(body);
} catch (const ValidationError &e) {      // 400
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
- All calls are blocking. Offload them with `nevla::async`:

  ```cpp
  auto fut = nevla::async([&] { return User.find({{"active", true}}).lean(); });
  auto users = fut.get();
  ```

  In event-loop servers (Drogon, Boost.Asio, ...) run database calls on a worker pool, not on the loop thread.

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
| `Model.bulkWrite([...])` | `Model.bulkWrite({...})` |
| `Model.watch()` | `Model.watch()` |
| `Query.cursor()` | `Query.cursor()` |
| `mongoose.startSession()` + `withTransaction` | `transaction([&] {...})` |
| `mongoose.set('debug', true)` | `setDebug(true)` |

---

## Differences from Mongoose

C++ syntax:

- No property access (`doc.name`): use `doc["name"]` / `doc.get("name")` and `doc.set("name", v)`.
- No `await`: every call is blocking; use `nevla::async` or your own thread pool.
- `virtual_`, `static_`, `or_`, `and_` because of C++ keywords.

Behaviour:

- Sub-documents do not get their own `_id`.
- Query `countDocuments()` ignores `skip`/`limit`.
- One connection (one database) at a time; use the database's `$lookup`/`aggregate` for cross-db work.
- Not implemented yet: `Decimal128`, `Buffer`/binary, `BigInt` types; `populate` across databases;
  callback / coroutine async API; `Query.explain()`; `Model.translateAliases()`.

---

## Project layout, tests and examples

```
nevla/
├── include/nevla/
│   ├── nevla.hpp        # include this
│   ├── value.hpp        # Value, Date, ObjectId
│   ├── schema.hpp       # Schema, Field, errors
│   ├── model.hpp        # connect, Model, Document, Query, Cursor, ChangeStream
│   └── typed.hpp        # NEVLA_STRUCT
├── src/                 # value.cc, schema.cc, model.cc
├── examples/
│   ├── 01_quickstart.cc     # CRUD
│   ├── 02_blog.cc           # populate, hooks, virtuals, methods
│   ├── 03_iot.cc            # custom _id, discriminators, TTL, aggregate, transaction
│   ├── 04_typed_struct.cc   # NEVLA_STRUCT
│   ├── 05_features.cc       # statics, plugins, Map, getters, virtual populate, paginate, cursor, bulkWrite
│   └── 06_iot_dashboard.cc  # end-to-end: 8 writer threads + live dashboards, every number verified
├── tests/test.cc        # 377 checks against a real MongoDB
├── docs/README.hinglish.md
├── .github/workflows/ci.yml
├── CHANGELOG.md  CONTRIBUTING.md  LICENSE  CMakeLists.txt
```

```bash
./build/nevla_tests                                         # "377 passed, 0 failed"
NEVLA_TEST_URI="mongodb://host:27017/?replicaSet=rs0" ./build/nevla_tests
cd build && ctest
./build/05_features
./build/06_iot_dashboard 12500        # 100,000 readings from 8 threads, checks every aggregate
```

Tested on Ubuntu 24.04, g++ 13, mongocxx 3.10.2, MongoDB 8 (single-node replica set), also under
AddressSanitizer + UndefinedBehaviorSanitizer.

---

## Contributing, changelog, license

- Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).
- Release notes: [CHANGELOG.md](CHANGELOG.md).
- Not affiliated with MongoDB, Inc. or the Mongoose project.
- License: [MIT](LICENSE).
