#include "nevla/async.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace nevla {
namespace {

thread_local detail::Context tContext;

class Pool {
  public:
    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : threads_)
            if (t.joinable()) t.join();
    }
    void setSize(unsigned n) {
        std::lock_guard<std::mutex> lk(mu_);
        if (threads_.empty()) size_ = std::max(1u, n);
    }
    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (threads_.empty()) start();
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

  private:
    void start() {  // under mu_
        unsigned n = size_ ? size_ : std::max(4u, std::thread::hardware_concurrency());
        for (unsigned i = 0; i < n; i++) threads_.emplace_back([this] { loop(); });
    }
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
                if (jobs_.empty()) return;  // stopping and nothing left
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();  // jobs catch their own exceptions
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    unsigned size_ = 0;
    bool stop_ = false;
};

Pool &pool() {
    static Pool p;
    return p;
}

}  // namespace

detail::Context &detail::context() { return tContext; }
detail::Context detail::captureContext() { return tContext; }
void detail::restoreContext(const Context &c) { tContext = c; }
void detail::runAsync(std::function<void()> job) { pool().submit(std::move(job)); }
void setAsyncThreads(unsigned n) { pool().setSize(n); }

}  // namespace nevla
