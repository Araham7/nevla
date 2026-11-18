// The smallest example: connect -> schema -> model -> create / find / update / delete
#include <iostream>
#include <nevla/nevla.hpp>

using namespace nevla;

int main() {
    connect("mongodb://127.0.0.1:27017", "nevla_demo");

    Schema userSchema(
        {
            {"name", String().required().trim()},
            {"email", String().required().lowercase().unique()},
            {"age", Int().min(0)},
            {"role", String().enumOf({"user", "admin"}).def("user")},
        },
        {.timestamps = true});

    auto User = model("User", userSchema);  // collection: "users"
    User.deleteMany();

    // CREATE
    auto u = User.create({{"name", "  Araham "}, {"email", "ARAHAM@Example.com"}, {"age", 24}});
    std::cout << "created: " << u.json(2) << "\n";

    // READ
    auto found = User.findOne({{"email", "araham@example.com"}}).exec();
    if (found)
        std::cout << "found: " << (*found)["name"].asString() << ", role = " << found->get<std::string>("role") << "\n";

    // UPDATE (change the document and save: only the changed fields are sent to the DB)
    u.set("age", 25);
    u.save();

    // Validation
    try {
        User.create({{"name", "No Email"}});
    } catch (const ValidationError &e) {
        std::cout << "validation: " << e.what() << "\n";
    }
    // Unique index
    try {
        User.create({{"name", "Another"}, {"email", "araham@example.com"}});
    } catch (const DuplicateKeyError &) {
        std::cout << "this email is already taken\n";
    }

    // DELETE
    u.deleteOne();
    std::cout << "left: " << User.countDocuments() << "\n";
}
