#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace tcp {

class ThreadPool {
   public:
    explicit ThreadPool(size_t num_threads) {
        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool &)            = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_)
            if (t.joinable())
                t.join();
        workers_.clear();
    }

   private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty())
                    return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_{false};
};

}  // namespace tcp
