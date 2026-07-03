#include "game/engine/timer.h"

#include <utility>

namespace mmcr::game {

Timer::Timer()
    : worker_thread_(&Timer::worker, this) {}

Timer::~Timer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        running_ = false;
    }
    cv_.notify_one();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void Timer::set(int delay_ms, Callback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
    expiry_time_ = Clock::now() + std::chrono::milliseconds(delay_ms);
    running_ = true;
    cv_.notify_one();
}

void Timer::set_extend(int delay_ms, Callback callback) {
    auto new_expiry = Clock::now() + std::chrono::milliseconds(delay_ms);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || new_expiry > expiry_time_) {
        expiry_time_ = new_expiry;
        callback_ = std::move(callback);
        running_ = true;
        cv_.notify_one();
    }
}

void Timer::set_shrink(int delay_ms, Callback callback) {
    auto new_expiry = Clock::now() + std::chrono::milliseconds(delay_ms);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || new_expiry < expiry_time_) {
        expiry_time_ = new_expiry;
        callback_ = std::move(callback);
        running_ = true;
        cv_.notify_one();
    }
}

void Timer::setExpiryTime(TimePoint new_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    expiry_time_ = new_time;
    running_ = true;
    cv_.notify_one();
}

uint64_t Timer::remainingMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return 0;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        expiry_time_ - Clock::now());
    return remaining.count() > 0 ? static_cast<uint64_t>(remaining.count()) : 0;
}

bool Timer::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void Timer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }

    running_ = false;
    callback_ = nullptr;
    cv_.notify_one();
}

void Timer::worker() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (!shutdown_) {
        cv_.wait(lock, [this] {
            return shutdown_ || running_;
        });

        if (shutdown_) {
            return;
        }

        while (running_ && !shutdown_) {
            const auto current_expiry = expiry_time_;
            if (cv_.wait_until(lock, current_expiry, [this, current_expiry] {
                    return shutdown_ || !running_ || expiry_time_ != current_expiry;
                })) {
                continue;
            }

            auto callback = std::move(callback_);
            running_ = false;
            callback_ = nullptr;

            lock.unlock();
            if (callback) {
                callback();
            }
            lock.lock();
        }
    }
}

} // namespace mmcr::game