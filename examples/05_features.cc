// v0.2 tour: statics, plugins, aliases, getters, Map, virtual populate, nested populate,
// pagination, cursors, bulkWrite, optimistic concurrency, debug logging
#include <iostream>
#include <nevla/nevla.hpp>

using namespace nevla;

// A reusable plugin: soft delete
void softDelete(Schema &s) {
    s.add("deletedAt", DateTime());
    s.pre("find", [](QueryInfo &q) {
        if (!q.filter.contains("deletedAt")) q.filter["deletedAt"] = nullptr;  // hide deleted rows
    });
    s.method("softDelete", [](Document &d, const Value &) {
        d.set("deletedAt", Date::now()).save();
        return Value(true);
    });
}

int main() {
    connect("mongodb://127.0.0.1:27017", "nevla_demo");
    setDebug(true);  // print every operation to stderr (like mongoose.set('debug', true))

    /* ---------- Shop: Map prices, getters, aliases, statics ---------- */
    Schema shopSchema({
        {"name", String().required().alias("n")},
        {"city", String().index()},
    });
    shopSchema.virtualPopulate("products", {.ref = "Product", .localField = "_id", .foreignField = "shop"});
    shopSchema.static_("byCity", [](const Model &M, const Value &city) {
        Value out = Value::array();
        for (auto &v : M.find({{"city", city}}).lean()) out.push_back(v);
        return out;
    });
    auto Shop = model("Shop", shopSchema);

    Schema productSchema(
        {
            {"title", String().required()},
            {"shop", Id().ref("Shop")},
            {"paise", Int().min(0, "{PATH} cannot be negative (got {VALUE})").get([](const Value &v) {
                 return Value(v.asDouble() / 100.0);
             })},                          // stored in paise
            {"stock", Map(Int().min(0))},  // stock per warehouse: {"dgp": 4, "kol": 0}
            {"tags", Array(String().lowercase())},
        },
        {.timestamps = true, .optimisticConcurrency = true, .getters = true});
    productSchema.plugin(softDelete);
    auto Product = model("Product", productSchema);

    Shop.deleteMany();
    Product.deleteMany();

    auto shop = Shop.create({{"n", "Araham Electronics"}, {"city", "Durgapur"}});
    std::cout << "shop name via alias: " << shop.get("n").asString() << "\n";

    /* ---------- bulkWrite ---------- */
    std::vector<Value> ops;
    for (int i = 1; i <= 12; i++)
        ops.push_back({{"insertOne",
                        {{"document",
                          {{"title", "Part " + std::to_string(i)},
                           {"shop", shop._id()},
                           {"paise", i * 1000},
                           {"stock", {{"dgp", i}, {"kol", 0}}},
                           {"tags", Value::array({"ESP32", "IoT"})}}}}}});
    auto bw = Product.bulkWrite(ops);
    std::cout << "inserted " << bw.insertedCount << " products\n";

    /* ---------- statics + virtual populate ---------- */
    std::cout << "shops in Durgapur: " << Shop.call("byCity", "Durgapur").size() << "\n";
    PopulateOptions cheap;
    cheap.path = "products";
    cheap.match = {{"paise", {{"$lt", 5000}}}};
    cheap.sort = "paise";
    cheap.select = "title paise shop";
    auto s = Shop.findById(shop._id()).populate(cheap).orFail();
    std::cout << "cheap products: " << s["products"].size() << "\n";

    /* ---------- pagination ---------- */
    auto page = Product.find().sort("paise").select("title paise").paginate(2, 5);
    std::cout << "page " << page.page << "/" << page.pages << " (" << page.total << " total):";
    for (auto &d : page.docs) std::cout << " " << d["title"].asString() << "=Rs" << d.get("paise").asDouble();
    std::cout << "\n";

    /* ---------- cursor: stream without loading everything ---------- */
    int64_t totalStock = 0;
    auto cur = Product.find().batchSize(4).cursor();
    while (auto d = cur.next()) totalStock += (*d)["stock.dgp"].asInt();
    std::cout << "total stock in dgp: " << totalStock << "\n";

    /* ---------- validation with custom message ---------- */
    try {
        Product.create({{"title", "Broken"}, {"paise", -5}});
    } catch (const ValidationError &e) {
        std::cout << "validation: " << e.errors.at("paise") << " [" << e.kinds.at("paise") << "]\n";
    }

    /* ---------- optimistic concurrency ---------- */
    auto a = Product.findOne({{"title", "Part 1"}}).orFail();
    auto b = Product.findOne({{"title", "Part 1"}}).orFail();
    a.set("paise", 1500).save();
    try {
        b.set("paise", 1700).save();
    } catch (const VersionError &e) {
        std::cout << "VersionError: " << e.what() << "\n";
    }

    /* ---------- plugin method: soft delete ---------- */
    a.call("softDelete");
    std::cout << "visible products after soft delete: " << Product.find().exec().size() << " (find() hides it)\n";

    setDebug(false);
}
