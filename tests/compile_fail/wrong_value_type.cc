// Must NOT compile: age is an int, "eighteen" is text
#include <nevla/nevla.hpp>
struct User { std::string id, name; int age = 0; };
NEVLA_STRUCT(User, id, name, age)
void f(nevla::TypedModel<User> &Users) { Users.find().where(&User::age).gte("eighteen").exec(); }
