#include "thread_pool.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

TEST(ThreadPool, SubmitAndExecute) {
    tcp::ThreadPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i)
        pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    pool.shutdown();
    EXPECT_EQ(counter.load(), 100);
}

TEST(ThreadPool, SingleThread) {
    tcp::ThreadPool pool(1);
    std::atomic<int> counter{0};
    for (int i = 0; i < 50; ++i)
        pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    pool.shutdown();
    EXPECT_EQ(counter.load(), 50);
}

TEST(ThreadPool, ZeroThreads) {
    tcp::ThreadPool pool(0);
    pool.shutdown();  // must not crash
}

TEST(ThreadPool, ConcurrentSubmits) {
    tcp::ThreadPool pool(8);
    std::atomic<int> counter{0};
    std::vector<std::thread> submitters;
    for (int i = 0; i < 8; ++i) {
        submitters.emplace_back([&pool, &counter] {
            for (int j = 0; j < 1000; ++j)
                pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        });
    }
    for (auto &t : submitters)
        t.join();
    pool.shutdown();
    EXPECT_EQ(counter.load(), 8000);
}

TEST(ThreadPool, ShutdownIdempotent) {
    tcp::ThreadPool pool(2);
    pool.shutdown();
    pool.shutdown();
}

TEST(ThreadPool, TasksCompleteBeforeShutdown) {
    tcp::ThreadPool pool(2);
    std::atomic<int> counter{0};
    for (int i = 0; i < 1000; ++i)
        pool.submit([&counter] {
            std::this_thread::yield();
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    pool.shutdown();
    EXPECT_EQ(counter.load(), 1000);
}
