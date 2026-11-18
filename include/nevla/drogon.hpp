#pragma once
/* Drogon glue (header-only; include it only in a Drogon app).

     #include <nevla/drogon.hpp>
     int main() {
         nevla::useDrogonLoop();                         // co_await continues on the same IO loop
         drogon::app().registerBeginningAdvice([] { nevla::connect(getenv("MONGODB_URI")); });
         ...
     }

     drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req) {
         auto users = co_await User.find("active == true").leanAsync();   // pool does the I/O,
         ...                                                             // we are back on our loop
     }

   Without useDrogonLoop() the handler continues on a nevla pool thread after a co_await, which is not
   the thread Drogon expects for the rest of the handler.                                             */

#include <trantor/net/EventLoop.h>

#include "async.hpp"

namespace nevla {

inline void useDrogonLoop() {
    setResumeOn([]() -> Resumer {
        trantor::EventLoop *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        if (!loop) return {};  // not on an IO loop (main, a worker thread): continue on the pool
        return [loop](std::function<void()> f) { loop->queueInLoop(std::move(f)); };
    });
}

}  // namespace nevla
