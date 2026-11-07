#ifndef THREADPOOL_HPP
#define THREADPOOL_HPP

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool {
   public:
    // Constructor: Creates a pool with the specified number of threads
    explicit ThreadPool(size_t numThreads);

    // Destructor: Joins all threads
    ~ThreadPool();

    // Enqueue a task into the thread pool; returns a future to retrieve the
    // result

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [f = std::forward<F>(f),
             args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(
                    [&f](auto&&... args) -> decltype(auto) {
                        return std::invoke(
                            f, std::forward<decltype(args)>(args)...);
                    },
                    std::move(args));
            });

        auto result = task->get_future();

        {
            std::unique_lock lock(queueMutex);

            if (stop) {
                throw std::runtime_error(
                    "ThreadPool is stopped; cannot enqueue tasks.");
            }

            tasks.emplace([task] { (*task)(); });
        }

        condition.notify_one();
        return result;
    }

   private:
    // Workers
    std::vector<std::thread> workers;

    // Task queue
    std::queue<std::function<void()>> tasks;

    // Synchronization
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
};

// Implementation
//

#endif  // THREADPOOL_HPP
