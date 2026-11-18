// Must NOT compile: $inc on a string member
#include <nevla/nevla.hpp>
struct User { std::string id, name; int age = 0; };
NEVLA_STRUCT(User, id, name, age)
void f(nevla::TypedModel<User> &Users) { Users.update("x").inc(&User::name, 1).exec(); }
