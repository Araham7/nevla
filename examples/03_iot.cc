// IoT jaisa data: custom string _id, discriminators, TTL + compound index, aggregate, transaction
#include <nevla/nevla.hpp>

#include <iostream>

using namespace nevla;

int main() {
    connect("mongodb://127.0.0.1:27017", "nevla_demo");

    // Device: apni string _id (UUID), ek hi collection me alag-alag type (discriminator)
    Schema deviceSchema({
        {"_id", String().def([] { return Value(uuid()); })},
        {"name", String().required()},
        {"owner", String().required().index()},
        {"online", Boolean().def(false)},
        {"lastSeen", DateTime()},
        {"meta", Mixed()},
    }, {.timestamps = true});
    auto Device = model("Device", deviceSchema);
    auto Sensor = Device.discriminator("Sensor", Schema({{"unit", String().required()}, {"value", Number()}}));
    auto Relay = Device.discriminator("Relay", Schema({{"state", Boolean().def(false)}}));

    // Telemetry: 7 din baad apne aap delete (TTL), device + time par index
    Schema pointSchema({
        {"device", String().required()},
        {"at", DateTime().def([] { return Value(Date::now()); })},
        {"value", Number().required()},
    }, {.versionKey = ""});
    pointSchema.index({{"device", 1}, {"at", -1}});
    pointSchema.index({{"at", 1}}, {{"expireAfterSeconds", 7 * 24 * 3600}});
    auto Point = model("Point", pointSchema, "telemetry_points");

    Device.deleteMany();
    Point.deleteMany();

    auto temp = Sensor.create({{"name", "Room temp"}, {"owner", "araham"}, {"unit", "C"}, {"meta", {{"pin", 4}, {"board", "ESP32"}}}});
    auto pump = Relay.create({{"name", "Pump"}, {"owner", "araham"}});

    for (double v : {29.5, 30.25, 31.0, 30.5}) Point.create({{"device", temp.id()}, {"value", v}});

    // Device online aaya: ek hi atomic update
    Device.updateOne({{"_id", temp.id()}}, {{"online", true}, {"lastSeen", Date::now()}, {"value", 30.5}});

    for (auto &d : Device.find({{"owner", "araham"}}).sort("name").exec())  // har ek sahi type ka Document
        std::cout << d.modelName() << "  " << d["name"].asString() << "  online=" << d["online"].asBool() << "\n";
    std::cout << "sensors: " << Sensor.countDocuments() << ", relays: " << Relay.countDocuments() << "\n";

    auto stats = Point.aggregate(Value::array({
        {{"$match", {{"device", temp.id()}}}},
        {{"$group", {{"_id", "$device"}, {"avg", {{"$avg", "$value"}}}, {"max", {{"$max", "$value"}}}, {"n", {{"$sum", 1}}}}}},
    }));
    std::cout << "stats: " << stats[0].toJson() << "\n";

    // Transaction (replica set / Atlas chahiye): device + uska data ek saath hatao
    try {
        transaction([&] {
            Point.deleteMany({{"device", temp.id()}});
            Device.deleteOne({{"_id", temp.id()}});
        });
        std::cout << "device + telemetry ek saath delete\n";
    } catch (const Error &e) {
        std::cout << "transaction nahi chala: " << e.what() << "\n";
    }
}
