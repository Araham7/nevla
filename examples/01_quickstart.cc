// Sabse chhota example: connect -> schema -> model -> create / find / update / delete
#include <nevla/nevla.hpp>

#include <iostream>

using namespace nevla;

int main() {
    connect("mongodb://127.0.0.1:27017", "nevla_demo");

    Schema userSchema({
        {"name", String().required().trim()},
        {"email", String().required().lowercase().unique()},
        {"age", Int().min(0)},
        {"role", String().enumOf({"user", "admin"}).def("user")},
    }, {.timestamps = true});

    auto User = model("User", userSchema);  // collection: "users"
    User.deleteMany();

    // CREATE
    auto u = User.create({{"name", "  Araham "}, {"email", "ARAHAM@Example.com"}, {"age", 24}});
    std::cout << "bana: " << u.json(2) << "\n";

    // READ
    auto found = User.findOne({{"email", "araham@example.com"}}).exec();
    if (found) std::cout << "mila: " << (*found)["name"].asString() << ", role = " << found->get<std::string>("role") << "\n";

    // UPDATE (document badlo, save karo: sirf badle hue fields DB me jaate hain)
    u.set("age", 25);
    u.save();

    // Validation
    try {
        User.create({{"name", "Bina Email"}});
    } catch (const ValidationError &e) {
        std::cout << "validation: " << e.what() << "\n";
    }
    // Unique index
    try {
        User.create({{"name", "Dusra"}, {"email", "araham@example.com"}});
    } catch (const DuplicateKeyError &) {
        std::cout << "ye email pehle se hai\n";
    }

    // DELETE
    u.deleteOne();
    std::cout << "bache: " << User.countDocuments() << "\n";
}
