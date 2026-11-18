# nevla — C++ ke liye Mongoose jaisa MongoDB ODM (Hinglish guide)

> English documentation: [../README.md](../README.md)

> **nevla** (नेवला) = Hindi me "mongoose". Naam bas placeholder hai, jo chaho rakh lo (neeche "Naam badalna" dekho).

Node.js me jo kaam **Mongoose** karta hai (schema, validation, defaults, hooks, populate…), wahi kaam C++20 me.
Neeche official `mongocxx` driver chalta hai; nevla uske upar ek aasaan layer hai.

```cpp
#include <nevla/nevla.hpp>
using namespace nevla;

int main() {
    connect("mongodb://127.0.0.1:27017", "mydb");

    Schema userSchema({
        {"name",  String().required().trim()},
        {"email", String().required().lowercase().unique()},
        {"age",   Int().min(0)},
        {"role",  String().enumOf({"user", "admin"}).def("user")},
    }, {.timestamps = true});

    auto User = model("User", userSchema);

    auto u = User.create({{"name", "Araham"}, {"email", "A@X.in"}, {"age", 24}});
    auto adults = User.find().where("age").gte(18).sort("-age").limit(10).exec();
    u.set("age", 25).save();
}
```

---

## Index

1. [Ye kya hai, aur kyun](#1-ye-kya-hai-aur-kyun)
2. [Install + build](#2-install--build)
3. [Apne project me jodna](#3-apne-project-me-jodna)
4. [Connect](#4-connect)
5. [Value — JSON jaisa data](#5-value--json-jaisa-data)
6. [Schema + field types](#6-schema--field-types)
7. [Validation](#7-validation)
8. [Defaults, setters, cast](#8-defaults-setters-cast)
9. [Model + Document banana](#9-model--document-banana)
10. [Padhna: find / findOne / findById](#10-padhna-find--findone--findbyid)
11. [Query builder](#11-query-builder)
12. [Document badalna + save()](#12-document-badalna--save)
13. [Seedha update: updateOne / updateMany / findOneAndUpdate](#13-seedha-update)
14. [Delete](#14-delete)
15. [count / exists / distinct / aggregate](#15-count--exists--distinct--aggregate)
16. [Nested object + sub-document array](#16-nested-object--sub-document-array)
17. [populate (ref)](#17-populate-ref)
18. [Hooks (middleware)](#18-hooks-middleware)
19. [Virtuals, methods, toJSON](#19-virtuals-methods-tojson)
20. [Indexes (unique, compound, TTL)](#20-indexes)
21. [Custom `_id` (string / UUID)](#21-custom-_id)
22. [Discriminators (ek collection, kai type)](#22-discriminators)
23. [Transactions](#23-transactions)
24. [Typed struct: `NEVLA_STRUCT`](#24-typed-struct-nevla_struct)
25. [Errors](#25-errors)
26. [Threads + async](#26-threads--async)
27. [Schema options](#27-schema-options)
28. [Mongoose ↔ nevla cheat-sheet](#28-mongoose--nevla-cheat-sheet)
29. [Mongoose se kya alag hai / kya nahi hai](#29-mongoose-se-kya-alag-hai)
30. [Tests, examples, folder](#30-tests-examples-folder)
31. [Open-source karne ke steps](#31-open-source-karne-ke-steps)
32. [v0.2 me kya naya hai](#32-v02-me-kya-naya-hai-short-me)
33. [Aggregation builder (v0.3)](#33-aggregation-builder-v03--aasaan-pipeline)
34. [v0.4: formula, time buckets, reports, pipeline populate](#34-v04-formula-time-buckets-ek-line-reports-pipeline-me-populate-debugging)
35. [v0.5: typed model, co_await + Drogon, memory://, plugins, encryption, migrations ...](#35-v05-wo-cheezein-jo-mongoose-me-nahi-hain)

---

## 1. Ye kya hai, aur kyun

C++ ke jo open-source MongoDB ODM pehle se hain, wo sab **"struct mapper"** hain: apna struct BSON me badlo aur wapas.
Mongoose wali cheezein (validation, defaults, hooks, populate) unme nahi hain.

| Library | Tarika | Haalat | Schema validation | Hooks | Populate |
|---|---|---|---|---|---|
| Mangrove (MongoDB ka apna) | CRTP model + macro | archived (band) | ✗ | ✗ | ✗ |
| MongoXX | Mapper `add_field` | chhota project | ✗ | ✗ | ✗ |
| quarkpunk/mongoose | struct serializer | chhota project | ✗ | ✗ | ✗ |
| Congo | chainable query | chhota project | ✗ | ✗ | ✗ |
| ThorsMongo | struct serializer | active, **GPL** | ✗ | ✗ | ✗ |
| **nevla** | Mongoose-style runtime schema **+** struct mapping | ye | ✓ | ✓ | ✓ |

nevla me dono raste hain:

- **Dynamic** (Mongoose jaisa): `doc["name"]`, `doc.set("age", 5)`
- **Typed**: `NEVLA_STRUCT` se apna C++ struct seedha (section 24)

Ye Mongoose ka **exact copy nahi** hai (C++ me JavaScript jaisa `doc.name = "x"` ho hi nahi sakta), par soch aur naam wahi hain. Farq section 29 me likhe hain.

---

## 2. Install + build

Chahiye: **g++ 11+ ya clang 14+ (C++20)**, **CMake 3.16+**, **mongocxx 3.8+**.

### Ubuntu 22.04 / 24.04

```bash
sudo apt update
sudo apt install -y build-essential cmake curl libmongoc-dev libbson-dev libssl-dev

# mongocxx (C++ driver) source se: apt me nahi milta
curl -OL https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.10.2/mongo-cxx-driver-r3.10.2.tar.gz
tar xzf mongo-cxx-driver-r3.10.2.tar.gz
cd mongo-cxx-driver-r3.10.2/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=/usr/local
make -j$(nproc)
sudo make install
sudo ldconfig
cd ../..
```

### nevla build

```bash
cd nevla
mkdir build && cd build
cmake ..
make -j$(nproc)

./nevla_tests          # MongoDB 127.0.0.1:27017 par chalna chahiye
./01_quickstart
sudo make install      # optional: /usr/local me daal do
```

Tests kisi aur server par chalane ho to: `NEVLA_TEST_URI="mongodb://host:27017" ./nevla_tests`

---

## 3. Apne project me jodna

**Tarika A — folder copy karke (sabse aasaan):**

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp CXX)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(third_party/nevla)          # nevla ka folder yahan rakho
add_executable(myapp main.cc)
target_link_libraries(myapp PRIVATE nevla::nevla)
```

**Tarika B — `sudo make install` ke baad:**

```cmake
find_package(nevla REQUIRED)
target_link_libraries(myapp PRIVATE nevla::nevla)
```

**Tarika C — GitHub par daalne ke baad (FetchContent):**

```cmake
include(FetchContent)
FetchContent_Declare(nevla GIT_REPOSITORY https://github.com/<tum>/nevla.git GIT_TAG v0.5.0)
FetchContent_MakeAvailable(nevla)
target_link_libraries(myapp PRIVATE nevla::nevla)
```

Code me bas ek include:

```cpp
#include <nevla/nevla.hpp>
using namespace nevla;
```

> `-Wextra` ke saath `{.timestamps = true}` par g++ "missing initializer" warning deta hai. Wo galti nahi hai; `-Wno-missing-field-initializers` laga do.

---

## 4. Connect

```cpp
connect("mongodb://127.0.0.1:27017", "mydb");            // URI + database
connect("mongodb://127.0.0.1:27017/mydb");               // database URI me
connect("mongodb+srv://user:pass@cluster.mongodb.net/mydb?retryWrites=true");   // Atlas

if (connected()) { /* ... */ }
disconnect();
```

- `connect()` server ko ping karta hai; server band ho to wahin `nevla::Error` aata hai.
- Andar connection **pool** hai, isliye kai threads se ek saath use kar sakte ho.
- Database na URI me ho na argument me, to `"test"` use hota hai.
- Models `connect()` se pehle bhi bana sakte ho; DB ka kaam `connect()` ke baad hi chalega.

---

## 5. Value — JSON jaisa data

`Value` nevla ka JSON/BSON type hai: null, bool, int, double, string, `Date`, `ObjectId`, array, object.

```cpp
Value v = {
    {"name", "Araham"},
    {"age", 24},
    {"ok", true},
    {"tags", Value::array({"iot", "cpp"})},
    {"address", {{"city", "Durgapur"}, {"pin", 713402}}},
    {"joined", Date::now()},
};

v["name"].asString();            // "Araham"
v["age"].asInt();                // 24
v["address"]["city"].asString(); // "Durgapur"
v["tags"].at(0).asString();      // "iot"
v["tags"].size();                // 2
v.contains("age");               // true

v["city"] = "Burdwan";           // naya key
v.erase("ok");

// dotted path
const Value *pin = v.path("address.pin");   // na ho to nullptr
v.pathRef("a.b.c") = 5;                     // raste ke objects khud ban jaate hain
v.erasePath("a.b.c");

// loop
for (const auto &tag : v["tags"].elements()) std::cout << tag.asString();
for (const auto &[key, val] : v.items())     std::cout << key << " = " << val.toJson();

// JSON
std::string text = v.toJson();        // ek line
std::string nice = v.toJson(2);       // indent ke saath
Value back = Value::parse(R"({"a":[1,2,3]})");

// C++ types
int age                       = v["age"].as<int>();
std::vector<std::string> tags = v["tags"].as<std::vector<std::string>>();
Value fromVector              = std::vector<int>{1, 2, 3};
```

**Array ya object?** `{...}` ke andar har item `{"string", kuch}` ho to object banta hai, warna array.
Pairs ka array hi chahiye, ya khaali/ek-item array, to saaf likho:

```cpp
Value a = Value::array({"x", 1});   // ["x", 1]  (hamesha array)
Value e = Value::array();           // []
Value o = Value::object();          // {}
```

**Getters lenient hain:** `asInt()`, `asDouble()`, `asBool()` type alag ho to convert karne ki koshish karte hain, na ho to fallback (`asInt(-1)`). `asString()` string na ho to `""`. Kisi bhi scalar ka text chahiye to `str()`.

**Date + ObjectId:**

```cpp
Date now = Date::now();                               // UTC, milliseconds
auto d = Date::parse("2026-11-17T10:30:00.000Z");     // std::optional<Date>
if (d) std::cout << d->iso() << " " << d->ms;

ObjectId id = ObjectId::generate();
id.str();                                             // 24 hex characters
ObjectId::isValid("6afb9f7045827a9e0d061d21");        // true
```

---

## 6. Schema + field types

```cpp
Schema s({
    {"name",    String()},
    {"age",     Int()},                     // sirf poora number
    {"price",   Number()},                  // int ya double
    {"active",  Boolean()},
    {"born",    DateTime()},
    {"owner",   Id()},                      // ObjectId
    {"extra",   Mixed()},                   // kuch bhi: na cast, na validation
    {"tags",    Array(String())},           // string ki list
    {"scores",  Array(Number())},
    {"address", Object(Schema({             // nested object
        {"city", String()},
        {"pin",  Int()},
    }))},
    {"items",   Array(Schema({              // sub-documents ki list
        {"sku", String().required()},
        {"qty", Int().def(1)},
    }))},
});

s.add("nickname", String());                // baad me field jodna
s.add(otherSchema);                         // dusre schema ke saare fields jodna
```

**Field ke saare options** (sab chain hote hain):

| Option | Kaam |
|---|---|
| `.required()` / `.required(true, "message")` | hona zaroori |
| `.def(value)` / `.def([] { return Value(...); })` | default value / default function |
| `.enumOf({"a", "b"})` | sirf inme se ek |
| `.min(n)` `.max(n)` | number ki had |
| `.minLength(n)` `.maxLength(n)` | string ki lambai |
| `.match("regex", "message")` | string pattern |
| `.validate(fn, "message")` | apna validator |
| `.trim()` `.lowercase()` `.uppercase()` | string saaf karna |
| `.set(fn)` | setter: save se pehle value badlo |
| `.unique()` | unique index |
| `.index()` | normal index |
| `.select(false)` | find me by default nahi aata |
| `.immutable()` | ek baar save ke baad badalta nahi |
| `.ref("Model")` | `populate()` ke liye |

---

## 7. Validation

`save()`, `create()`, `insertMany()` se pehle apne aap chalta hai. Khud chalana ho to `doc.validate()`.

```cpp
Schema s({
    {"name",  String().required().minLength(2).maxLength(30)},
    {"email", String().required().match("^[^@]+@[^@]+\\.[a-z]+$", "email galat hai")},
    {"age",   Int().min(0).max(150)},
    {"role",  String().enumOf({"user", "admin"})},
    {"pin",   Int().validate([](const Value &v) { return v.asInt() >= 100000 && v.asInt() <= 999999; },
                             "pin 6 digit ka hona chahiye")},
    {"phone", String().required(true, "phone number do")},
});
auto User = model("User", s);

try {
    User.create({{"name", "A"}, {"email", "galat"}, {"age", 200}, {"role", "raja"}, {"pin", 12}});
} catch (const ValidationError &e) {
    std::cout << e.what() << "\n";
    // User validation failed: age: Path `age` (200) is more than maximum allowed value (150)., email: email galat hai, ...

    for (const auto &[path, message] : e.errors)      // har field ka alag error
        std::cout << path << " -> " << message << "\n";

    if (e.errors.count("email")) { /* form me email ke neeche dikhao */ }
}
```

- Saare errors ek saath milte hain (pehle par ruk nahi jata).
- Nested ke path dotted hote hain: `address.city`, `items.0.sku`.
- Khaali (null / diya hi nahi) field par sirf `required` check hota hai; baaki validators tabhi chalte hain jab value ho.
- `.unique()` validator **nahi** hai. Wo index hai; duplicate par save ke waqt `DuplicateKeyError` aata hai (section 25).

---

## 8. Defaults, setters, cast

```cpp
Schema s({
    {"role",    String().def("user")},
    {"active",  Boolean().def(true)},
    {"joined",  DateTime().def([] { return Value(Date::now()); })},   // har naye document par chalta hai
    {"token",   String().def([] { return Value(uuid()); })},
    {"email",   String().trim().lowercase()},
    {"code",    String().uppercase()},
    {"slug",    String().set([](const Value &v) { return Value("post-" + v.asString()); })},
    {"age",     Int()},
});
auto T = model("Thing", s);

auto d = T({{"email", "  A@B.COM "}, {"code", "ab"}, {"slug", "hello"}, {"age", "24"}});
d["email"].asString();   // "a@b.com"
d["code"].asString();    // "AB"
d["slug"].asString();    // "post-hello"
d["age"].asInt();        // 24      <- string "24" apne aap int bana (cast)
d["role"].asString();    // "user"  <- default
```

**Cast** (Mongoose jaisa): `"24"` → `24`, `"true"` → `true`, ISO string → `Date`, 24-hex string → `ObjectId`.
Na ban paye to (Mongoose jaisa, v0.2 se) document banate waqt error **nahi** aata; `validate()` / `save()`
`ValidationError` deta hai jisme us path ka `kind` = `"cast"`:

```cpp
auto d = T({{"age", "abc"}});
try {
    d.save();
} catch (const ValidationError &e) {
    e.errors.at("age");   // Cast to Int failed for value "abc" at path `age`
    e.kinds.at("age");    // "cast"
}
```

Filter / update me galat value ho to turant `CastError` aata hai (`User.find({{"age", "abc"}})`).

Cast **filter aur update** me bhi hota hai: `User.find({{"age", "24"}})` aur `User.findById("6afb…")` seedha chalte hain.

---

## 9. Model + Document banana

```cpp
auto User = model("User", userSchema);                  // collection "users"  (naam chhota + bahuvachan)
auto Log  = model("Log", logSchema, "app_logs");        // collection ka naam khud do
auto Same = model("User");                              // kahin aur se wahi model (naam se)

User.name();             // "User"
User.collectionName();   // "users"
```

Naam se collection: `User → users`, `Category → categories`, `Box → boxes`.

**Banane ke 3 tarike:**

```cpp
// 1) create: banao + validate + save, ek saath
auto a = User.create({{"name", "Araham"}, {"email", "a@x.in"}});

// 2) pehle memory me banao, baad me save
auto b = User({{"name", "Ravi"}});
b.set("email", "ravi@x.in");
b.isNew();        // true
b.save();
b.isNew();        // false

// 3) bahut saare ek saath (ek hi DB call)
auto docs = User.insertMany({
    {{"name", "Sita"},  {"email", "sita@x.in"}},
    {{"name", "Mohan"}, {"email", "mohan@x.in"}},
});
```

Naye document ko `_id` (ObjectId) turant mil jaata hai, save se pehle bhi: `b.id()` (string) ya `b._id()` (Value).

`insertMany` me koi ek bhi document validation me fail ho to kuch bhi insert nahi hota.

---

## 10. Padhna: find / findOne / findById

```cpp
// bahut saare
std::vector<Document> all    = User.find().exec();
std::vector<Document> admins = User.find({{"role", "admin"}}).exec();

// ek
std::optional<Document> u = User.findOne({{"email", "a@x.in"}}).exec();
if (u) std::cout << (*u)["name"].asString();

// id se (string ya ObjectId dono chalte hain)
auto byId = User.findById("6afb9f7045827a9e0d061d21").exec();

// na mile to exception
Document must = User.findOne({{"email", "a@x.in"}}).orFail();   // NotFoundError

// lean: Document nahi, sirf plain Value (tez; sirf padhna ho to)
std::vector<Value>   rows = User.find().lean();
std::optional<Value> row  = User.findOne({{"role", "admin"}}).lean();
```

**Document se value padhna:**

```cpp
Document d = User.findOne().orFail();

d["name"].asString();                 // field
d["address.city"].asString();         // dotted path
d.get<int>("age");                    // seedha C++ type
d.has("nickname");                    // hai aur null nahi
d.id();                               // _id string me
d.json(2);                            // poora document JSON text me
d.toObject();                         // poora document Value me

for (const auto &t : d["tags"].elements()) std::cout << t.asString();
```

**MongoDB ke operators** seedha filter me:

```cpp
User.find({{"age", {{"$gte", 18}, {"$lt", 60}}}}).exec();
User.find({{"role", {{"$in", Value::array({"admin", "mod"})}}}}).exec();
User.find({{"$or", Value::array({ {{"age", {{"$lt", 18}}}}, {{"role", "admin"}} })}}).exec();
User.find({{"tags", "iot"}}).exec();                 // array me "iot" ho
User.find({{"address.city", "Durgapur"}}).exec();    // nested field
```

---

## 11. Query builder

Filter object ki jagah chain bhi likh sakte ho. `find()` aur `findOne()` dono par chalta hai.

```cpp
auto users = User.find()
    .where("age").gte(18).lt(60)             // age >= 18 AND age < 60
    .where("role").in({"user", "admin"})
    .where("name").regex("^a", "i")          // "a" se shuru, case-insensitive
    .where("email").exists()
    .where("city", "Durgapur")               // where(path, value) = barabar
    .sort("-age name")                       // age ghatte kram me, phir name
    .skip(20)
    .limit(10)
    .select("name email age")                // sirf ye fields
    .exec();
```

| Method | MongoDB |
|---|---|
| `.where("f").equals(v)` / `.where("f", v)` | `{f: v}` |
| `.gt(v)` `.gte(v)` `.lt(v)` `.lte(v)` `.ne(v)` | `$gt` `$gte` `$lt` `$lte` `$ne` |
| `.in({...})` `.nin({...})` | `$in` `$nin` |
| `.exists()` / `.exists(false)` | `$exists` |
| `.regex("pattern", "i")` | `$regex` + `$options` |
| `.sort("-age name")` / `.sort({{"age", -1}})` | sort |
| `.skip(n)` `.limit(n)` | pagination |
| `.select("a b")` / `.select("-a -b")` / `.select("+hidden")` | projection |
| `.populate("path")` | section 17 |
| `.count()` | kitne match hue |

**select:**

```cpp
User.find().select("name email").exec();     // sirf name, email (+ _id)
User.find().select("-password -__v").exec(); // ye chhod ke sab
User.findOne().select("+password").exec();   // select(false) wala field bhi lao
```

**Pagination:**

```cpp
int page = 3, perPage = 20;
auto rows  = User.find({{"role", "user"}}).sort("-createdAt").skip((page - 1) * perPage).limit(perPage).lean();
auto total = User.countDocuments({{"role", "user"}});
```

---

## 12. Document badalna + save()

```cpp
auto u = User.findOne({{"email", "a@x.in"}}).orFail();

u.set("age", 25);                              // ek field
u.set("address.city", "Burdwan");              // nested
u.set({{"name", "Araham A."}, {"role", "admin"}});   // kai ek saath
u.push("tags", "esp32");                       // array me jodna
u.unset("nickname");                           // field hatana
u.set("age", 26).set("role", "user").save();   // chain

u.save();
```

`save()` sirf **badle hue fields** bhejta hai (`$set` / `$unset`), poora document nahi. Isse do jagah se alag-alag field badle to ek dusre ko nahi mitate.

```cpp
u.isModified();               // kuch bhi badla?
u.isModified("age");          // ye field badla?
u.modifiedPaths();            // {"age", "address.city"}
```

**Mixed ya gehre data ko seedha badalna:** `mut()` reference deta hai aur path ko modified mark kar deta hai.

```cpp
u.mut("extra")["theme"] = "dark";
u.mut("tags").push_back("new");
u.save();
```

**Dhyan do:**

- `set()` cast aur setters chalata hai (`set("age", "25")` → int 25). Cast na ho to error `validate()`/`save()` par `ValidationError` (kind `"cast"`) ban kar aata hai.
- `strict` schema me jo field schema me nahi hai, `set()` use chupchaap chhod deta hai.
- `.immutable()` field save ho chuke document me badalta nahi.
- `save()` pehle validate karta hai. Fail ho to `ValidationError`, DB me kuch nahi jaata.
- Document DB se delete ho chuka ho to `save()` par `NotFoundError`.

---

## 13. Seedha update

Document load kiye bina, ek hi DB call. Counter, status, bulk kaam ke liye ye behtar hai (atomic).

```cpp
// operator na do to apne aap $set ban jaata hai
User.updateOne({{"email", "a@x.in"}}, {{"age", 26}, {"role", "admin"}});

// MongoDB operators
User.updateOne({{"_id", id}}, {{"$inc", {{"loginCount", 1}}}});
User.updateOne({{"_id", id}}, {{"$push", {{"tags", "new"}}}});
User.updateOne({{"_id", id}}, {{"$addToSet", {{"tags", "unique"}}}});
User.updateOne({{"_id", id}}, {{"$pull", {{"tags", "old"}}}});
User.updateOne({{"_id", id}}, {{"$unset", {{"nickname", ""}}}});
User.updateOne({{"_id", id}}, {{"$set", {{"address.city", "Asansol"}}}, {"$inc", {{"v", 1}}}});

// bahut saare
UpdateResult r = User.updateMany({{"role", "user"}}, {{"active", false}});
r.matchedCount;     // kitne mile
r.modifiedCount;    // kitne sach me badle

// upsert: na ho to bana do (schema ke defaults + timestamps bhi lagte hain)
auto res = User.updateOne({{"email", "new@x.in"}}, {{"name", "Naya"}}, {.upsert = true});
if (!res.upsertedId.isNull()) std::cout << "naya bana: " << res.upsertedId.str();

// update par bhi validators ($set ke values par)
User.updateOne({{"_id", id}}, {{"age", -5}}, {.runValidators = true});   // ValidationError
```

**Update karke document wapas lena:**

```cpp
// purana document (update se pehle wala), Mongoose ke default jaisa
auto before = User.findOneAndUpdate({{"email", "a@x.in"}}, {{"age", 27}});

// naya document (update ke baad wala) = Mongoose ka { new: true }
auto after = User.findOneAndUpdate({{"email", "a@x.in"}}, {{"age", 27}}, {.returnNew = true});

auto byId = User.findByIdAndUpdate(id, {{"$inc", {{"points", 10}}}}, {.returnNew = true});

// counter jaisa: na ho to banao, aur naya wala lautao
auto c = Counter.findOneAndUpdate({{"_id", "orders"}}, {{"$inc", {{"seq", 1}}}}, {.returnNew = true, .upsert = true});
```

Na mile (aur upsert na ho) to `std::nullopt`.

`timestamps` on ho to har update par `updatedAt` apne aap badalta hai.

---

## 14. Delete

```cpp
u.deleteOne();                                        // ye document

int64_t n1 = User.deleteOne({{"email", "a@x.in"}});   // pehla match   -> 0 ya 1
int64_t n2 = User.deleteMany({{"active", false}});    // saare match
int64_t n3 = User.deleteMany();                       // SAB (dhyan se!)

auto gone  = User.findOneAndDelete({{"email", "a@x.in"}});   // delete + wo document wapas
auto gone2 = User.findByIdAndDelete(id);

User.dropCollection();                                // poori collection + indexes
```

---

## 15. count / exists / distinct / aggregate

```cpp
int64_t total  = User.countDocuments();
int64_t admins = User.countDocuments({{"role", "admin"}});
int64_t quick  = User.estimatedDocumentCount();          // tez, andaaze se (metadata se)
int64_t viaQ   = User.find().where("age").gte(18).count();

bool taken = User.exists({{"email", "a@x.in"}});

std::vector<Value> roles  = User.distinct("role");
std::vector<Value> cities = User.distinct("address.city", {{"active", true}});

// aggregate: pipeline = stages ka array, result plain Values
auto stats = User.aggregate(Value::array({
    {{"$match", {{"active", true}}}},
    {{"$group", {{"_id", "$role"}, {"count", {{"$sum", 1}}}, {"avgAge", {{"$avg", "$age"}}}}}},
    {{"$sort",  {{"count", -1}}}},
}));
for (const auto &row : stats)
    std::cout << row["_id"].asString() << ": " << row["count"].asInt() << ", avg " << row["avgAge"].asDouble() << "\n";
```

> v0.3 se `aggregate` ke `$match` stages bhi schema se cast hote hain (string id → ObjectId, date string → Date). Detail: section 33.

---

## 16. Nested object + sub-document array

```cpp
Schema orderSchema({
    {"customer", String().required()},
    {"address",  Object(Schema({
        {"city", String().required()},
        {"pin",  Int()},
    }))},
    {"items",    Array(Schema({
        {"sku",   String().required()},
        {"qty",   Int().def(1).min(1)},
        {"price", Number().required()},
    }))},
});
auto Order = model("Order", orderSchema);

auto o = Order.create({
    {"customer", "Araham"},
    {"address",  {{"city", "Durgapur"}, {"pin", 713209}}},
    {"items",    Value::array({
        {{"sku", "ESP32"}, {"price", 450}},               // qty default 1
        {{"sku", "DHT22"}, {"qty", 2}, {"price", 180}},
    })},
});

o["address.city"].asString();                    // "Durgapur"
o["items"].at(0)["qty"].asInt();                 // 1
o["items.1.sku"].asString();                     // "DHT22"

o.push("items", {{"sku", "OLED"}, {"price", 220}});   // cast + default ke saath
o.set("address.pin", 713402);
o.set("items.0.qty", 5);
o.save();

double total = 0;
for (const auto &it : o["items"].elements()) total += it["qty"].asInt() * it["price"].asDouble();

Order.find({{"items.sku", "ESP32"}}).exec();                      // kisi bhi item me
Order.updateOne({{"_id", o._id()}}, {{"$push", {{"items", {{"sku", "LED"}, {"price", 5}}}}}});
```

- Nested ke errors: `address.city`, `items.1.price`.
- `Object(Schema)` ke andar ka `required` field tab bhi zaroori hai jab poora nested object diya hi na ho (Mongoose ke nested path jaisa).
- Sub-documents ko apna `_id` **nahi** milta (Mongoose deta hai). Chahiye to schema me khud field rakho.

---

## 17. populate (ref)

Ek document me dusre ki `_id` rakho, padhte waqt poora document mangwa lo.

```cpp
auto Author = model("Author", Schema({{"name", String().required()}}));
auto Tag    = model("Tag",    Schema({{"label", String()}}));
auto Post   = model("Post",   Schema({
    {"title",    String().required()},
    {"author",   Id().ref("Author")},                      // ek
    {"tags",     Array(Id().ref("Tag"))},                  // list
    {"comments", Array(Schema({
        {"by",   Id().ref("Author")},                      // sub-document ke andar
        {"text", String()},
    }))},
}));

auto prem = Author.create({{"name", "Premchand"}});
auto t1   = Tag.create({{"label", "hindi"}});
auto post = Post.create({
    {"title", "Godaan"},
    {"author", prem._id()},                                // ya prem.id() string, cast ho jaata hai
    {"tags", Value::array({t1._id()})},
    {"comments", Value::array({ {{"by", prem._id()}, {"text", "Dhanyavaad"}} })},
});

// query ke saath
auto posts = Post.find().populate("author").populate("tags").populate("comments.by").exec();
for (auto &p : posts) {
    std::cout << p["title"].asString() << " by " << p["author.name"].asString() << "\n";
    for (const auto &t : p["tags"].elements())     std::cout << "  #" << t["label"].asString() << "\n";
    for (const auto &c : p["comments"].elements()) std::cout << "  " << c["by"]["name"].asString() << ": " << c["text"].asString() << "\n";
}

// ek document par, baad me
auto one = Post.findById(post._id()).orFail();
one["author"].isObjectId();     // true  (abhi sirf id)
one.populate("author");
one["author"]["name"].asString();

// lean ke saath bhi
auto rows = Post.find().populate("author").lean();
```

- Chahe 1000 posts hon, har `populate` path ke liye **ek hi** extra query lagti hai (`$in`), har post ke liye alag nahi.
- Ref wala document mil na paye to: akela field `null` ho jaata hai, list me se wo item hat jaata hai.
- Ref model ke `select(false)` fields populate me bhi nahi aate.
- Jis path par `.ref()` nahi hai uspe `populate()` → `Error`.
- Populate ke baad us field ko wapas `save()` mat karo (wo ab object hai, id nahi). Padhne ke liye populate, badalne ke liye id.

---

## 18. Hooks (middleware)

**Document hooks:** `"validate"`, `"save"`, `"deleteOne"`

```cpp
Schema s({{"email", String()}, {"password", String()}, {"slug", String()}, {"title", String()}});

s.pre("save", [](Document &doc) {
    if (doc.isModified("password"))                       // sirf tab jab password badla ho
        doc.set("password", myHash(doc["password"].asString()));
    if (doc.isNew())
        doc.set("slug", makeSlug(doc["title"].asString()));
});

s.post("save", [](Document &doc) {
    std::cout << "save hua: " << doc.id() << "\n";
});

s.pre("validate", [](Document &doc) { /* validation se pehle value theek karo */ });
s.pre("deleteOne", [](Document &doc) { /* is document se jude doosre data hatao */ });
s.post("deleteOne", [](Document &doc) { /* log */ });
```

`save()` ka kram: **pre validate → validation → post validate → pre save → DB → post save**

Hook me exception feko to kaam wahin ruk jaata hai (DB me kuch nahi jaata):

```cpp
s.pre("save", [](Document &doc) {
    if (doc["role"].asString() == "admin" && !doc.has("email")) throw Error("admin ke liye email zaroori hai");
});
```

**Query hooks:** `"find"`, `"findOne"`, `"countDocuments"`, `"updateOne"`, `"updateMany"`, `"findOneAndUpdate"`, `"deleteOne"`, `"deleteMany"`, `"findOneAndDelete"`

Inme filter (aur update) ko query chalne se pehle badal sakte ho:

```cpp
// Soft delete: deleted wale kabhi na dikhen
for (const char *op : {"find", "findOne", "countDocuments"})
    s.pre(op, [](QueryInfo &q) {
        if (!q.filter.contains("deleted")) q.filter["deleted"] = false;
    });

// Har update par ek field
s.pre("updateOne", [](QueryInfo &q) {
    (*q.update)["$set"]["lastTouchedBy"] = "system";     // q.update sirf update wale ops me hota hai
});

// q.op = "find" / "updateOne" ...,  q.model = model ka naam
```

**Kaun kya chalata hai:**

| Kaam | Hooks |
|---|---|
| `doc.save()`, `Model.create()` | validate + save |
| `Model.insertMany()` | sirf validate |
| `doc.deleteOne()` | deleteOne (document wala) |
| `Model.find()` / `findOne()` / `findById()` | find / findOne (query wala) |
| `Model.updateOne()` waghera | usi naam ka query hook; **save hooks nahi** |

Hooks model banane (`model(...)`) se **pehle** schema par lagao.

---

## 19. Virtuals, methods, toJSON

```cpp
Schema s({
    {"first",    String()},
    {"last",     String()},
    {"password", String()},
    {"born",     DateTime()},
}, {.virtuals = true});                       // toJSON() me virtuals + "id" bhi aayen

// Virtual: DB me save nahi hota, padhte waqt banta hai
s.virtual_("fullName", [](const Document &d) {
    return Value(d["first"].asString() + " " + d["last"].asString());
});

// Method: document par apna function
s.method("greet", [](Document &d, const Value &args) {
    return Value(args["hello"].asString() + ", " + d["first"].asString() + "!");
});
s.method("promote", [](Document &d, const Value &) {
    d.set("role", "admin").save();
    return Value(true);
});

// toJSON transform: API me bhejne se pehle saaf karo
s.toJSON([](const Document &, Value &out) {
    out.erase("password");
    out.erase("__v");
});

auto Person = model("Person", s);
auto p = Person.create({{"first", "Araham"}, {"last", "Abeddin"}, {"password", "x"}});

p["fullName"].asString();                                // "Araham Abeddin"
p.call("greet", {{"hello", "Namaste"}}).asString();      // "Namaste, Araham!"
p.call("promote");

p.toJSON();     // Value: virtuals + id ke saath, password / __v ke bina
p.json();       // wahi, JSON text me   <- HTTP response me ye bhejo
p.toObject();   // kaccha data, jaisa DB me hai
```

**Static** (model-level) function ke liye C++ me alag cheez nahi chahiye, seedha function likho:

```cpp
std::optional<Document> findByEmail(const std::string &email) {
    return model("User").findOne({{"email", email}}).exec();
}
```

---

## 20. Indexes

```cpp
Schema s({
    {"email",  String().unique()},          // unique index
    {"owner",  String().index()},           // normal index
    {"device", String()},
    {"at",     DateTime()},
    {"loc",    Mixed()},
});

s.index({{"device", 1}, {"at", -1}});                                   // compound
s.index({{"owner", 1}, {"email", 1}}, {{"unique", true}});              // compound unique
s.index({{"at", 1}}, {{"expireAfterSeconds", 7 * 24 * 3600}});          // TTL: 7 din baad apne aap delete
s.index({{"loc", "2dsphere"}});                                         // geo
s.index({{"title", "text"}});                                           // text search
s.index({{"email", 1}}, {{"sparse", true}, {"name", "email_sparse"}});  // MongoDB ke koi bhi index options
```

- Indexes model ke **pehle DB operation** par apne aap ban jaate hain (`autoIndex`).
- Production me khud control chahiye to: `{.autoIndex = false}` rakho aur deploy ke waqt `User.createIndexes();` bulao.
- Nested schema ke `.index()` / `.unique()` bhi bante hain (`address.pin` jaise).

---

## 21. Custom `_id`

By default `_id` ObjectId hota hai. Apna chahiye to schema me `_id` likh do:

```cpp
// String key
auto Setting = model("Setting", Schema({
    {"_id",   String()},
    {"value", Mixed()},
}));
Setting.create({{"_id", "theme"}, {"value", {{"dark", true}}}});
auto theme = Setting.findById("theme").orFail();
Setting.updateOne({{"_id", "lang"}}, {{"value", "hi"}}, {.upsert = true});

// UUID apne aap
auto Device = model("Device", Schema({
    {"_id",  String().def([] { return Value(uuid()); })},
    {"name", String()},
}));
auto d = Device.create({{"name", "ESP32"}});
d.id();     // "6220f7dd-8fd1-4d29-8a6b-5aa145479403"

// Number
auto Seq = model("Seq", Schema({{"_id", Int()}, {"label", String()}}));
Seq.create({{"_id", 1}, {"label", "pehla"}});
```

Same `_id` dobara daalo to `DuplicateKeyError`.

---

## 22. Discriminators

Ek hi collection me alag-alag "type" ke documents, har type ke apne extra fields.

```cpp
auto Device = model("Device", Schema({
    {"name",   String().required()},
    {"online", Boolean().def(false)},
}));
auto Sensor = Device.discriminator("Sensor", Schema({{"unit", String().required()}, {"value", Number()}}));
auto Relay  = Device.discriminator("Relay",  Schema({{"channels", Int().def(1)}}));

Sensor.create({{"name", "temp"}, {"unit", "C"}, {"value", 31.5}});   // devices collection me, __t: "Sensor"
Relay.create({{"name", "pump"}, {"channels", 4}});
Device.create({{"name", "generic"}});

Sensor.find().exec();            // sirf sensors
Relay.countDocuments();          // sirf relays
Device.find().exec();            // SAB

for (auto &d : Device.find().exec()) {
    if (d.modelName() == "Sensor")      std::cout << d["value"].asDouble() << d["unit"].asString();
    else if (d.modelName() == "Relay")  std::cout << d["channels"].asInt();
}
```

- Base model se find karo to bhi har document apne sahi type ka `Document` banta hai (uske schema se cast/validate hota hai).
- Child model ke find / update / delete / count / aggregate me type ka filter apne aap lagta hai.
- Type wale field ka naam badalna ho: `{.discriminatorKey = "kind"}`.
- Discriminators model banane ke turant baad, DB use karne se pehle register karo.

---

## 23. Transactions

Kai kaam ek saath: ya sab honge, ya koi nahi.

```cpp
transaction([&] {
    Account.updateOne({{"owner", "a"}}, {{"$inc", {{"balance", -500}}}});
    Account.updateOne({{"owner", "b"}}, {{"$inc", {{"balance",  500}}}});
    Transfer.create({{"from", "a"}, {"to", "b"}, {"amount", 500}});
});
```

Andar se koi bhi exception nikle to **sab rollback**, aur wahi exception bahar aata hai:

```cpp
try {
    transaction([&] {
        auto a = Account.findOne({{"owner", "a"}}).orFail();
        if (a["balance"].asInt() < 500) throw Error("balance kam hai");
        a.set("balance", a["balance"].asInt() - 500).save();
        Account.updateOne({{"owner", "b"}}, {{"$inc", {{"balance", 500}}}});
    });
} catch (const Error &e) {
    std::cout << "transfer nahi hua: " << e.what();
}
```

- `transaction()` ke andar usi thread par nevla ke saare calls apne aap us transaction me chalte hain; session khud pass nahi karna.
- MongoDB me transactions sirf **replica set / Atlas** par chalte hain. Akele `mongod` par `Error` aayega. Local test ke liye: `mongod --replSet rs0` chalao aur ek baar `mongosh --eval "rs.initiate()"`.
- Network gadbad par driver transaction dobara chala sakta hai, isliye andar aisa kaam mat karo jo dobara hone par galat ho jaye (jaise email bhejna).

---

## 24. Typed struct: `NEVLA_STRUCT`

String keys ki jagah apna C++ struct chahiye? Ek macro se jod do.

```cpp
struct Address {
    std::string city;
    int pin = 0;
};
NEVLA_STRUCT(Address, city, pin)

struct Student {
    std::string id;                       // "id" <-> MongoDB "_id"
    std::string name;
    int roll = 0;
    double cgpa = 0;
    std::vector<std::string> subjects;
    std::optional<std::string> hostel;    // DB me na ho to nullopt
    Address address;                      // nested struct
    Date admitted;
};
NEVLA_STRUCT(Student, id, name, roll, cgpa, subjects, hostel, address, admitted)
```

Macro struct ke **bahar, usi namespace me** likho.

```cpp
auto StudentModel = model("Student", studentSchema);

// struct -> DB
Student s;
s.name = "Araham";  s.roll = 42;  s.cgpa = 8.7;
s.subjects = {"DSP", "Embedded"};
s.address = {"Durgapur", 713209};
auto doc = StudentModel.create(toValue(s));     // schema validation ke saath

// DB -> struct
Student back = doc.as<Student>();               // back.id ab bhara hua hai
Student one  = StudentModel.findById(back.id).orFail().as<Student>();

std::vector<Student> toppers;
for (auto &v : StudentModel.find().where("cgpa").gte(8).lean()) toppers.push_back(v.as<Student>());

// struct ke badlav wapas DB me
one.cgpa = 9.1;
StudentModel.findById(one.id).orFail().set(toValue(one)).save();
```

- `id` naam ka member `_id` se judta hai. Khaali ho to bheja nahi jaata (DB khud banata hai).
- Chalne wale member types: `std::string`, `bool`, saare int/float, `Date`, `ObjectId`, `Value`, `std::vector<T>`, `std::optional<T>`, aur koi bhi `NEVLA_STRUCT` wala struct.
- Bina model/schema ke bhi chalta hai: `Value v = toValue(s);  Student s2 = v.as<Student>();`

**Apna type jodna** (jaise enum): do function likho, usi namespace me.

```cpp
enum class Role { User, Admin };
inline void to_value(nevla::Value &out, const Role &r) { out = (r == Role::Admin ? "admin" : "user"); }
inline void from_value(const nevla::Value &v, Role &r) { r = (v.asString() == "admin" ? Role::Admin : Role::User); }
```

---

## 25. Errors

Sab `nevla::Error` (jo `std::runtime_error` hai) se bante hain.

| Error | Kab |
|---|---|
| `ValidationError` | schema validation fail. `.errors` = `map<path, message>`, `.kinds` = `map<path, kind>` |
| `CastError` | filter / update ki value sahi type me nahi badal payi. `.path` |
| `VersionError` | `optimisticConcurrency` on hai aur beech me kisi aur ne document save kar diya |
| `DuplicateKeyError` | unique index / `_id` dobara (MongoDB E11000) |
| `NotFoundError` | `orFail()` ko kuch nahi mila; `save()` ka document DB me nahi |
| `Error` | baaki sab: connection, galat JSON, MongoDB ke doosre errors |

```cpp
try {
    User.create(body);
} catch (const ValidationError &e) {
    // 400: e.errors me har field ka message
} catch (const CastError &e) {
    // 400: e.path wala field galat type ka hai
} catch (const DuplicateKeyError &e) {
    // 409: pehle se hai
} catch (const NotFoundError &e) {
    // 404
} catch (const Error &e) {
    // 500: e.what()
}
```

Pehle chhote (derived) errors pakdo, `Error` sabse aakhir me.

---

## 26. Threads + async

- nevla **thread-safe** hai: ek hi `Model` ko kai threads se ek saath use karo (andar connection pool hai).
- Ek hi `Document` object ko do threads me ek saath mat badlo. Har thread apna document load kare.
- Normal calls **blocking** hain (jawab aane tak rukte hain). Coroutine me `co_await ...Async()` use karo (section 35).

Blocking kaam ko alag thread par bhejna ho:

```cpp
auto fut = nevla::async([&] { return User.find({{"active", true}}).lean(); });
// ... yahan doosra kaam ...
std::vector<Value> users = fut.get();     // exception ho to yahan aata hai
```

Event-loop wale server (Drogon, Boost.Asio…) me blocking call event-loop thread par seedha mat chalao, warna us dauran baaki requests rukti hain. Wahan `co_await User.find(...).leanAsync()` likho aur Drogon me `nevla::useDrogonLoop()` ek baar call karo (section 35).

---

## 27. Schema options

```cpp
Schema s({...}, {
    .timestamps       = true,        // createdAt + updatedAt apne aap
    .collection       = "my_coll",   // collection ka naam
    .strict           = true,        // schema me na likhe fields chhod do (false = sab rakh lo)
    .versionKey       = "__v",       // "" = band
    .discriminatorKey = "__t",
    .autoIndex        = true,        // pehle operation par indexes banao
    .virtuals         = false,       // toJSON() me virtuals + aliases + "id"
    .createdAt        = "createdAt", // timestamp fields ke naam (jaise "created_at")
    .updatedAt        = "updatedAt",
    .optimisticConcurrency = false,  // save() par VersionError agar beech me kisi ne badla
    .getters          = false,       // toJSON() me field getters lagao
});
```

Jo chahiye sirf wahi likho (kram yahi rakhna hai, C++ ka niyam): `{.timestamps = true, .virtuals = true}`.

**timestamps:**

```cpp
auto p = Post.create({{"title", "x"}});
p["createdAt"].asDate().iso();     // banne ka time (kabhi nahi badalta)
p["updatedAt"].asDate().iso();     // har save() / update par naya
Post.find().sort("-createdAt").exec();
```

**strict = false:**

```cpp
auto Loose = model("Loose", Schema({{"a", Int()}}, {.strict = false}));
auto l = Loose.create({{"a", 1}, {"kuchBhi", {{"x", 1}}}});     // "kuchBhi" bhi save hoga
l.set("aurBhi", "haan").save();
```

---

## 28. Mongoose ↔ nevla cheat-sheet

| Mongoose (JavaScript) | nevla (C++) |
|---|---|
| `mongoose.connect(uri)` | `connect(uri, "db")` |
| `new Schema({ name: { type: String, required: true } })` | `Schema({{"name", String().required()}})` |
| `{ type: Number, min: 0, default: 1 }` | `Number().min(0).def(1)` |
| `{ type: String, enum: ['a','b'] }` | `String().enumOf({"a", "b"})` |
| `{ type: ObjectId, ref: 'User' }` | `Id().ref("User")` |
| `[String]` | `Array(String())` |
| `[{ sku: String }]` | `Array(Schema({{"sku", String()}}))` |
| `Schema.Types.Mixed` | `Mixed()` |
| `{ timestamps: true }` | `{.timestamps = true}` |
| `mongoose.model('User', schema)` | `model("User", schema)` |
| `new User({...})` | `User({...})` |
| `await User.create({...})` | `User.create({...})` |
| `await User.find({ age: { $gte: 18 } })` | `User.find({{"age", {{"$gte", 18}}}}).exec()` |
| `await User.findOne({...})` | `User.findOne({...}).exec()` |
| `await User.findById(id)` | `User.findById(id).exec()` |
| `.orFail()` | `.orFail()` |
| `.lean()` | `.lean()` |
| `.where('age').gte(18).sort('-age').limit(10)` | `.where("age").gte(18).sort("-age").limit(10)` |
| `.select('name -_id')` / `.select('+password')` | `.select("name -_id")` / `.select("+password")` |
| `.populate('author')` | `.populate("author")` |
| `doc.name` | `doc["name"].asString()` |
| `doc.name = 'x'` | `doc.set("name", "x")` |
| `doc.tags.push('a')` | `doc.push("tags", "a")` |
| `doc.markModified('extra')` | `doc.markModified("extra")` ya `doc.mut("extra")` |
| `doc.isModified('password')` | `doc.isModified("password")` |
| `await doc.save()` | `doc.save()` |
| `await doc.deleteOne()` | `doc.deleteOne()` |
| `User.updateOne(f, u, { upsert: true })` | `User.updateOne(f, u, {.upsert = true})` |
| `findOneAndUpdate(f, u, { new: true })` | `findOneAndUpdate(f, u, {.returnNew = true})` |
| `User.countDocuments(f)` | `User.countDocuments(f)` |
| `User.exists(f)` | `User.exists(f)` |
| `User.distinct('role')` | `User.distinct("role")` |
| `User.aggregate([...])` | `User.aggregate(Value::array({...}))` |
| `schema.pre('save', function () {...})` | `schema.pre("save", [](Document &d) {...})` |
| `schema.virtual('x').get(fn)` | `schema.virtual_("x", fn)` |
| `schema.methods.greet = fn` / `doc.greet()` | `schema.method("greet", fn)` / `doc.call("greet")` |
| `schema.index({ a: 1, b: -1 })` | `schema.index({{"a", 1}, {"b", -1}})` |
| `Base.discriminator('Child', schema)` | `Base.discriminator("Child", schema)` |
| `session.withTransaction(fn)` | `transaction(fn)` |
| `doc.toJSON()` / `doc.toObject()` | `doc.toJSON()` / `doc.toObject()` |

---

## 29. Mongoose se kya alag hai

**Likhne ka tarika (C++ ki majboori):**

- `doc.name` nahi ho sakta → `doc["name"]` padhne ke liye, `doc.set("name", v)` likhne ke liye.
- `doc["name"]` sirf padhne ka hai. Seedha badalna ho to `doc.mut("path")`.
- `await` ki jagah `co_await` (`...Async()` wale functions, section 35).
- `virtual` C++ ka keyword hai, isliye `virtual_()`.

**Jaan-boojh kar alag:**

- Sub-documents ko apna `_id` nahi milta.
- Query ka `countDocuments()` `skip`/`limit` ko nahi maanta.
- Ek hi connection (ek database) ek waqt me.

**Abhi nahi hai (aage jod sakte ho):**

- types: `Decimal128`, `Buffer` (binary), `BigInt`
- `Model.translateAliases()`

---

## 30. Tests, examples, folder

```
nevla/
├── include/nevla/
│   ├── nevla.hpp      <- bas yahi include karo
│   ├── value.hpp      Value, Date, ObjectId
│   ├── schema.hpp     Schema, Field, errors
│   ├── model.hpp      connect, Model, Document, Query
│   └── typed.hpp      NEVLA_STRUCT
├── src/               value.cc, schema.cc, model.cc
├── examples/
│   ├── 01_quickstart.cc      CRUD
│   ├── 02_blog.cc            populate, hooks, virtuals, methods
│   ├── 03_iot.cc             custom _id, discriminators, TTL, aggregate, transaction
│   ├── 04_typed_struct.cc    NEVLA_STRUCT
│   ├── 05_features.cc        v0.2: statics, plugin, Map, getters, virtual populate, paginate, cursor, bulkWrite
│   ├── 06_iot_dashboard.cc   end-to-end: threads se data bhejo + dashboard, har number verify
│   ├── 07_offline_api.cc     v0.5 ka tour, bina MongoDB ke (memory://)
│   └── 08_drogon.cc          Drogon + co_await (Drogon installed ho tabhi build hota hai)
├── tests/test.cc      MongoDB par 1658 checks, memory:// par 708
├── CMakeLists.txt
├── LICENSE            MIT
└── README.md
```

```bash
cd build
./nevla_tests        # "1658 passed, 0 failed"
NEVLA_TEST_URI=memory://t ./nevla_tests   # "708 passed, 0 failed" (bina server ke)
ctest                # sab ek saath (7 tests)
./01_quickstart
./02_blog
./03_iot
./04_typed_struct
```

Kis par test hua: Ubuntu 24.04, g++ 13, mongocxx 3.10.2, MongoDB 8 (replica set). AddressSanitizer + UBSan ke saath bhi saaf.

CMake options: `-DNEVLA_BUILD_TESTS=OFF -DNEVLA_BUILD_EXAMPLES=OFF` (sirf library).

---

## 31. Open-source karne ke steps

1. **Naam pakka karo.** "mongoose" naam mat rakhna (wo unka naam hai). GitHub par dekh lo ki jo naam chaho wo khaali hai.
2. `LICENSE` me naam/saal dekh lo (MIT rakha hai: koi bhi, company bhi, free me use kar sake).
3. GitHub par repo banao aur push karo:
   ```bash
   cd nevla
   git init && git add . && git commit -m "nevla v0.4.0"
   git branch -M main
   git remote add origin https://github.com/<tum>/nevla.git
   git push -u origin main
   git tag v0.4.0 && git push --tags
   ```
4. English README (`README.md`) ready hai; ye Hinglish guide `docs/` me hai.
5. `.github/workflows/ci.yml` pehle se hai: har push par driver build karke MongoDB ke saath `nevla_tests` chalata hai. README ke badge me `<your-username>` badal do.
6. Baad me: vcpkg / Conan par daalna.

### Naam badalna

Poore project me `nevla` → naya naam, ek command se:

```bash
NEW=mynaam
grep -rl --exclude-dir=build -i nevla . | xargs sed -i "s/nevla/$NEW/g; s/NEVLA/${NEW^^}/g"
mv include/nevla include/$NEW && mv include/$NEW/nevla.hpp include/$NEW/$NEW.hpp
```

---

## 32. v0.2 me kya naya hai (short me)

Poori detail English `README.md` me hai; yahan har cheez ka chhota example:

```cpp
// Field ke naye options
{"age",    Int().min(18, "{PATH} 18+ hona chahiye, mila {VALUE}")}   // custom message + {PATH}/{VALUE}
{"born",   DateTime().min(*Date::parse("1900-01-01"))}             // Date min/max
{"prices", Map(Number().min(0))}                                    // {"inr": 499, "usd": 6}
{"name",   String().alias("n")}                                     // doc.get("n"), filter/update me bhi
{"paise",  Int().get([](const Value &v) { return Value(v.asDouble() / 100); })}  // getter
{"phone",  String().unique().sparse()}                              // sparse index
{"expAt",  DateTime().expires(0)}                                   // TTL index

// Statics + plugin
schema.static_("byEmail", [](const Model &M, const Value &e) { return M.findOne({{"email", e}}).lean().value_or(Value()); });
User.call("byEmail", "a@x.in");
schema.plugin(softDelete);                          // void softDelete(Schema &s) {...}

// Post query hooks (result badal bhi sakte ho) + aggregate hook
schema.post("find", [](QueryInfo &q, Value &docs) { /* docs = array */ });
schema.pre("aggregate", [](QueryInfo &q) { q.filter.push_back(Value{{"$limit", 100}}); });

// Query: or_/and_/nor, elemMatch, size, all, collation, hint ...
User.find().or_({{{"role", "admin"}}, {{"age", {{"$gte", 60}}}}}).collation({{"locale", "en"}, {"strength", 2}}).exec();

// Bade data ke liye cursor, aur pagination
auto cur = Log.find().batchSize(500).cursor();
while (auto d = cur.next()) { /* ek-ek document */ }
Page p = Post.find().sort("-createdAt").paginate(2, 20);   // p.docs, p.total, p.pages, p.hasNext

// populate options + nested + virtual populate
PopulateOptions po;  po.path = "author";  po.select = "name company";
PopulateOptions c;   c.path = "company";  po.populate.push_back(c);
Post.find().populate(po).exec();
authorSchema.virtualPopulate("posts", {.ref = "Post", .localField = "_id", .foreignField = "author"});

// Naye Model functions
User.replaceOne(filter, newDoc);   User.findOneAndReplace(filter, newDoc, {.returnNew = true});
User.bulkWrite({{{"insertOne", {{"document", {...}}}}}, {{"deleteOne", {{"filter", {...}}}}}});
auto stream = User.watch();  if (auto ev = stream.next()) { /* change event */ }   // replica set chahiye
User.syncIndexes();   User.listIndexes();   User.where("age").gte(18).exec();

// Document helpers
doc.updateOne({{"$inc", {{"n", 1}}}});  doc.equals(other);  doc.increment();  doc.overwrite({...});
doc.populated("author");  doc.depopulate("author");

// Optimistic concurrency
Schema s({...}, {.optimisticConcurrency = true});   // purane version wala save() -> VersionError

// Debug logging (mongoose.set('debug', true) jaisa)
nevla::setDebug(true);
// Auto index fail ho (jaise pehle se duplicate data) to model chalta rahega, error yahan aayega
nevla::onIndexError([](const std::string &model, const std::string &msg) { /* log */ });
```

**Update ab Mongoose jaisa aur strict hai:** schema me na likhe paths hat jaate hain, `immutable` fields update
se nahi badalte (upsert par `$setOnInsert` me jaate hain), `$inc`/`$mul`/`$pull` ki values bhi cast hoti hain,
positional paths (`items.$.qty`, `items.$[].qty`) cast hote hain, aur `createdAt` update se kabhi nahi badalta.

**Robustness:** `connect()` dobara bulana (reconnect) chalte hue operations ke beech bhi safe hai; collection
naam ab Mongoose jaisa (`Person` → `people`, `Category` → `categories`); invalid regex aur galat date
(`2026-02-30`) pehle hi pakad liye jaate hain.

---

## 33. Aggregation builder (v0.3) — aasaan pipeline

**Pipeline kya hai:** documents ek line me kai stages se guzarte hain (filter → group → sort → limit).
Har stage ek kaam karti hai aur result aage deti hai. Saara hisaab MongoDB server par hota hai, tumhare program
me sirf chhota result aata hai.

Pehle aisa likhna padta tha (bahut brackets, `"$field"` strings, `ObjectId(...)` khud banana):

```cpp
Point.aggregate(Value::array({
    {{"$match", {{"device", ObjectId(id)}, {"at", {{"$gte", *Date::parse("2026-11-17")}}}}}},
    {{"$group", {{"_id", "$device"}, {"avgValue", {{"$avg", "$value"}}}, {"count", {{"$sum", 1}}}}}},
    {{"$sort", {{"avgValue", -1}}}},
}));
```

Ab bas itna:

```cpp
auto top = Point.aggregate()
    .match({{"device", id}, {"at", {{"$gte", "2026-11-17"}}}})   // string id / date string apne aap cast
    .group("device", {avg("value"), max("value"), count()})      // -> {_id, avgValue, maxValue, count}
    .sort("-avgValue")
    .limit(5)
    .exec();
```

**Auto-cast (Mongoose me nahi hai):** `$match` me string id → `ObjectId`, `"2026-11-17"` → `Date`, `"10"` → `10`,
alias → asli naam. Ye builder aur haath se likhe `aggregate(Value::array(...))` dono me hota hai.
`group` / `project` / `addFields` ke baad cast band ho jaata hai (kyunki tab fields schema jaise nahi rehte).
Galat value (`match({{"value", "garam"}})`) par turant `CastError` aata hai, chupchap khaali result nahi.

**Group ke accumulators:**

| Likho | Result field |
|---|---|
| `count()` | `count` |
| `sum("value")`, `avg("value")` | `sumValue`, `avgValue` |
| `min("value")`, `max("value")` | `minValue`, `maxValue` |
| `first("value")`, `last("value")` | `firstValue`, `lastValue` (pehle `sort()` lagao) |
| `push("value")`, `addToSet("tag")` | array |
| `avg("value").as("meanTemp")` | apna naam |

```cpp
.group("device room", {count()})     // do keys -> _id: {device, room}
.group("", {sum("amount")})          // poori collection
```

**Baaki stages:** `sort`, `limit`, `skip`, `sample`, `project("a b -_id")`, `addFields({...})`,
`unwind("tags")`, `lookup("Device", "device", "_id", "dev")` (model ka naam bhi chalega), `count("n")`,
`replaceRoot("doc")`, aur koi bhi stage kachcha: `stage({{"$facet", {...}}})`.

**Result lena:**

```cpp
agg.exec();                 // std::vector<Value>
agg.first();                // sirf pehla (optional)
agg.exec<DeviceStat>();     // seedha NEVLA_STRUCT me (group key "_id" -> member "id")
std::cout << agg.toJson();  // jo pipeline jaayegi wo dekho (mongosh me paste kar sakte ho)
```

**Room-wise report (join ke saath):**

```cpp
auto rooms = Reading.aggregate()
    .match({{"at", {{"$gte", "2026-11-01"}}}})
    .lookup("Device", "device", "_id", "dev")
    .unwind("dev")
    .group("dev.room", {avg("value"), count()})
    .sort("-avgValue")
    .exec();
```

Tip: `match()` jitna pehle ho sake lagao: index use hota hai aur aage kam data jaata hai.
Agar query chalte waqt collection ban/hat jaaye aur server `QueryPlanKilled` de, to nevla read ko apne aap
dobara chala leta hai.

Poora end-to-end test: `./build/06_iot_dashboard 12500`. Ye 8 threads se 1 lakh readings bhejta hai,
saath me dashboard queries chalata hai, aur aakhir me har count/sum/avg/min/max ko bheje gaye data se milata hai.

---

## 34. v0.4: formula, time buckets, ek-line reports, pipeline me populate, debugging

### Formula string (sabse aasaan tareeka)

MongoDB ke `{"$gt": ...}` waale objects ki jagah normal maths / C++ jaisi condition likho:

```cpp
User.find("age >= 18 && role in ['admin', 'editor']").exec();
User.countDocuments("plan == 'pro' || credits > 0");
User.updateMany("lastSeen < date('2026-01-01')", {{"active", false}});

Order.aggregate()
    .match("status == 'paid' && createdAt >= date('2026-11-01')")
    .addField("total", "price * qty * (1 - discount)")
    .group("customer", {sum("total"), sum("price * qty").as("gross"), count()})
    .exec();
```

- **Kya-kya likh sakte ho:** `+ - * / %`, `== != < <= > >=` (akela `=` bhi chalega), `in [..]`, `not in [..]`, `&& || !` (ya `and or not`), `'text'`, `date('2026-11-17')`, `now()`.
- **Functions:** `round`, `abs`, `min`, `max`, `if(cond, a, b)`, `concat`, `lower`, `year(at, 'Asia/Kolkata')`, `dateDiff(a, b, 'day')`, `size`, `exists(field)` ... Poori list English README me hai.
- **Speed:** `field > 30` jaise hisse normal filter bante hain, isliye index use hota hai aur string date bhi `Date` me cast hoti hai. Sirf `used > limit` (field vs field) jaise hisse `$expr` bante hain.
- **Galti:** galat formula par `FormulaError` aata hai, column number aur `^` nishaan ke saath:
  ```
  formula error at column 9: unexpected '*'
    price * * qty
            ^
  ```
- **Accumulator me formula:** naam dena zaroori hai, jaise `sum("price * qty").as("revenue")`.
- **Dhyan do (0.4 se badla):** jahan filter chahiye wahan string ab formula maani jaati hai. Pehle string
  ignore ho jaati thi, to `User.find("kuch bhi")` saare documents de deta tha. Ab `find("age >= 18")` asli filter
  hai, aur akela shabd field ka test hai: `find("active")` matlab `{active: true}`. Galat spelling bhi pakadni
  ho to `strictQuery` on karo (section 35).

### Chart ke liye time buckets: `bucketByTime`

```cpp
Reading.aggregate()
    .match({{"device", id}})
    .bucketByTime("at", "15m", {avg("value"), max("value"), count()})
    .exec();
// [{time: 10:00, avgValue: .., maxValue: .., count: ..}, {time: 10:15, ...}]   time ke kram me

// har device ki alag line, Indian time ke din, khaali ghante bhi (count 0) taaki graph me gap na dikhe
Reading.aggregate().bucketByTime("at", "1h", {avg("value")},
                                 {.by = "device", .timezone = "Asia/Kolkata", .fillGaps = true}).exec();
```

Interval aise likh sakte ho: `"30s" "15m" "1h" "1d" "1w" "1M"` (M = month), `"1y"`, ya `"2 hours"`.

### Ek line ki reports

```cpp
Order.countBy("status");                 // {"paid": 120, "pending": 8}   (sabse bada pehle)
Order.sumBy("customer", "amount");       // har customer ka total
Reading.avgBy("device", "value");        // har device ki average
Reading.stats("value");                  // {count, sum, avg, min, max, stdDev}
Reading.latestPerGroup("device", "at");  // har device ki sabse nayi reading
```

Sabme filter bhi de sakte ho: `Order.countBy("status", "createdAt >= date('2026-11-01')")`.

### Pipeline ke andar populate

```cpp
Reading.aggregate().populate("device").group("device.room", {avg("value"), count()}).exec();
Author.aggregate().populate("posts").addField("postCount", "size(posts)").sort("-postCount").exec();
```

- `$lookup` nevla schema ke `ref` se khud bana deta hai; `select`, `match`, `sort`, `limit` aur nested populate sab chalte hain.
- Ise `group()`/`project()` se **pehle** lagao.
- Array ke andar ke sub-documents (`comments.by`) ke liye `find().populate()` use karo.

### Debugging

```cpp
auto agg = Reading.aggregate().match("value > 50").group("device", {count()});
std::cout << agg.toShell();   // db.getCollection("readings").aggregate([...])  -> mongosh me paste karo
std::cout << agg.plan();      // "IXSCAN(value_1)" = index laga;  "COLLSCAN" = poori collection padhi, index banao
```

Is version ko `06_iot_dashboard` se bhi test kiya: 8 threads ne 1 lakh readings bheji, 5 threads ek saath dashboards
chala rahe the, aur formula, buckets, countBy, stats, populate, sabke result bheje gaye data se match hue.

---

## 35. v0.5: wo cheezein jo Mongoose me nahi hain

Pehle ek seedha sawaal: **nevla khud ka driver hai?** Nahi. Neeche MongoDB ka official **mongocxx** driver hai.
Network, connection pool, TLS, login aur replica set wo sambhalta hai. nevla uske upar ODM layer hai (schema,
validation, hooks, populate ...). Sirf `memory://` wala test-database nevla ka apna engine hai.

### Typed model: galti compile time pe pakdi jaaye

```cpp
struct Device { std::string id, name, room; double temp = 0; bool online = false; std::vector<std::string> tags; };
NEVLA_STRUCT(Device, id, name, room, temp, online, tags)

TypedModel<Device> Devices("Device");          // schema struct se khud ban jaata hai
Device d = Devices.create({.name = "temp-1", .room = "lab", .temp = 24.5});
auto hot = Devices.find().where(&Device::temp).gte(30).lt(60).sort(&Device::temp, -1).limit(10).exec();
Devices.update(d.id).set(&Device::temp, 26.0).push(&Device::tags, "calibrated").exec();
```

`&Device::tmep` likha (spelling galat) ya `temp` ko `"hot"` se compare kiya, to **program compile hi nahi hoga**.
Mongoose me aisi galti chup-chaap khaali result deti hai. Rules chahiye to `schemaOf<Device>()` lo aur `add()` karo.

### `co_await` (Drogon ke saath bhi)

```cpp
Task<int64_t> dashboard(Model Users) {
    auto adults = co_await Users.find("age >= 18").countAsync();
    auto rows   = co_await Users.find("role == 'admin'").limit(5).leanAsync();
    co_return adults;
}
```

- DB ka kaam nevla ke thread pool pe hota hai; event loop thread kabhi nahi rukta.
- **Kisi bhi coroutine type** me chalta hai: `drogon::Task<>`, Asio, ya nevla ka apna `Task<T>`. Koi adapter nahi.
- **Drogon me ek line zaroor likho:** `nevla::useDrogonLoop();` (`#include <nevla/drogon.hpp>`).
  Iske bina `co_await` ke baad handler nevla ke pool thread pe chalta, Drogon ke IO loop pe nahi. Iske saath
  har handler usi IO loop pe wapas aata hai jahan se shuru hua tha. `08_drogon --check` ye 400 requests
  pe check karta hai (bina is line ke 400+ handler galat thread pe milte hain, test fail).
- Drogon nahi, koi aur loop hai? `nevla::setResumeOn(...)` se apna loop batao (English README me example).
- Validation error ko 400 banana: `e.toJSON()` deta hai `{"email": "Path `email` is required."}`.

**GCC 13 ka bug:** `co_await` wali line me `{{"name", "x"}}` jaisi braces likhi to GCC 13 khud crash ho jaata
hai (*internal compiler error*). Function me lapetne se bhi nahi bachta. Isliye braces ki jagah ye likho:

```cpp
co_await User.createAsync(nevla::obj("name", "Asha", "age", 21));
co_await User.find(nevla::obj("age", nevla::obj("$gte", 18))).countAsync();
co_await User.find("age >= 18").countAsync();      // ya formula
```

GCC 14+, Clang me braces bhi chalte hain.

### `memory://`: bina MongoDB ke test

```cpp
connect("memory://test");      // database program ke andar hi, koi server nahi
```

Wahi code, wahi API. Unit test ke liye perfect: na MongoDB install, na Docker, na download (Node ka
`mongodb-memory-server` to asli mongod download karta hai). nevla ka test suite iske upar 708 checks chalata hai,
aur 930 random queries asli MongoDB aur memory dono pe chala ke result match karta hai.
**Production ke liye nahi:** ek global lock hai, aur process band hote hi data gaya. Change streams, collation,
`$text`, geo, `$where` jaisi cheezein saaf error deti hain.

### `strictQuery`: field ki spelling galat to error

```cpp
nevla::setStrictQuery(true);
User.find({{"emial", "a@x.in"}}).exec();
// Error: unknown path "emial" in a filter on model "User" (strictQuery). Did you mean "email"?
```

### Slow query log

```cpp
nevla::setSlowQueryThreshold(100);   // 100 ms se dheemi query stderr pe, index ka sujhav bhi
```

### Time-series collection (sensor data ke liye, MongoDB 5.0+)

```cpp
Schema s({{"device", String()}, {"value", Number()}, {"at", DateTime().required()}},
         {.timeseries = {.timeField = "at", .metaField = "device", .expireAfterSeconds = 90 * 86400}});
```

### Batch writer (MQTT jaisa tez data)

```cpp
auto writer = Reading.batchWriter({.maxDocs = 500, .maxDelayMs = 200});
writer.add({{"device", id}, {"value", 30.5}, {"at", Date::now()}});   // validation abhi, likhna background me
writer.flush();
```

500 docs ho gaye ya 200 ms ho gaye, to ek `insertMany`. Queue bhar gayi to `add()` ruk jaata hai (memory nahi phoolti).

### `fromQuery`: URL se seedha query

```cpp
// GET /users?age[gte]=18&city[in]=Kolkata,Durgapur&sort=-createdAt&page=2&limit=20&fields=name,email
Page page = User.fromQuery(nevla::parseQueryString(req->query()), {.allow = {"age", "city", "createdAt"}});
return page.toJSON();     // {docs, total, page, perPage, pages, hasNext, hasPrev}
```

Jo field `allow` me nahi, wo `QueryParamError` (400 bhejo). `"18"` → number, `"2026-11-01"` → Date apne aap.

### Schema export + MongoDB validator

```cpp
userSchema.toTypeScript("User");   // React dashboard ke liye TypeScript interface
userSchema.toOpenAPI();            // Swagger
User.syncValidator();              // schema MongoDB me hi validator ban ke baith jaata hai
nevla::syncValidators();           // saare models ek saath (deploy step / CLI ke liye)
```

Validator ke baad mongosh ya Python se bhi galat data nahi ghus sakta.

### Plugins: soft delete, history, multi-tenant

```cpp
schema.plugin(nevla::plugins::softDelete);   // doc.call("softDelete") -> find() me chhup jaata hai, restore bhi
schema.plugin(nevla::plugins::history);      // har badlav ka record: kisne, kab, kya
schema.plugin(nevla::plugins::multiTenant);  // har company ka data alag

nevla::TenantScope t("acme");                // is request ki saari queries sirf "acme" ki
nevla::ActorScope who("araham@acme.in");     // history me "kisne"
```

Tenant set nahi kiya to error. Galti se doosri company ka data kabhi nahi dikhega.

### Field encryption

```cpp
nevla::setEncryptionKey(std::getenv("NEVLA_KEY"));
Schema s({{"aadhaar", String().encrypted()},       // sabse safe, search nahi hoga
          {"token",   String().encrypted(true)}});  // isse == se dhundh sakte ho
```

AES-256-GCM. Database me sirf `nvenc:1:...` dikhta hai. Key kho gayi to data bhi gaya; key database me mat rakho.

### Migrations

```cpp
nevla::migration("2026-11-20-add-plan",
    [] { User.updateMany(Value::object(), {{"plan", "free"}}); },
    [] { User.updateMany(Value::object(), {{"$unset", {{"plan", ""}}}}); });
nevla::migrate();     // jo nahi chali wahi chalegi; do server ek saath start hon to bhi ek hi baar
nevla::rollback();
```

### findById cache

```cpp
User.cache({.ttlMs = 30000});   // findById memory se; nevla se koi bhi write hua to cache saaf
```

Pura example: `examples/07_offline_api.cc` (bina MongoDB ke chalta hai) aur `examples/08_drogon.cc`.
Har cheez ki detail English README me hai.

---

## License

MIT. `LICENSE` file dekho.
