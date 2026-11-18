# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/).

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
