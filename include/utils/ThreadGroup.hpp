#pragma once

#include <thread>
#include <vector>
#include <utility>


/**
 * @brief Thread schedulation of ease of use
 * 
 * Threads take a lambda function or a function type, and can be scheduled in groups
 * 
 * @note when executing a join, that will be executed on all threads of the group.
 * @note when scheduling a thread it's possible to pass a variable number of parameters 
 */
class ThreadGroup {
public:
    ThreadGroup() = default;

    ThreadGroup(const ThreadGroup&) = delete;

    ThreadGroup(ThreadGroup&&) = default;

    ThreadGroup& operator=(const ThreadGroup&) = delete;

    ThreadGroup& operator=(ThreadGroup&&) = default;

    /**
     * @brief Creates a new thread and appends it to the group
     * 
     * @param func (T) function to execute
     * @param args (Args&&) arguments to pass to the function
     * 
     * @note the `tid` parameter is passed to the function as last parameter
     */
    template<class T, class... Args>
    void thread(T func, Args&&... args) {
        int tid = static_cast<int>(threads.size());
        threads.emplace_back(std::move(func), std::forward<Args>(args)..., tid);
    }

    /**
     * @brief Creates a new thread and appends it to the group
     * 
     * @param func (T) function to execute
     * @param result (R&) reference to store the result
     * @param args (Args&&) arguments to pass to the function
     * 
     * @note the `tid` parameter is passed to the function as last parameter
     */
    template <class T, class R, class... Args>
    void threadWithResult(T func, R& result, Args&&... args) {
        threads.emplace_back(
            [func = std::move(func), &result, ... capturedArgs = std::forward<Args>(args)](int tid) {
                result = func(capturedArgs..., tid);
            },
            static_cast<int>(threads.size()));
    }

    /**
     * @brief Joins all threads in the group
     */
    void join() {
        for (std::thread& thread: threads) {
            thread.join();
        }
        threads.clear();
    }

    /**
     * @brief Destructor for the ThreadGroup
     * 
     * Joins all threads in the group
     */
    ~ThreadGroup() noexcept {
        join();
    }

private:
    std::vector<std::thread> threads{};
};
