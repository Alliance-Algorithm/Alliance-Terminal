#pragma once

#include <cstdint>
#include <ctime>

namespace teleop::core {

// 单调钟：不随系统时间调整；Linux CLOCK_MONOTONIC 不计 suspend 时间。
using NowFn = std::int64_t (*)() noexcept;

inline std::int64_t now_ns() noexcept {
    timespec timestamp{};
    if (::clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0;
    }
    return static_cast<std::int64_t>(timestamp.tv_sec) * 1'000'000'000
         + static_cast<std::int64_t>(timestamp.tv_nsec);
}

} // namespace teleop::core
