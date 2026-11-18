// Must NOT compile: there is no member User::agee
#include <nevla/nevla.hpp>
struct User { std::string id, name; int age = 0; };
NEVLA_STRUCT(User, id, name, age)
void f(nevla::TypedModel<User> &Users) { Users.find().where(&User::agee).gte(18).exec(); }
