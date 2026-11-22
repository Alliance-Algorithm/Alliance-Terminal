#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace teleop::core {

// 单槽：只保留最新值，set 覆盖未取走的值并计数。
// 线程安全：生产者和消费者可以不在同一线程（75 Hz 下互斥量开销可忽略）。
template <typename T>
class LatestValueRegister {
public:
    void set(T value) {
        std::lock_guard lock{mutex_};
        if (value_.has_value()) {
            ++overwritten_;
        }
        value_ = std::move(value);
    }

    bool try_take(T& out) {
        std::lock_guard lock{mutex_};
        if (!value_.has_value()) {
            return false;
        }
        out = std::move(*value_);
        value_.reset();
        return true;
    }

    [[nodiscard]] bool has_value() const {
        std::lock_guard lock{mutex_};
        return value_.has_value();
    }

    [[nodiscard]] std::uint64_t overwritten() const {
        std::lock_guard lock{mutex_};
        return overwritten_;
    }

    void clear() {
        std::lock_guard lock{mutex_};
        value_.reset();
    }

private:
    mutable std::mutex mutex_;
    std::optional<T> value_;
    std::uint64_t overwritten_ = 0;
};

} // namespace teleop::core
