# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/).

## [0.5.0] - 2026-11-19

### Added (features Mongoose does not have)
- **Typed models**: `TypedModel<T>` on a `NEVLA_STRUCT`, queries and updates with member pointers
  (`where(&Device::temp).gte(30)`, `update(id).push(&Device::tags, "x")`); typos and wrong value types are
  compile errors (checked by `tests/compile_fail`). `schemaOf<T>()` builds a schema from the struct.
- **co_await**: `...Async()` forms of queries, writes, aggregate and `saveAsync()` returning `Async<T>`, usable
  from any coroutine type (`drogon::Task<>`, Asio, ...); `nevla::Task<T>` for code without one; `asyncCall()`.
  Tenant / actor scope travels into the pool thread.
- **Resume on the awaiting event loop**: `setResumeOn(capture)`; `<nevla/drogon.hpp>` with `useDrogonLoop()`.
- `obj(...)` / `arr(...)`: build Values without braced lists (GCC 13 crashes on `{{...}}` inside `co_await`).
- **In-memory database**: `connect("memory://name")`, a MongoDB-compatible engine for tests and demos
  (CRUD, updates, unique indexes, `$jsonSchema`, aggregation, transactions). Checked against MongoDB by a
  differential test (930 comparisons).
- **strictQuery** (schema option or `setStrictQuery(true)`): an unknown filter path throws, with
  "Did you mean ...?".
- **Slow query log**: `setSlowQueryThreshold(ms, fn)` with an index suggestion (ESR rule).
- **Time-series collections**: `SchemaOptions::timeseries` (time / meta field, granularity, expiry).
- **Batch writer**: `Model::batchWriter({.maxDocs, .maxDelayMs, .maxQueue, .ordered})` with backpressure,
  `flush()`, `stats()`, `onError()`.
- **REST query parameters**: `Model::fromQuery(params, {.allow, ...})`, `parseQueryString()`, `Page::toJSON()`,
  `QueryParamError`.
- **Schema export**: `toJsonSchema()`, `toOpenAPI()`, `toTypeScript(name)`; `Model::syncValidator()` stores the
  schema as MongoDB's `$jsonSchema` validator; `syncValidators()` for every model; `createCollection()`,
  `collectionInfo()`.
- **Plugins**: `plugins::softDelete`, `plugins::history` (audit log, `ActorScope`), `plugins::multiTenant`
  (`TenantScope`, `AllTenantsScope`; no tenant = error).
- **Field encryption**: `String().encrypted()` (random IV) and `.encrypted(true)` (deterministic, equality
  filters work), AES-256-GCM via OpenSSL, `setEncryptionKey()`.
- **Migrations**: `migration(id, up, down)`, `migrate()` (with a lock against two servers),
  `rollback()`, `migrationStatus()`.
- **findById cache**: `Model::cache({.ttlMs, .maxEntries})`, cleared by every write through nevla.
- `ValidationError::toJSON()` (`{path: message}` for a 400 body), `dropDatabase()`.
- Examples `07_offline_api` (runs on `memory://`) and `08_drogon` (built when Drogon is found;
  `08_drogon --check` is a CTest test: 400 concurrent requests must all resume on their IO loop).
- CI runs `ctest` (MongoDB suite, memory suite, compile-fail checks) and example 07.

### Fixed
- `syncValidator()` on a discriminator child replaced the base model's validator for the whole collection;
  it now throws, and `syncValidators()` skips children.
- Typed `push()` / `pull()` did not compile with a string literal.
- `distinct()` and `bulkWrite()` now run query middleware (needed by the plugins).

### Changed
- nevla now links OpenSSL (`libcrypto`) for field encryption.

## [0.4.0] - 2026-11-18

### Added
- **Formula strings** (`include/nevla/formula.hpp`): `expr("price * qty * (1 - discount)")` and
  `formulaFilter("temp > 30 && room == 'lab'")` with maths, comparisons, `in` / `not in`, `&& || !`,
  `date('...')`, `now()` and 40 functions (`round`, `if`, `concat`, `year`, `dateDiff`, ...).
  Simple `field <op> constant` parts become normal, index-friendly, schema-cast filter conditions; the rest
  becomes `$expr`. Mistakes throw `FormulaError` with the column and a caret.
- Formulas everywhere a filter is accepted: `find("...")`, `findOne`, `countDocuments`, `exists`,
  `updateOne/Many`, `deleteOne/Many`, `bulkWrite` filters, `aggregate().match("...")`, `PopulateOptions::match`.
- `Aggregate::addField(name, formula)`; accumulators take formulas: `sum("price * qty").as("revenue")`.
- `Aggregate::bucketByTime(field, "15m", accumulators, {.by, .timezone, .fillGaps})` for chart data.
- `Aggregate::populate(path | PopulateOptions)`: `$lookup` from the schema's `ref` / `virtualPopulate`, with
  select (hidden fields stay hidden), match, sort, limit, nested populate, array order kept.
- One-line reports: `Model::countBy`, `sumBy`, `avgBy`, `stats`, `latestPerGroup`.
- Debugging: `Aggregate::toShell()` (paste into mongosh), `explain()`, `plan()` ("IXSCAN(...)" / "COLLSCAN").
- `06_iot_dashboard` now also checks formulas, buckets, countBy, stats, pipeline populate and latestPerGroup.

### Changed
- A string passed where a filter is expected is now parsed as a formula (it used to be ignored and match
  everything).

## [0.3.0] - 2026-11-18

### Added
- **Aggregation builder**: `Model.aggregate()` returns a chainable `Aggregate` with `match`, `group`, `sort`,
  `limit`, `skip`, `project`, `addFields`, `unwind`, `lookup` (model or collection name), `count`, `sample`,
  `replaceRoot`, `stage` (raw), `options` (`allowDiskUse`, `maxTimeMS`, `collation`, `hint`), and
  `exec()`, `exec<T>()` (into `NEVLA_STRUCT`), `first()`, `pipeline()`, `toJson()`.
- Accumulator helpers: `count()`, `sum`, `avg`, `min`, `max`, `first`, `last`, `push`, `addToSet`,
  `stdDevPop`, `accumulator(op, expr, name)`, with automatic output names (`avg("value")` → `avgValue`)
  and `.as("name")`. Several group keys: `group("device room", ...)`.
- **Automatic casting of `$match` in aggregation pipelines** (builder and hand-written pipelines) while the
  documents still have the schema's shape. Mongoose does not do this.
- Reads (`find`, `aggregate`) interrupted by the server with `QueryPlanKilled` / `CursorKilled` are retried
  (up to 3 attempts; never for `$out` / `$merge` or inside a transaction).
- Example `06_iot_dashboard.cc`: concurrent writers + live dashboard readers, verifies every aggregate.

### Changed
- `Model.aggregate(pipeline)` now requires an array (it used to treat anything else as an empty pipeline).

## [0.2.0] - 2026-11-18

### Added
- **Schema**: `Map(Field)` type; `alias()`, `get()` (getters), `sparse()`, `expires()` (TTL) field options;
  `min/max(Date)`; custom messages for `required`, `enumOf`, `min`, `max`, `minLength`, `maxLength`, `match`
  with `{PATH}` / `{VALUE}` placeholders; `Schema::remove()`, `plugin()` (with options), `static_()`,
  `virtualPopulate()`, `post()` query hooks.
- **Schema options**: `createdAt` / `updatedAt` names, `optimisticConcurrency`, `getters`.
- **ValidationError::kinds** (`required`, `enum`, `min`, `max`, `minlength`, `maxlength`, `regexp`,
  `user defined`, `cast`) and new **VersionError**.
- **Query**: `or_`, `and_`, `nor`, `all`, `size`, `mod`, `elemMatch`, `where(object)`, `where(path, value)`,
  `collation`, `hint`, `maxTimeMS`, `batchSize`, `comment`, `distinct`, `cursor()`, `forEach()`, `paginate()`.
- **Populate**: `PopulateOptions` (`select`, `match`, `sort`, `limit`, nested `populate`, `model`),
  virtual populate (`justOne` too), `Document::populated()` / `depopulate()`.
- **Model**: `replaceOne`, `findOneAndReplace`, `bulkWrite` (Mongoose format), `watch()` change streams,
  `syncIndexes`, `listIndexes`, `where(path)`, `call()` for statics, `insertMany({.ordered = false})`.
- **Document**: `updateOne`, `increment`, `equals`, `overwrite`.
- `setDebug(bool | callback)`, `onIndexError(handler)`, `databaseName()`, `ping()`, `modelNames()`.
- `std::map<std::string, T>` support in `as<T>()` / `toValue()` / `NEVLA_STRUCT`.
- `post("aggregate")` / `pre("aggregate")` hooks (the pipeline is `QueryInfo::filter`).
- Example `05_features.cc`, English README, Hinglish guide in `docs/`, CI workflow, CMake version file, `ctest`.

### Changed
- Values that cannot be cast in **documents** are now reported by `validate()` / `save()` as a
  `ValidationError` (kind `"cast"`) instead of throwing `CastError` when the document is built (Mongoose behaviour).
  Filters and updates still throw `CastError`.
- Updates are cast like Mongoose: unknown paths are removed (`strict`), `immutable` fields are removed (moved to
  `$setOnInsert` on upsert), `$inc` / `$mul` / `$pull` / `$pullAll` values are cast, positional paths are
  supported, `createdAt` is never changed by an update.
- The version key is incremented when an array path is saved.
- Collection names follow Mongoose pluralization more closely (`Person` → `people`, `Status` → `status`).
- All error messages are in English.

### Fixed
- `connect()` / `disconnect()` while other threads run operations could destroy the pool in use
  (connection is now reference-counted).
- A failing automatic index creation blocked every operation of the model; it is now reported and skipped.
- `Date::parse` accepted impossible dates such as `2026-02-30`.
- Invalid `match()` regexes failed at validation time; they now fail when the schema is defined.
- Validators that throw are treated as failed validation instead of escaping.
- Saving a populated document could store embedded documents instead of ids.
- Discriminator registration was not thread-safe.

## [0.1.0] - 2026-11-17

- First version: schemas, validation, casting, defaults, setters, CRUD, query builder, populate, document and
  query hooks, virtuals, methods, toJSON transform, indexes, custom `_id`, discriminators, transactions,
  `NEVLA_STRUCT`, thread safety.
