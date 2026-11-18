#include "nevla/formula.hpp"

#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace nevla {
namespace {

/* ---------------- tokens ---------------- */
enum class T { Num, Str, Ident, Op, LParen, RParen, LBrack, RBrack, Comma, End };

struct Tok {
    T type;
    std::string text;
    Value num;
    size_t col;  // 1-based
};

[[noreturn]] void fail(std::string_view src, size_t col, const std::string &msg) {
    std::string out = "formula error at column " + std::to_string(col) + ": " + msg + "\n  " + std::string(src) +
                      "\n  " + std::string(col > 0 ? col - 1 : 0, ' ') + "^";
    throw FormulaError(out, col);
}

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.'; }

std::vector<Tok> tokenize(std::string_view s) {
    std::vector<Tok> out;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        size_t col = i + 1;
        if (std::isspace(static_cast<unsigned char>(c))) {
            i++;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            size_t j = i;
            bool isDouble = false;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) j++;
            if (j < s.size() && s[j] == '.') {
                isDouble = true;
                j++;
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) j++;
            }
            if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                size_t k = j + 1;
                if (k < s.size() && (s[k] == '+' || s[k] == '-')) k++;
                if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
                    isDouble = true;
                    j = k;
                    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) j++;
                }
            }
            if (j < s.size() && identStart(s[j])) fail(s, j + 1, "unexpected character after a number");
            std::string text(s.substr(i, j - i));
            Value v;
            if (isDouble) {
                v = std::stod(text);
            } else {
                try {
                    v = static_cast<int64_t>(std::stoll(text));
                } catch (const std::exception &) {
                    v = std::stod(text);  // too big for int64
                }
            }
            out.push_back({T::Num, text, v, col});
            i = j;
            continue;
        }
        if (c == '\'' || c == '"') {
            std::string text;
            size_t j = i + 1;
            bool closed = false;
            while (j < s.size()) {
                if (s[j] == '\\' && j + 1 < s.size()) {
                    char e = s[j + 1];
                    text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    j += 2;
                    continue;
                }
                if (s[j] == c) {
                    closed = true;
                    break;
                }
                text += s[j++];
            }
            if (!closed) fail(s, col, "text is not closed (missing " + std::string(1, c) + ")");
            out.push_back({T::Str, text, Value(), col});
            i = j + 1;
            continue;
        }
        if (identStart(c)) {
            size_t j = i;
            while (j < s.size() && identChar(s[j])) j++;
            std::string text(s.substr(i, j - i));
            if (text.back() == '.') fail(s, j, "a field path cannot end with '.'");
            out.push_back({T::Ident, text, Value(), col});
            i = j;
            continue;
        }
        auto two = s.substr(i, 2);
        if (two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "&&" || two == "||") {
            out.push_back({T::Op, std::string(two), Value(), col});
            i += 2;
            continue;
        }
        switch (c) {
            case '+':
            case '-':
            case '*':
            case '/':
            case '%':
            case '<':
            case '>':
            case '!':
                out.push_back({T::Op, std::string(1, c), Value(), col});
                break;
            case '=':
                out.push_back({T::Op, "==", Value(), col});  // single '=' is accepted as equality
                break;
            case '(':
                out.push_back({T::LParen, "(", Value(), col});
                break;
            case ')':
                out.push_back({T::RParen, ")", Value(), col});
                break;
            case '[':
                out.push_back({T::LBrack, "[", Value(), col});
                break;
            case ']':
                out.push_back({T::RBrack, "]", Value(), col});
                break;
            case ',':
                out.push_back({T::Comma, ",", Value(), col});
                break;
            default:
                fail(s, col, std::string("unexpected character '") + c + "'");
        }
        i++;
    }
    out.push_back({T::End, "", Value(), s.size() + 1});
    return out;
}

/* ---------------- syntax tree ---------------- */
struct Node {
    enum K { Lit, Field, Arr, Unary, Binary, Call } k;
    Value lit;
    std::string name;  // field path / operator / function
    std::vector<Node> kids;
    size_t col = 0;
};

class Parser {
  public:
    explicit Parser(std::string_view src) : src_(src), toks_(tokenize(src)) {}

    Node parseAll() {
        if (toks_[0].type == T::End) fail(src_, 1, "the formula is empty");
        Node n = orExpr();
        if (peek().type != T::End) fail(src_, peek().col, "unexpected '" + peek().text + "'");
        return n;
    }

  private:
    std::string_view src_;
    std::vector<Tok> toks_;
    size_t p_ = 0;

    const Tok &peek(size_t ahead = 0) const { return toks_[std::min(p_ + ahead, toks_.size() - 1)]; }
    const Tok &next() { return toks_[p_++]; }
    bool isOp(const char *op, size_t ahead = 0) const { return peek(ahead).type == T::Op && peek(ahead).text == op; }
    bool isWord(const char *w, size_t ahead = 0) const { return peek(ahead).type == T::Ident && peek(ahead).text == w; }
    void expect(T type, const char *what) {
        if (peek().type != type)
            fail(src_, peek().col,
                 std::string("expected ") + what +
                     (peek().type == T::End ? " but the formula ended" : ", got '" + peek().text + "'"));
        next();
    }

    static Node bin(const std::string &op, Node a, Node b, size_t col) {
        Node n{Node::Binary, Value(), op, {}, col};
        n.kids.push_back(std::move(a));
        n.kids.push_back(std::move(b));
        return n;
    }

    Node orExpr() {
        Node l = andExpr();
        while (isOp("||") || isWord("or")) {
            size_t col = next().col;
            l = bin("||", std::move(l), andExpr(), col);
        }
        return l;
    }
    Node andExpr() {
        Node l = notExpr();
        while (isOp("&&") || isWord("and")) {
            size_t col = next().col;
            l = bin("&&", std::move(l), notExpr(), col);
        }
        return l;
    }
    Node notExpr() {
        if ((isOp("!") || isWord("not")) && !(isWord("not") && peek(1).type == T::End)) {
            size_t col = next().col;
            Node n{Node::Unary, Value(), "!", {}, col};
            n.kids.push_back(notExpr());
            return n;
        }
        return cmpExpr();
    }
    Node cmpExpr() {
        Node l = addExpr();
        static const std::set<std::string> cmp = {"==", "!=", "<", "<=", ">", ">="};
        if (peek().type == T::Op && cmp.count(peek().text)) {
            const Tok &t = next();
            Node r = addExpr();
            if (peek().type == T::Op && cmp.count(peek().text))
                fail(src_, peek().col, "write 'a < b && b < c' instead of 'a < b < c'");
            return bin(t.text, std::move(l), std::move(r), t.col);
        }
        if (isWord("in") || (isWord("not") && peek(1).type == T::Ident && peek(1).text == "in")) {
            bool neg = isWord("not");
            size_t col = next().col;
            if (neg) next();
            return bin(neg ? "notin" : "in", std::move(l), addExpr(), col);
        }
        return l;
    }
    Node addExpr() {
        Node l = mulExpr();
        while (isOp("+") || isOp("-")) {
            const Tok &t = next();
            l = bin(t.text, std::move(l), mulExpr(), t.col);
        }
        return l;
    }
    Node mulExpr() {
        Node l = unary();
        while (isOp("*") || isOp("/") || isOp("%")) {
            const Tok &t = next();
            l = bin(t.text, std::move(l), unary(), t.col);
        }
        return l;
    }
    Node unary() {
        if (isOp("-") || isOp("+")) {
            const Tok &t = next();
            Node inner = unary();
            if (t.text == "+") return inner;
            if (inner.k == Node::Lit && inner.lit.isNumber()) {  // constant: fold it
                inner.lit = inner.lit.isInt() ? Value(-inner.lit.asInt()) : Value(-inner.lit.asDouble());
                inner.col = t.col;
                return inner;
            }
            Node n{Node::Unary, Value(), "-", {}, t.col};
            n.kids.push_back(std::move(inner));
            return n;
        }
        return primary();
    }
    Node primary() {
        const Tok &t = peek();
        switch (t.type) {
            case T::Num:
                next();
                return Node{Node::Lit, t.num, "", {}, t.col};
            case T::Str:
                next();
                return Node{Node::Lit, Value(t.text), "", {}, t.col};
            case T::LParen: {
                next();
                Node n = orExpr();
                expect(T::RParen, "')'");
                return n;
            }
            case T::LBrack: {
                size_t col = next().col;
                Node n{Node::Arr, Value(), "", {}, col};
                if (peek().type != T::RBrack) {
                    for (;;) {
                        n.kids.push_back(orExpr());
                        if (peek().type == T::Comma) {
                            next();
                            continue;
                        }
                        break;
                    }
                }
                expect(T::RBrack, "']'");
                return n;
            }
            case T::Ident: {
                next();
                if (t.text == "true" || t.text == "false")
                    return Node{Node::Lit, Value(t.text == "true"), "", {}, t.col};
                if (t.text == "null") return Node{Node::Lit, Value(), "", {}, t.col};
                if (peek().type == T::LParen) {
                    next();
                    Node n{Node::Call, Value(), t.text, {}, t.col};
                    if (peek().type != T::RParen) {
                        for (;;) {
                            n.kids.push_back(orExpr());
                            if (peek().type == T::Comma) {
                                next();
                                continue;
                            }
                            break;
                        }
                    }
                    expect(T::RParen, "')' to close " + t.text + "(");
                    return n;
                }
                static const std::set<std::string> reserved = {"and", "or", "not", "in"};
                if (reserved.count(t.text)) fail(src_, t.col, "'" + t.text + "' needs something before it");
                return Node{Node::Field, Value(), t.text[0] == '$' ? t.text.substr(1) : t.text, {}, t.col};
            }
            case T::End:
                fail(src_, t.col, "the formula ended too early");
            default:
                fail(src_, t.col, "unexpected '" + t.text + "'");
        }
    }

    void expect(T type, const std::string &what) { expect(type, what.c_str()); }
};

/* ---------------- to MongoDB expression ---------------- */
struct Fn {
    const char *op;
    int minArgs, maxArgs;  // -1 = any
    enum Shape { Single, List, DatePart, Special } shape;
};

const std::map<std::string, Fn> &functions() {
    static const std::map<std::string, Fn> f = {
        {"abs", {"$abs", 1, 1, Fn::Single}},
        {"ceil", {"$ceil", 1, 1, Fn::Single}},
        {"floor", {"$floor", 1, 1, Fn::Single}},
        {"sqrt", {"$sqrt", 1, 1, Fn::Single}},
        {"exp", {"$exp", 1, 1, Fn::Single}},
        {"ln", {"$ln", 1, 1, Fn::Single}},
        {"log10", {"$log10", 1, 1, Fn::Single}},
        {"trunc", {"$trunc", 1, 2, Fn::List}},
        {"round", {"$round", 1, 2, Fn::List}},
        {"pow", {"$pow", 2, 2, Fn::List}},
        {"log", {"$log", 2, 2, Fn::List}},
        {"min", {"$min", 1, -1, Fn::List}},
        {"max", {"$max", 1, -1, Fn::List}},
        {"sum", {"$sum", 1, -1, Fn::List}},
        {"avg", {"$avg", 1, -1, Fn::List}},
        {"size", {"$size", 1, 1, Fn::Single}},
        {"concat", {"$concat", 1, -1, Fn::List}},
        {"lower", {"$toLower", 1, 1, Fn::Single}},
        {"upper", {"$toUpper", 1, 1, Fn::Single}},
        {"len", {"$strLenCP", 1, 1, Fn::Single}},
        {"substr", {"$substrCP", 3, 3, Fn::List}},
        {"trim", {"$trim", 1, 1, Fn::Special}},
        {"if", {"$cond", 3, 3, Fn::List}},
        {"ifNull", {"$ifNull", 2, 2, Fn::List}},
        {"exists", {"", 1, 1, Fn::Special}},
        {"date", {"", 1, 1, Fn::Special}},
        {"now", {"", 0, 0, Fn::Special}},
        {"dateDiff", {"$dateDiff", 3, 3, Fn::Special}},
        {"toInt", {"$toInt", 1, 1, Fn::Single}},
        {"toDouble", {"$toDouble", 1, 1, Fn::Single}},
        {"toString", {"$toString", 1, 1, Fn::Single}},
        {"toDate", {"$toDate", 1, 1, Fn::Single}},
        {"toBool", {"$toBool", 1, 1, Fn::Single}},
        {"year", {"$year", 1, 2, Fn::DatePart}},
        {"month", {"$month", 1, 2, Fn::DatePart}},
        {"day", {"$dayOfMonth", 1, 2, Fn::DatePart}},
        {"hour", {"$hour", 1, 2, Fn::DatePart}},
        {"minute", {"$minute", 1, 2, Fn::DatePart}},
        {"second", {"$second", 1, 2, Fn::DatePart}},
        {"dayOfWeek", {"$dayOfWeek", 1, 2, Fn::DatePart}},
    };
    return f;
}

std::string functionList() {
    std::string out;
    for (const auto &kv : functions()) out += (out.empty() ? "" : ", ") + kv.first;
    return out;
}

class Compiler {
  public:
    explicit Compiler(std::string_view src) : src_(src) {}

    Value toExpr(const Node &n) {
        switch (n.k) {
            case Node::Lit:
                if (n.lit.isString() && !n.lit.asString().empty() && n.lit.asString()[0] == '$')
                    return Value{{"$literal", n.lit}};  // "$5 off" is text, not a field
                return n.lit;
            case Node::Field:
                return Value("$" + n.name);
            case Node::Arr: {
                Value a = Value::array();
                for (const auto &k : n.kids) a.push_back(toExpr(k));
                return a;
            }
            case Node::Unary:
                if (n.name == "-") return Value{{"$multiply", Value::array({-1, toExpr(n.kids[0])})}};
                return Value{{"$not", Value::array({toExpr(n.kids[0])})}};
            case Node::Binary:
                return binary(n);
            case Node::Call:
                return call(n);
        }
        return Value();
    }

    Value toFilter(const Node &n) {
        if (n.k == Node::Binary && n.name == "&&") {
            std::vector<Value> parts;
            collect(n, "&&", parts);
            return mergeAnd(parts);
        }
        if (n.k == Node::Binary && n.name == "||") {
            std::vector<Value> parts;
            collect(n, "||", parts);
            Value arr = Value::array();
            for (auto &p : parts) arr.push_back(std::move(p));
            return Value{{"$or", arr}};
        }
        if (n.k == Node::Unary && n.name == "!") {
            const Node &inner = n.kids[0];
            if (inner.k == Node::Field) return Value{{inner.name, Value{{"$ne", true}}}};
            return Value{{"$nor", Value::array({toFilter(inner)})}};
        }
        if (n.k == Node::Field) return Value{{n.name, true}};  // "active" -> {active: true}
        if (n.k == Node::Call && n.name == "exists") {
            checkArgs(n);
            if (n.kids[0].k != Node::Field) fail(src_, n.kids[0].col, "exists() needs a field name");
            return Value{{n.kids[0].name, Value{{"$exists", true}}}};
        }
        if (n.k == Node::Binary) {
            static const std::map<std::string, std::string> ops = {{"==", ""},     {"!=", "$ne"}, {"<", "$lt"},
                                                                   {"<=", "$lte"}, {">", "$gt"},  {">=", "$gte"}};
            static const std::map<std::string, std::string> flipped = {{"<", ">"},   {"<=", ">="}, {">", "<"},
                                                                       {">=", "<="}, {"==", "=="}, {"!=", "!="}};
            auto it = ops.find(n.name);
            if (it != ops.end()) {
                const Node *field = nullptr, *other = nullptr;
                std::string op = n.name;
                if (n.kids[0].k == Node::Field && isConstant(n.kids[1])) {
                    field = &n.kids[0];
                    other = &n.kids[1];
                } else if (n.kids[1].k == Node::Field && isConstant(n.kids[0])) {
                    field = &n.kids[1];
                    other = &n.kids[0];
                    op = flipped.at(op);
                }
                if (field) {
                    Value v = constant(*other);
                    const std::string &mop = ops.at(op);
                    if (mop.empty()) return Value{{field->name, v}};
                    return Value{{field->name, Value{{mop, v}}}};
                }
            }
            if ((n.name == "in" || n.name == "notin") && n.kids[0].k == Node::Field && n.kids[1].k == Node::Arr &&
                isConstant(n.kids[1]))
                return Value{{n.kids[0].name, Value{{n.name == "in" ? "$in" : "$nin", constant(n.kids[1])}}}};
        }
        return Value{{"$expr", toExpr(n)}};
    }

  private:
    std::string_view src_;

    static void collect(const Node &n, const std::string &op, std::vector<Value> &out, Compiler *self) {
        if (n.k == Node::Binary && n.name == op) {
            collect(n.kids[0], op, out, self);
            collect(n.kids[1], op, out, self);
        } else {
            out.push_back(self->toFilter(n));
        }
    }
    void collect(const Node &n, const std::string &op, std::vector<Value> &out) { collect(n, op, out, this); }

    // {a: {$gt: 1}} + {a: {$lt: 5}} -> {a: {$gt: 1, $lt: 5}};  conflicts -> {$and: [...]}
    static Value mergeAnd(std::vector<Value> &parts) {
        Value merged = Value::object();
        for (const auto &p : parts) {
            for (const auto &kv : p.items()) {
                Value *cur = merged.find(kv.first);
                if (!cur) {
                    merged[kv.first] = kv.second;
                    continue;
                }
                bool opsA = isOps(*cur), opsB = isOps(kv.second);
                bool clash = !opsA || !opsB || kv.first[0] == '$';  // two $expr / $or ... -> $and
                if (!clash)
                    for (const auto &o : kv.second.items()) clash |= cur->contains(o.first);
                if (clash) {
                    Value arr = Value::array();
                    for (auto &q : parts) arr.push_back(std::move(q));
                    return Value{{"$and", arr}};
                }
                for (const auto &o : kv.second.items()) (*cur)[o.first] = o.second;
            }
        }
        return merged;
    }
    static bool isOps(const Value &v) {
        if (!v.isObject() || v.empty()) return false;
        for (const auto &kv : v.items())
            if (kv.first.empty() || kv.first[0] != '$') return false;
        return true;
    }

    bool isConstant(const Node &n) const {
        if (n.k == Node::Lit) return true;
        if (n.k == Node::Arr) {
            for (const auto &k : n.kids)
                if (!isConstant(k)) return false;
            return true;
        }
        return n.k == Node::Call && n.name == "date";
    }
    Value constant(const Node &n) {
        if (n.k == Node::Lit) return n.lit;
        if (n.k == Node::Arr) {
            Value a = Value::array();
            for (const auto &k : n.kids) a.push_back(constant(k));
            return a;
        }
        return call(n);  // date('...')
    }

    Value list(const Node &n) {
        Value a = Value::array();
        for (const auto &k : n.kids) a.push_back(toExpr(k));
        return a;
    }

    Value binary(const Node &n) {
        static const std::map<std::string, std::string> ops = {
            {"+", "$add"},  {"-", "$subtract"}, {"*", "$multiply"}, {"/", "$divide"}, {"%", "$mod"},
            {"==", "$eq"},  {"!=", "$ne"},      {"<", "$lt"},       {"<=", "$lte"},   {">", "$gt"},
            {">=", "$gte"}, {"&&", "$and"},     {"||", "$or"}};
        if (n.name == "in" || n.name == "notin") {
            Value in{{"$in", Value::array({toExpr(n.kids[0]), toExpr(n.kids[1])})}};
            return n.name == "in" ? in : Value{{"$not", Value::array({in})}};
        }
        const std::string &op = ops.at(n.name);
        // a + b + c -> {$add: [a, b, c]}  (same for *, &&, ||)
        if (n.name == "+" || n.name == "*" || n.name == "&&" || n.name == "||") {
            Value args = Value::array();
            flatten(n, n.name, args);
            return Value{{op, args}};
        }
        return Value{{op, Value::array({toExpr(n.kids[0]), toExpr(n.kids[1])})}};
    }
    void flatten(const Node &n, const std::string &op, Value &args) {
        if (n.k == Node::Binary && n.name == op) {
            flatten(n.kids[0], op, args);
            flatten(n.kids[1], op, args);
        } else {
            args.push_back(toExpr(n));
        }
    }

    const Fn &checkArgs(const Node &n) {
        auto it = functions().find(n.name);
        if (it == functions().end())
            fail(src_, n.col, "unknown function '" + n.name + "'. Available: " + functionList());
        const Fn &f = it->second;
        int argc = static_cast<int>(n.kids.size());
        if (argc < f.minArgs || (f.maxArgs >= 0 && argc > f.maxArgs)) {
            std::string want = f.minArgs == f.maxArgs ? std::to_string(f.minArgs)
                               : f.maxArgs < 0        ? "at least " + std::to_string(f.minArgs)
                                                      : std::to_string(f.minArgs) + " or " + std::to_string(f.maxArgs);
            fail(src_, n.col, n.name + "() takes " + want + " argument(s), got " + std::to_string(argc));
        }
        return f;
    }

    Value call(const Node &n) {
        const Fn &f = checkArgs(n);
        if (n.name == "date") {
            if (n.kids[0].k != Node::Lit || !n.kids[0].lit.isString())
                fail(src_, n.kids[0].col,
                     "date() needs text like date('2026-11-17') or date('2026-11-17T10:30:00+05:30')");
            auto d = Date::parse(n.kids[0].lit.asString());
            if (!d) fail(src_, n.kids[0].col, "'" + n.kids[0].lit.asString() + "' is not a valid date");
            return Value(*d);
        }
        if (n.name == "now") return Value("$$NOW");
        if (n.name == "exists") {
            if (n.kids[0].k != Node::Field) fail(src_, n.kids[0].col, "exists() needs a field name");
            return Value{{"$ne", Value::array({Value{{"$type", toExpr(n.kids[0])}}, "missing"})}};
        }
        if (n.name == "trim") return Value{{"$trim", Value{{"input", toExpr(n.kids[0])}}}};
        if (n.name == "dateDiff") {
            if (n.kids[2].k != Node::Lit || !n.kids[2].lit.isString())
                fail(src_, n.kids[2].col, "dateDiff() unit must be text: 'day', 'hour', 'minute', ...");
            return Value{
                {"$dateDiff",
                 Value{{"startDate", toExpr(n.kids[0])}, {"endDate", toExpr(n.kids[1])}, {"unit", n.kids[2].lit}}}};
        }
        switch (f.shape) {
            case Fn::Single:
                return Value{{f.op, toExpr(n.kids[0])}};
            case Fn::DatePart: {
                Value spec{{"date", toExpr(n.kids[0])}};
                if (n.kids.size() == 2) spec["timezone"] = toExpr(n.kids[1]);
                return Value{{f.op, spec}};
            }
            case Fn::List:
                if (n.kids.size() == 1 && (n.name == "min" || n.name == "max" || n.name == "sum" || n.name == "avg"))
                    return Value{{f.op, toExpr(n.kids[0])}};  // min(array field)
                return Value{{f.op, list(n)}};
            default:
                return Value();
        }
    }
};

}  // namespace

Value expr(std::string_view formula) {
    Parser p(formula);
    Node n = p.parseAll();
    return Compiler(formula).toExpr(n);
}

Value formulaFilter(std::string_view formula) {
    Parser p(formula);
    Node n = p.parseAll();
    return Compiler(formula).toFilter(n);
}

bool detail::isFieldPath(std::string_view s) {
    if (s.empty()) return false;
    size_t i = s[0] == '$' ? 1 : 0;
    if (i >= s.size() || !(std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_')) return false;
    for (; i < s.size(); i++)
        if (!(std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '.')) return false;
    return s.back() != '.';
}

}  // namespace nevla
