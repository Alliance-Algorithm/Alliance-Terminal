#include <cstdint>

#include <gtest/gtest.h>

#include "core/clock.hpp"
#include "core/latest_value_register.hpp"

TEST(Core, LatestValueRegisterKeepsLatest) {
    teleop::core::LatestValueRegister<int> slot;
    int value = 0;

    EXPECT_FALSE(slot.has_value());
    EXPECT_FALSE(slot.try_take(value));

    slot.set(1);
    slot.set(2);
    slot.set(3);

    EXPECT_TRUE(slot.has_value());
    EXPECT_EQ(slot.overwritten(), 2ULL);
    EXPECT_TRUE(slot.try_take(value));
    EXPECT_EQ(value, 3);
    EXPECT_FALSE(slot.try_take(value));
}

TEST(Core, LatestValueRegisterClear) {
    teleop::core::LatestValueRegister<int> slot;
    slot.set(7);
    slot.clear();

    int value = 0;
    EXPECT_FALSE(slot.try_take(value));
}

TEST(Core, ClockIsMonotonic) {
    const std::int64_t first = teleop::core::now_ns();
    const std::int64_t second = teleop::core::now_ns();

    EXPECT_GT(first, 0);
    EXPECT_GE(second, first);
}
