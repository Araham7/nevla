// MUST compile (control for the compile-fail tests)
#include <nevla/nevla.hpp>
struct User { std::string id, name; int age = 0; std::vector<std::string> tags; };
NEVLA_STRUCT(User, id, name, age, tags)
void f(nevla::TypedModel<User> &Users) {
    Users.find().where(&User::age).gte(18).exec();
    Users.find().where(&User::name).in({"a", "b"}).exec();
    Users.update("x").inc(&User::age, 1).exec();
    Users.update("x").push(&User::tags, "pro").pull(&User::tags, "old").exec();  // string literals
    Users.update("x").set(&User::name, "Asha").exec();
}
