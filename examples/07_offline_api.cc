// v0.5 tour on the built-in in-memory database: runs anywhere, no MongoDB needed.
//   ./07_offline_api                     (memory://)
//   ./07_offline_api mongodb://127.0.0.1:27017   (the same code on a real server)
// Typed models, a REST-style query, multi-tenant + soft delete + history plugins, field encryption,
// a batch writer, migrations, the findById cache, co_await, schema export.
#include <nevla/nevla.hpp>

#include <iostream>

using namespace nevla;

struct Device {
    std::string id, name, room, token;
    double temp = 0;
    bool online = false;
    std::vector<std::string> tags;
};
NEVLA_STRUCT(Device, id, name, room, token, temp, online, tags)

Task<int64_t> onlineCount(Model devices) {
    int64_t n = co_await devices.find("online == true").countAsync();
    co_return n;
}

int main(int argc, char **argv) {
    connect(argc > 1 ? argv[1] : "memory://demo", "nevla_demo");
    setEncryptionKey("demo key: use a real secret from your environment");

    // ---- schema from the struct + rules + plugins
    Schema s = schemaOf<Device>({.timestamps = true});
    s.add("name", String().required().trim());
    s.add("room", String().enumOf({"lab", "hall", "roof"}));
    s.add("token", String().encrypted(true));  // stored encrypted, still searchable
    s.plugin(plugins::multiTenant);
    s.plugin(plugins::softDelete);
    s.plugin(plugins::history);
    TypedModel<Device> Devices("Device", s);
    Model raw = Devices.model();
    {
        AllTenantsScope admin;
        raw.dropCollection();
    }

    // ---- a migration (runs once, recorded in nevla_migrations)
    migration("2026-11-18-default-tags", [&] {
        AllTenantsScope admin;
        raw.updateMany("tags == null", {{"tags", Value::array({"new"})}});
    });

    TenantScope tenant("acme");  // everything below belongs to tenant "acme"
    ActorScope actor("araham@acme.in");

    Device a;
    a.name = "  temp-1 ";
    a.room = "lab";
    a.token = "tok-AAA";
    a.temp = 24.5;
    a.online = true;
    Device saved = Devices.create(a);
    std::cout << "created " << saved.id << " name='" << saved.name << "'\n";

    // ---- batch writer for many devices
    auto writer = raw.batchWriter({.maxDocs = 100, .maxDelayMs = 50});
    for (int i = 0; i < 250; i++)
        writer.add({{"name", "sensor-" + std::to_string(i)}, {"room", i % 2 ? "hall" : "roof"},
                    {"token", "tok-" + std::to_string(i)}, {"temp", 20 + i % 15}, {"online", i % 3 == 0}});
    writer.flush();
    std::cout << "batch: " << writer.stats().written << " written in " << writer.stats().batches << " batches\n";
    std::cout << "migrations run now: " << migrate().size() << "\n";

    // ---- typed, compile-time checked queries
    auto hot = Devices.find().where(&Device::temp).gte(30).sort(&Device::temp, -1).limit(3).exec();
    std::cout << "hottest: ";
    for (auto &d : hot) std::cout << d.name << "(" << d.temp << ") ";
    std::cout << "\n";
    auto byToken = Devices.findOne({{"token", "tok-42"}});  // equality on an encrypted field
    std::cout << "found by encrypted token: " << (byToken ? byToken->name : "-") << "\n";

    // ---- a REST request: GET /devices?room=hall&temp[gte]=30&sort=-temp&limit=5&fields=name,temp
    auto params = parseQueryString("room=hall&temp%5Bgte%5D=30&sort=-temp&limit=5&fields=name,temp");
    Page page = raw.fromQuery(params, {.allow = {"name", "room", "temp", "online"}});
    std::cout << "API page: " << page.toJSON()["docs"].size() << " of " << page.total << "\n";

    // ---- dashboard numbers
    std::cout << "per room: " << raw.countBy("room").toJson() << "\n";
    std::cout << "temp stats: " << raw.stats("temp").toJson() << "\n";
    std::cout << "online (co_await): " << onlineCount(raw).get() << "\n";

    // ---- soft delete + history
    auto doc = raw.findById(saved.id).orFail();
    doc.set("temp", 26.0).save();
    doc.call("softDelete");
    std::cout << "visible after soft delete: " << raw.countDocuments() << ", history entries: "
              << raw.call("history", saved.id).size() << "\n";

    // ---- the findById cache
    raw.cache({.ttlMs = 10000});  // (bypassed here: tenant / soft-delete middleware must always run)
    raw.findById(saved.id).lean();
    std::cout << "cache hits: " << raw.cacheStats().hits << "\n";

    // ---- another tenant sees nothing of acme's
    {
        TenantScope other("globex");
        std::cout << "globex sees " << raw.countDocuments() << " devices\n";
    }

    // ---- types for the React dashboard
    std::cout << "\n" << s.toTypeScript("Device");
}
