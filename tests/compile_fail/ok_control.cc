// MUST compile (control for the compile-fail tests)
#include <nevla/nevla.hpp>
struct User { std::string id, name; int age = 0; };
NEVLA_STRUCT(User, id, name, age)
void f(nevla::TypedModel<User> &Users) {
    Users.find().where(&User::age).gte(18).exec();
    Users.update("x").inc(&User::age, 1).exec();
}
