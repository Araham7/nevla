#pragma once
/* co_await support (C++20 coroutines).

   Every *Async() method returns an Async<T>: the database work runs on nevla's thread pool and the coroutine
   continues when the result is there, so the calling thread (e.g. an event loop) is never blocked.

     nevla::Task<int64_t> countAdults() {
         auto n = co_await User.find("age >= 18").countAsync();
         co_return n;
     }
     int64_t n = countAdults().get();         // run + wait (outside any coroutine)

   Any coroutine type can co_await an Async<T> (await_suspend takes a plain std::coroutine_handle<>),
   so it works inside drogon::Task<>, cppcoro, asio awaitables, or nevla::Task.

   Where the coroutine continues: by default on the nevla pool thread that did the work. Install a
   resumer to come back on the thread that awaited (an event loop):
       nevla::setResumeOn([]() -> nevla::Resumer { ... });   // see below
   With Drogon just #include <nevla/drogon.hpp> and call nevla::useDrogonLoop() once at startup:
   every co_await then continues on the same Drogon IO loop that started it.

   Thread-local state travels with the job: nevla::TenantScope / ActorScope values are visible to it.
   A nevla::transaction() does NOT span *Async() calls (a transaction belongs to one thread).          */

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace nevla {

namespace detail {
// Thread-local state that follows a job onto the pool thread
struct Context {
    std::string tenant;
    bool hasTenant = false, allTenants = false;
    std::string actor;
};
Context &context();  // this thread's
Context captureContext();
void restoreContext(const Context &c);
// Run a job on nevla's async thread pool
void runAsync(std::function<void()> job);
}  // namespace detail

// Number of pool threads (default: max(4, hardware threads)). Call before the first *Async() call.
void setAsyncThreads(unsigned n);

/* Resume control. A Resumer takes "resume the coroutine" and runs it somewhere (e.g. queues it on an
   event loop). The capture function is called on the AWAITING thread when a co_await suspends; it
   returns the Resumer for that thread, or an empty one to continue on the pool thread.
       nevla::setResumeOn([]() -> nevla::Resumer {
           auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
           if (!loop) return {};                          // not on a loop: continue on the pool
           return [loop](std::function<void()> f) { loop->queueInLoop(std::move(f)); };
       });
   Pass {} to go back to the default. Set it once at startup (it is not meant to change while
   operations are in flight).                                                                       */
using Resumer = std::function<void(std::function<void()>)>;
void setResumeOn(std::function<Resumer()> capture);
namespace detail {
Resumer captureResumer();  // the Resumer for the current thread (empty = resume on the pool)
}

// An awaitable database operation. co_await it inside a coroutine, or .get() to run it here and now.
template <class T>
class [[nodiscard]] Async {
  public:
    explicit Async(std::function<T()> fn) : fn_(std::move(fn)) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        auto ctx = detail::captureContext();
        auto post = detail::captureResumer();
        detail::runAsync([this, h, ctx, post = std::move(post)] {
            detail::restoreContext(ctx);
            try {
                result_.emplace(fn_());
            } catch (...) {
                err_ = std::current_exception();
            }
            detail::restoreContext({});
            if (post) post([h] { h.resume(); });  // back on the awaiting thread's loop
            else h.resume();                       // `this` may be gone after this line
        });
    }
    T await_resume() {
        if (err_) std::rethrow_exception(err_);
        return std::move(*result_);
    }
    T get() { return fn_(); }

  private:
    std::function<T()> fn_;
    std::optional<T> result_;
    std::exception_ptr err_;
};

template <>
class [[nodiscard]] Async<void> {
  public:
    explicit Async(std::function<void()> fn) : fn_(std::move(fn)) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        auto ctx = detail::captureContext();
        auto post = detail::captureResumer();
        detail::runAsync([this, h, ctx, post = std::move(post)] {
            detail::restoreContext(ctx);
            try {
                fn_();
            } catch (...) {
                err_ = std::current_exception();
            }
            detail::restoreContext({});
            if (post) post([h] { h.resume(); });
            else h.resume();
        });
    }
    void await_resume() {
        if (err_) std::rethrow_exception(err_);
    }
    void get() { fn_(); }

  private:
    std::function<void()> fn_;
    std::exception_ptr err_;
};

// Wrap any blocking work:  co_await nevla::asyncCall([&] { return doSomething(); });
template <class F>
auto asyncCall(F fn) -> Async<decltype(fn())> {
    return Async<decltype(fn())>(std::function<decltype(fn())()>(std::move(fn)));
}

/* ---------------- Task<T>: a simple coroutine type (if your framework has none) ---------------- */
template <class T>
class Task;

namespace detail {
struct WaitState {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
};

struct SyncWaiter {
    struct promise_type {
        std::shared_ptr<WaitState> state;
        SyncWaiter get_return_object() { return SyncWaiter{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        auto final_suspend() noexcept {
            struct Signal {
                bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    auto s = h.promise().state;  // keep the state alive while notifying
                    {
                        std::lock_guard<std::mutex> lk(s->mu);
                        s->done = true;
                    }
                    s->cv.notify_all();
                }
                void await_resume() noexcept {}
            };
            return Signal{};
        }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }  // never: the body catches everything
    };
    std::coroutine_handle<promise_type> h;
};
}  // namespace detail

template <class T = void>
class [[nodiscard]] Task {
  public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr err;
        std::coroutine_handle<> continuation;
        Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        auto final_suspend() noexcept {
            struct Next {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    auto c = h.promise().continuation;
                    return c ? c : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };
            return Next{};
        }
        template <class U>
        void return_value(U &&v) {
            value.emplace(std::forward<U>(v));
        }
        void unhandled_exception() { err = std::current_exception(); }
    };

    Task(Task &&o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task &operator=(Task &&o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    Task(const Task &) = delete;
    ~Task() {
        if (h_) h_.destroy();
    }

    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
        h_.promise().continuation = c;
        return h_;
    }
    T await_resume() {
        if (h_.promise().err) std::rethrow_exception(h_.promise().err);
        return std::move(*h_.promise().value);
    }

    // Run the task and block until it finishes (use outside coroutines, e.g. in main or a test)
    T get() {
        auto waiter = wait(*this);
        auto state = std::make_shared<detail::WaitState>();
        waiter.h.promise().state = state;
        waiter.h.resume();
        {
            std::unique_lock<std::mutex> lk(state->mu);
            state->cv.wait(lk, [&] { return state->done; });
        }
        waiter.h.destroy();
        return await_resume();
    }

  private:
    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    // Waits for the task without taking its result (get() takes it afterwards)
    struct Done {
        std::coroutine_handle<promise_type> h;
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
            h.promise().continuation = c;
            return h;
        }
        void await_resume() noexcept {}
    };
    static detail::SyncWaiter wait(Task &t) { co_await Done{t.h_}; }
    std::coroutine_handle<promise_type> h_;
};

template <>
class [[nodiscard]] Task<void> {
  public:
    struct promise_type {
        std::exception_ptr err;
        std::coroutine_handle<> continuation;
        Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        auto final_suspend() noexcept {
            struct Next {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    auto c = h.promise().continuation;
                    return c ? c : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };
            return Next{};
        }
        void return_void() {}
        void unhandled_exception() { err = std::current_exception(); }
    };

    Task(Task &&o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task &operator=(Task &&o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    Task(const Task &) = delete;
    ~Task() {
        if (h_) h_.destroy();
    }

    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
        h_.promise().continuation = c;
        return h_;
    }
    void await_resume() {
        if (h_.promise().err) std::rethrow_exception(h_.promise().err);
    }
    void get() {
        auto waiter = wait(*this);
        auto state = std::make_shared<detail::WaitState>();
        waiter.h.promise().state = state;
        waiter.h.resume();
        {
            std::unique_lock<std::mutex> lk(state->mu);
            state->cv.wait(lk, [&] { return state->done; });
        }
        waiter.h.destroy();
        await_resume();
    }

  private:
    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    // Waits for the task without taking its result (get() takes it afterwards)
    struct Done {
        std::coroutine_handle<promise_type> h;
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
            h.promise().continuation = c;
            return h;
        }
        void await_resume() noexcept {}
    };
    static detail::SyncWaiter wait(Task &t) { co_await Done{t.h_}; }
    std::coroutine_handle<promise_type> h_;
};

}  // namespace nevla
