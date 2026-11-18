// populate (ref), hooks, virtuals, methods, toJSON transform, query builder
#include <nevla/nevla.hpp>

#include <iostream>

using namespace nevla;

int main() {
    connect("mongodb://127.0.0.1:27017", "nevla_demo");

    Schema authorSchema({
        {"first", String().required()},
        {"last", String()},
        {"password", String().select(false)},  // find() me nahi aata
    }, {.virtuals = true});
    authorSchema.virtual_("fullName", [](const Document &d) {
        return Value(d["first"].asString() + " " + d["last"].asString());
    });
    authorSchema.pre("save", [](Document &d) {
        if (d.isModified("password")) d.set("password", "hashed(" + d["password"].asString() + ")");
    });
    authorSchema.toJSON([](const Document &, Value &out) { out.erase("password"); });

    Schema postSchema({
        {"title", String().required().minLength(3)},
        {"author", Id().ref("Author")},
        {"tags", Array(String().lowercase())},
        {"comments", Array(Schema({{"by", Id().ref("Author")}, {"text", String().required()}}))},
        {"views", Int().def(0)},
        {"published", Boolean().def(false)},
    }, {.timestamps = true});
    postSchema.index({{"author", 1}, {"createdAt", -1}});
    postSchema.method("publish", [](Document &d, const Value &) {
        d.set("published", true).save();
        return Value(true);
    });

    auto Author = model("Author", authorSchema);
    auto Post = model("Post", postSchema);
    Author.deleteMany();
    Post.deleteMany();

    auto prem = Author.create({{"first", "Munshi"}, {"last", "Premchand"}, {"password", "1234"}});
    auto ravi = Author.create({{"first", "Ravi"}, {"last", "Kumar"}});
    std::cout << "virtual: " << prem["fullName"].asString() << "\n";
    std::cout << "toJSON (password gayab): " << prem.json() << "\n";

    auto post = Post.create({
        {"title", "Godaan"},
        {"author", prem._id()},
        {"tags", Value::array({"Hindi", "Classic"})},
        {"comments", Value::array({{{"by", ravi._id()}, {"text", "Bahut badhiya"}}})},
    });
    post.call("publish");
    post.push("comments", {{"by", prem._id()}, {"text", "Dhanyavaad"}});
    post.save();
    Post.updateOne({{"_id", post._id()}}, {{"$inc", {{"views", 1}}}});

    auto posts = Post.find({{"published", true}})
                     .where("views").gte(1)
                     .sort("-createdAt")
                     .limit(10)
                     .populate("author")
                     .populate("comments.by")
                     .exec();
    for (auto &p : posts) {
        std::cout << p["title"].asString() << " - " << p["author.first"].asString() << " " << p["author.last"].asString() << "\n";
        for (const auto &c : p["comments"].elements())
            std::cout << "   " << c["by"]["first"].asString() << ": " << c["text"].asString() << "\n";
    }
}
