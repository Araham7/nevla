// Apne C++ struct ke saath: NEVLA_STRUCT (compile-time mapping) + schema validation dono
#include <nevla/nevla.hpp>

#include <iostream>

using namespace nevla;

struct Address {
    std::string city;
    int pin = 0;
};
NEVLA_STRUCT(Address, city, pin)

struct Student {
    std::string id;  // "id" <-> MongoDB "_id"
    std::string name;
    int roll = 0;
    double cgpa = 0;
    std::vector<std::string> subjects;
    std::optional<std::string> hostel;  // DB me na ho to nullopt
    Address address;
    Date admitted;
};
NEVLA_STRUCT(Student, id, name, roll, cgpa, subjects, hostel, address, admitted)

int main() {
    connect("mongodb://127.0.0.1:27017", "nevla_demo");

    auto StudentModel = model("Student", Schema({
        {"name", String().required()},
        {"roll", Int().required().unique()},
        {"cgpa", Number().min(0).max(10)},
        {"subjects", Array(String())},
        {"hostel", String()},
        {"address", Object(Schema({{"city", String()}, {"pin", Int()}}))},
        {"admitted", DateTime()},
    }));
    StudentModel.deleteMany();

    Student s;
    s.name = "Araham";
    s.roll = 42;
    s.cgpa = 8.7;
    s.subjects = {"DSP", "Embedded"};
    s.address = {"Durgapur", 713209};
    s.admitted = Date::now();

    auto doc = StudentModel.create(toValue(s));  // struct -> Document (validation ke saath)
    s = doc.as<Student>();                        // Document -> struct (ab s.id bhara hua hai)
    std::cout << "id = " << s.id << "\n";

    std::vector<Student> toppers;
    for (auto &v : StudentModel.find().where("cgpa").gte(8).sort("-cgpa").lean()) toppers.push_back(v.as<Student>());
    for (auto &t : toppers)
        std::cout << t.roll << "  " << t.name << "  " << t.cgpa << "  " << t.address.city << "  hostel="
                  << t.hostel.value_or("-") << "\n";

    s.cgpa = 9.1;
    s.hostel = "H-3";
    StudentModel.findById(s.id).orFail().set(toValue(s)).save();  // struct ke badlav wapas DB me
    std::cout << StudentModel.findById(s.id).orFail().json() << "\n";
}
