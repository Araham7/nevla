#pragma once
// Internal: the in-memory database behind connect("memory://...") (for tests and demos, no MongoDB needed).
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nevla/schema.hpp"

namespace nevla::mem {

struct WriteResult {
    int64_t matched = 0, modified = 0;
    Value upsertedId;
};
struct InsertResult {
    int64_t inserted = 0;
    std::string error;  // first error (e.g. duplicate key); empty = all inserted
};
struct FindSpec {
    Value filter = Value::object(), sort, projection;
    int64_t skip = 0, limit = 0;
};

std::vector<Value> find(const std::string &db, const std::string &coll, const FindSpec &spec);
int64_t count(const std::string &db, const std::string &coll, const Value &filter);
int64_t estimatedCount(const std::string &db, const std::string &coll);
std::vector<Value> distinct(const std::string &db, const std::string &coll, const std::string &field,
                            const Value &filter);

void insertOne(const std::string &db, const std::string &coll, const Value &doc);  // throws DuplicateKeyError
InsertResult insertMany(const std::string &db, const std::string &coll, const std::vector<Value> &docs, bool ordered);
WriteResult update(const std::string &db, const std::string &coll, const Value &filter, const Value &update,
                   bool upsert, bool many);
WriteResult replace(const std::string &db, const std::string &coll, const Value &filter, const Value &replacement,
                    bool upsert);
int64_t remove(const std::string &db, const std::string &coll, const Value &filter, bool many);
std::optional<Value> findOneAndUpdate(const std::string &db, const std::string &coll, const Value &filter,
                                      const Value &update, bool upsert, bool returnNew, const Value &projection);
std::optional<Value> findOneAndReplace(const std::string &db, const std::string &coll, const Value &filter,
                                       const Value &replacement, bool upsert, bool returnNew, const Value &projection);
std::optional<Value> findOneAndDelete(const std::string &db, const std::string &coll, const Value &filter);

std::vector<Value> aggregate(const std::string &db, const std::string &coll, const Value &pipeline);

void createIndex(const std::string &db, const std::string &coll, const Value &keys, const Value &options);
std::vector<Value> listIndexes(const std::string &db, const std::string &coll);
void dropIndex(const std::string &db, const std::string &coll, const std::string &name);
void drop(const std::string &db, const std::string &coll);
void dropDatabase(const std::string &db);
bool createCollection(const std::string &db, const std::string &coll, const Value &options);  // false = existed
Value collectionInfo(const std::string &db, const std::string &coll);
void setValidator(const std::string &db, const std::string &coll, const Value &validator, const std::string &level);

// Runs fn with the whole database locked; on an exception every change is undone
void transaction(const std::string &db, const std::function<void()> &fn);

// Exposed for tests of the engine itself
bool matches(const Value &doc, const Value &filter);
int compareValues(const Value &a, const Value &b);

}  // namespace nevla::mem
