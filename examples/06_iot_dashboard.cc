// End-to-end check with the aggregation builder: like an IoT backend.
//  - 8 writer threads send telemetry for 20 devices (single inserts + batches + updates)
//  - 3 reader threads build dashboard numbers at the same time
//  - at the end every number from MongoDB is compared with what was actually sent
// Exit code 0 = everything matched.
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <nevla/nevla.hpp>
#include <random>
#include <thread>

using namespace nevla;
using Clock = std::chrono::steady_clock;

struct DeviceSummary {
    std::string id;  // group key (_id) = device id
    double avgValue = 0, minValue = 0, maxValue = 0, sumValue = 0;
    int64_t count = 0;
};
NEVLA_STRUCT(DeviceSummary, id, avgValue, minValue, maxValue, sumValue, count)

int main(int argc, char **argv) {
    const char *env = std::getenv("NEVLA_URI");
    connect(env ? env : "mongodb://127.0.0.1:27017", "nevla_demo");
    const int devicesN = 20, writers = 8, perWriter = argc > 1 ? std::atoi(argv[1]) : 2500;

    auto Device = model("DashDevice", Schema({{"name", String().required()},
                                              {"room", String().enumOf({"lab", "hall", "roof", "store"})},
                                              {"online", Boolean().def(false)},
                                              {"lastValue", Number()}}));
    auto Reading = model("DashReading", Schema({{"device", Id().ref("DashDevice").required()},
                                                {"value", Number().required()},
                                                {"unit", String().def("C")},
                                                {"at", DateTime().required()},
                                                {"note", String()}},
                                               {.versionKey = ""}));
    Device.dropCollection();
    Reading.dropCollection();

    const char *rooms[] = {"lab", "hall", "roof", "store"};
    std::vector<std::string> ids;
    for (int i = 0; i < devicesN; i++)
        ids.push_back(Device.create({{"name", "sensor-" + std::to_string(i)}, {"room", rooms[i % 4]}}).id());

    // What we sent, per device (to check MongoDB's answers later)
    struct Sent {
        int64_t n = 0;
        double sum = 0, min = 1e18, max = -1e18;
    };
    std::map<std::string, Sent> sent;
    std::mutex sentMu;
    std::atomic<int64_t> writeErrors{0}, readErrors{0}, reads{0};
    std::atomic<bool> writing{true};

    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int w = 0; w < writers; w++)
        threads.emplace_back([&, w] {
            std::mt19937 rng(w * 7919 + 1);
            std::uniform_int_distribution<int> dev(0, devicesN - 1);
            std::uniform_real_distribution<double> val(-40.0, 85.0);
            std::map<std::string, Sent> mine;
            std::vector<Value> batch;
            auto remember = [&](const std::string &id, double v) {
                auto &s = mine[id];
                s.n++;
                s.sum += v;
                s.min = std::min(s.min, v);
                s.max = std::max(s.max, v);
            };
            for (int i = 0; i < perWriter; i++) {
                const std::string &id = ids[dev(rng)];
                double v = std::round(val(rng) * 100) / 100;  // 2 decimals like a real sensor
                // values arrive in different shapes, like from MQTT: numbers, numeric strings, ISO dates
                Value value = i % 3 == 0 ? Value(std::to_string(v)) : Value(v);
                Value at = i % 2 ? Value(Date::now()) : Value(Date::now().iso());
                try {
                    if (i % 10 == 0) {  // single insert + device status update
                        Reading.create({{"device", id}, {"value", value}, {"at", at}, {"note", "single ✓ ₹"}});
                        Device.updateOne({{"_id", id}}, {{"online", true}, {"lastValue", value}});
                    } else {
                        batch.push_back({{"device", id}, {"value", value}, {"at", at}});
                        if (batch.size() == 100) {
                            Reading.insertMany(batch);
                            batch.clear();
                        }
                    }
                    remember(id, v);
                } catch (const std::exception &e) {
                    writeErrors++;
                    std::cerr << "write error: " << e.what() << "\n";
                }
            }
            if (!batch.empty()) Reading.insertMany(batch);
            std::lock_guard<std::mutex> lk(sentMu);
            for (auto &[id, s] : mine) {
                auto &t = sent[id];
                t.n += s.n;
                t.sum += s.sum;
                t.min = std::min(t.min, s.min);
                t.max = std::max(t.max, s.max);
            }
        });

    // Dashboard readers running while data is still coming in
    for (int r = 0; r < 3; r++)
        threads.emplace_back([&, r] {
            while (writing) {
                try {
                    if (r == 0) {
                        Reading.aggregate().group("device", {avg("value"), count()}).sort("-avgValue").limit(5).exec();
                    } else if (r == 1) {
                        Reading.aggregate()
                            .match({{"device", ids[0]}, {"at", {{"$gte", "2020-01-01"}}}})
                            .sort("-at")
                            .limit(20)
                            .exec();
                    } else {
                        Reading.aggregate()
                            .lookup("DashDevice", "device", "_id", "dev")
                            .unwind("dev")
                            .group("dev.room", {avg("value"), count()})
                            .exec();
                    }
                    reads++;
                } catch (const std::exception &e) {
                    readErrors++;
                    std::cerr << "read error: " << e.what() << "\n";
                }
            }
        });

    for (int w = 0; w < writers; w++) threads[w].join();
    writing = false;
    for (size_t i = writers; i < threads.size(); i++) threads[i].join();
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    // ---- verify ----
    int problems = 0;
    auto bad = [&](const std::string &what) {
        problems++;
        std::cout << "MISMATCH: " << what << "\n";
    };
    const int64_t expected = int64_t(writers) * perWriter;
    int64_t stored = Reading.countDocuments();
    if (stored != expected) bad("stored " + std::to_string(stored) + " != sent " + std::to_string(expected));

    auto summary = Reading.aggregate()
                       .group("device", {avg("value"), min("value"), max("value"), sum("value"), count()})
                       .sort("_id")
                       .exec<DeviceSummary>();
    if ((int)summary.size() != (int)sent.size()) bad("device count");
    for (const auto &s : summary) {
        const Sent &e = sent[s.id];
        if (s.count != e.n) bad(s.id + " count");
        if (std::fabs(s.sumValue - e.sum) > 1e-6 * std::max(1.0, std::fabs(e.sum))) bad(s.id + " sum");
        if (std::fabs(s.avgValue - e.sum / e.n) > 1e-9 * std::max(1.0, std::fabs(e.sum / e.n)) + 1e-9)
            bad(s.id + " avg");
        if (s.minValue != e.min) bad(s.id + " min");
        if (s.maxValue != e.max) bad(s.id + " max");
    }

    // every document was cast to the schema's types (strings -> numbers, ISO strings -> dates)
    auto types = Reading.aggregate()
                     .group("", {accumulator("$addToSet", Value{{"$type", "$value"}}, "valueTypes"),
                                 accumulator("$addToSet", Value{{"$type", "$at"}}, "atTypes"),
                                 accumulator("$addToSet", Value{{"$type", "$device"}}, "deviceTypes")})
                     .first()
                     .value();
    for (const auto &t : types["valueTypes"].elements())
        if (t.asString() != "double" && t.asString() != "int") bad("value stored as " + t.asString());
    if (types["atTypes"].size() != 1 || types["atTypes"].at(0).asString() != "date")
        bad("at type " + types["atTypes"].toJson());
    if (types["deviceTypes"].size() != 1 || types["deviceTypes"].at(0).asString() != "objectId") bad("device type");

    // room report (lookup) adds up to the total
    int64_t roomTotal = 0;
    for (auto &r : Reading.aggregate()
                       .lookup("DashDevice", "device", "_id", "dev")
                       .unwind("dev")
                       .group("dev.room", {count()})
                       .exec())
        roomTotal += r["count"].asInt();
    if (roomTotal != expected) bad("room total");

    // unicode survived the round trip, devices got their status
    auto note = Reading.findOne({{"note", {{"$exists", true}}}}).lean();
    if (!note || (*note)["note"].asString() != "single ✓ ₹") bad("unicode note");
    if (Device.countDocuments({{"online", true}}) == 0) bad("device status updates");

    // bad input is rejected, not stored
    int rejected = 0;
    try {
        Reading.create({{"device", ids[0]}, {"value", "hot"}, {"at", Date::now()}});
    } catch (const ValidationError &) {
        rejected++;
    }
    try {
        Reading.create({{"device", "not-an-id"}, {"value", 1}, {"at", Date::now()}});
    } catch (const ValidationError &) {
        rejected++;
    }
    try {
        Reading.create({{"device", ids[0]}, {"value", 1}, {"at", "yesterday"}});
    } catch (const ValidationError &) {
        rejected++;
    }
    try {
        Reading.aggregate().match({{"value", "hot"}}).exec();
    } catch (const CastError &) {
        rejected++;
    }
    if (rejected != 4) bad("bad input accepted");
    if (Reading.countDocuments() != expected) bad("bad input was stored");

    std::cout << "sent " << expected << " readings from " << writers << " threads in " << secs << " s ("
              << int64_t(expected / secs) << "/s) while running " << reads << " dashboard queries\n"
              << "write errors: " << writeErrors << ", read errors: " << readErrors << ", mismatches: " << problems
              << "\n";
    std::cout << "top device: " << summary.front().id << " avg " << summary.front().avgValue << "\n";
    Device.dropCollection();
    Reading.dropCollection();
    return (problems || writeErrors || readErrors) ? 1 : 0;
}
