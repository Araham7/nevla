#pragma once
/* Built-in plugins and request context.

   Soft delete
     schema.plugin(nevla::plugins::softDelete);            // or plugins::softDelete(schema, {{"field", "removedAt"}})
     doc.call("softDelete");  doc.call("restore");  doc.call("isDeleted");
     User.call("softDelete", "lastSeen < date('2025-01-01')");   User.call("restore", filter);
     User.find(...)                            -> hides soft-deleted documents
     User.find({{"$withDeleted", true}})       -> shows everything
     User.find({{"deletedAt", {{"$ne", nullptr}}}}) -> only the deleted ones

   History (audit log)
     schema.plugin(nevla::plugins::history);               // {{"collection", "nevla_history"}}
     nevla::ActorScope who("asha@x.in");                   // who did it (this thread / request)
     User.call("history", id)  -> [{op, changes, by, at, ...}, ...]

   Multi-tenant
     schema.plugin(nevla::plugins::multiTenant);           // {{"field", "tenantId"}}
     nevla::TenantScope t("acme");                         // every query / insert of this thread is scoped to acme
     nevla::AllTenantsScope admin;                         // admin tools: no tenant filter
     No tenant set -> nevla::Error (never a silent cross-tenant query).

   Encryption
     nevla::setEncryptionKey(key);                          // 64 hex characters (32 bytes) or a passphrase
     {"aadhaar", String().encrypted()}  {"token", String().encrypted(true)}  (true = can be filtered on)  */

#include <optional>
#include <string>

#include "async.hpp"
#include "schema.hpp"

namespace nevla {

/* ---------------- request context (thread-local, travels into *Async() calls) ---------------- */
class TenantScope {
  public:
    explicit TenantScope(std::string tenant);
    ~TenantScope();
    TenantScope(const TenantScope &) = delete;
    TenantScope &operator=(const TenantScope &) = delete;

  private:
    detail::Context saved_;
};
class AllTenantsScope {  // admin / maintenance code: multiTenant models are not filtered
  public:
    AllTenantsScope();
    ~AllTenantsScope();
    AllTenantsScope(const AllTenantsScope &) = delete;
    AllTenantsScope &operator=(const AllTenantsScope &) = delete;

  private:
    detail::Context saved_;
};
class ActorScope {  // who is doing the work (history plugin)
  public:
    explicit ActorScope(std::string actor);
    ~ActorScope();
    ActorScope(const ActorScope &) = delete;
    ActorScope &operator=(const ActorScope &) = delete;

  private:
    detail::Context saved_;
};
std::optional<std::string> currentTenant();
std::string currentActor();

/* ---------------- encryption ---------------- */
void setEncryptionKey(const std::string &key);  // 64 hex chars = raw 32-byte key, anything else = passphrase (SHA-256)
void clearEncryptionKey();

namespace plugins {
struct SoftDelete {
    void operator()(Schema &s) const { (*this)(s, Value()); }
    void operator()(Schema &s, const Value &options) const;  // {"field": "deletedAt"}
};
struct History {
    void operator()(Schema &s) const { (*this)(s, Value()); }
    void operator()(Schema &s, const Value &options) const;  // {"collection": "nevla_history"}
};
struct MultiTenant {
    void operator()(Schema &s) const { (*this)(s, Value()); }
    void operator()(Schema &s, const Value &options) const;  // {"field": "tenantId"}
};
inline constexpr SoftDelete softDelete{};
inline constexpr History history{};
inline constexpr MultiTenant multiTenant{};
}  // namespace plugins

namespace detail {
// used by the library
bool hasEncryptionKey();
bool isEncryptedText(const std::string &s);
std::string encryptText(const std::string &plain, bool deterministic);
std::string decryptText(const std::string &cipher);
}  // namespace detail

}  // namespace nevla
