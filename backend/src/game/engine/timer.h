#pragma once

#include <functional>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace mmcr::game {

class Timer {
public:
    using Callback = std::function<void()>;
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    Timer();

    ~Timer();

    // Start or adjust the timer
    // 'delay_ms' is milliseconds from NOW when the timer should fire
    void set(int delay_ms, Callback callback);
    // Set new expiry time, but only if it's later than the current expiry time
    void set_extend(int delay_ms, Callback callback);
    // Set new expiry time, but only if it's earlier than the current expiry time
    void set_shrink(int delay_ms, Callback callback);

    // Set new expiry time directly
    void setExpiryTime(TimePoint new_time);

    // Get remaining time in milliseconds
    uint64_t remainingMs() const;
    // Check if timer is running
    bool isRunning() const;

    // Stop the timer
    void stop();

private:
    void worker();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    bool running_{false};
    bool shutdown_{false};
    TimePoint expiry_time_;
    Callback callback_;
};

} // namespace mmcr::game