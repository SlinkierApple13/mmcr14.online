#pragma once

#include <deque>
#include <shared_mutex>

namespace mmcr::util {

template <typename T>
class MyQueue {
public:
    void push_back(T item) {
        std::unique_lock lock(mutex_);
        queue_.push_back(std::move(item));
    }
    void push_front(T item) {
        std::unique_lock lock(mutex_);
        queue_.push_front(std::move(item));
    }
    void pop_back() {
        std::unique_lock lock(mutex_);
        queue_.pop_back();
    }
    void pop_front() {
        std::unique_lock lock(mutex_);
        queue_.pop_front();
    }
    std::vector<T> get_all() {
        std::shared_lock lock(mutex_);
        return {queue_.begin(), queue_.end()};
    }
    T poll_back() {
        std::unique_lock lock(mutex_);
        T item = std::move(queue_.back());
        queue_.pop_back();
        return item;
    }
    T poll_front() {
        std::unique_lock lock(mutex_);
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }
    T& back() {
        std::shared_lock lock(mutex_);
        return queue_.back();
    }
    T& front() {
        std::shared_lock lock(mutex_);
        return queue_.front();
    }
    void clear() {
        std::unique_lock lock(mutex_);
        queue_.clear();
    }
    bool empty() const {
        std::shared_lock lock(mutex_);
        return queue_.empty();
    }
    size_t size() const {
        std::shared_lock lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::shared_mutex mutex_;
    std::deque<T> queue_;
};

} // namespace mmcr::util