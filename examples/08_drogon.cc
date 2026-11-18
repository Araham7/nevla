/* nevla inside a Drogon app (built only when Drogon is found).

   - drogon::Task<> handlers co_await nevla directly (no adapter)
   - nevla::useDrogonLoop(): after every co_await the handler is back on its own IO loop
   - ValidationError -> 400 with {field: message}
   - runs on memory:// by default (no MongoDB needed); pass a URI to use a real server

   Self-check mode (used by CTest):  08_drogon --check [uri]
   starts the server, fires many concurrent requests at it, verifies every handler resumed on its IO
   loop thread and that the responses are right, then exits 0 / 1.                                   */
#include <drogon/drogon.h>
#include <nevla/drogon.hpp>
#include <nevla/nevla.hpp>

#include <atomic>
#include <iostream>
#include <thread>

using namespace drogon;

static nevla::Model Users() { return nevla::model("User"); }
static std::atomic<int> gWrongThread{0}, gHandled{0};

// the thread check: are we on an IO loop thread, and the same one we started on?
static void checkLoop(trantor::EventLoop *start) {
    if (trantor::EventLoop::getEventLoopOfCurrentThread() != start) gWrongThread++;
}

static HttpResponsePtr json(HttpStatusCode code, const nevla::Value &v) {
    auto r = HttpResponse::newHttpResponse();
    r->setStatusCode(code);
    r->setContentTypeCode(CT_APPLICATION_JSON);
    r->setBody(v.toJson());
    return r;
}

// POST /users  {"name": "...", "age": 30}
static Task<HttpResponsePtr> createUser(HttpRequestPtr req) {
    auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    try {
        nevla::Value body = nevla::Value::parse(std::string(req->body()));
        auto doc = co_await Users().createAsync(body);
        checkLoop(loop);
        gHandled++;
        co_return json(k201Created, doc.toJSON());
    } catch (const nevla::ValidationError &e) {
        checkLoop(loop);
        gHandled++;
        co_return json(k400BadRequest, nevla::obj("errors", e.toJSON()));
    }
}

// GET /users/count?minAge=18
static Task<HttpResponsePtr> countUsers(HttpRequestPtr req) {
    auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    int minAge = std::atoi(req->getParameter("minAge").c_str());
    int64_t n = co_await Users().find(nevla::obj("age", nevla::obj("$gte", minAge))).countAsync();
    checkLoop(loop);
    auto rows = co_await Users().find(nevla::obj("age", nevla::obj("$gte", minAge))).limit(5).leanAsync();
    checkLoop(loop);
    gHandled++;
    co_return json(k200OK, nevla::obj("count", n, "sample", static_cast<int64_t>(rows.size())));
}

static int selfCheck(int port) {
    const int N = 400;
    std::atomic<int> ok{0}, bad{0}, done{0};
    auto client = HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(port));
    for (int i = 0; i < N; i++) {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/users");
        req->setContentTypeCode(CT_APPLICATION_JSON);
        bool invalid = i % 8 == 0;  // no name -> 400
        req->setBody(invalid ? nevla::obj("age", i).toJson() : nevla::obj("name", "u" + std::to_string(i), "age", i % 60).toJson());
        client->sendRequest(req, [&, invalid](ReqResult r, const HttpResponsePtr &resp) {
            bool good = r == ReqResult::Ok && resp->statusCode() == (invalid ? k400BadRequest : k201Created);
            if (good && invalid) good = nevla::Value::parse(std::string(resp->body()))["errors"].contains("name");
            (good ? ok : bad)++;
            done++;
        });
    }
    for (int t = 0; t < 600 && done < N; t++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int expected = 0;
    for (int i = 0; i < N; i++)
        if (i % 8 && i % 60 >= 18) expected++;
    std::atomic<int> cdone{0};
    int64_t got = -1;
    auto creq = HttpRequest::newHttpRequest();
    creq->setPath("/users/count");
    creq->setParameter("minAge", "18");
    client->sendRequest(creq, [&](ReqResult r, const HttpResponsePtr &resp) {
        if (r == ReqResult::Ok) got = nevla::Value::parse(std::string(resp->body()))["count"].asInt();
        cdone++;
    });
    for (int t = 0; t < 200 && !cdone; t++) std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "requests ok: " << ok << "/" << N << ", bad: " << bad << ", handled: " << gHandled
              << ", resumed on wrong thread: " << gWrongThread << ", count: " << got << " (expected " << expected << ")\n";
    return ok == N && bad == 0 && gWrongThread == 0 && got == expected ? 0 : 1;
}

int main(int argc, char **argv) {
    bool check = argc > 1 && std::string(argv[1]) == "--check";
    std::string uri = argc > (check ? 2 : 1) ? argv[check ? 2 : 1] : "memory://drogon_demo";
    int port = check ? 18765 : 8080;

    if (!std::getenv("NEVLA_NO_LOOP")) nevla::useDrogonLoop();  // NEVLA_NO_LOOP=1: see the problem it fixes
    nevla::connect(uri, "nevla_drogon");
    nevla::model("User", nevla::Schema({{"name", nevla::String().required()}, {"age", nevla::Int().min(0)}}));
    Users().deleteMany(nevla::Value::object());

    app().registerHandler("/users", &createUser, {Post});
    app().registerHandler("/users/count", &countUsers, {Get});
    app().addListener("127.0.0.1", port).setThreadNum(4);

    int rc = 0;
    std::thread checker;
    if (check) {
        checker = std::thread([&] {
            while (!app().isRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            rc = selfCheck(port);
            app().quit();
        });
    } else {
        std::cout << "listening on http://127.0.0.1:" << port << " (" << uri << ")\n";
    }
    app().run();
    if (checker.joinable()) checker.join();
    nevla::disconnect();
    return rc;
}
