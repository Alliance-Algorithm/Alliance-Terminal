#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "rm_proto/topics.hpp"

namespace {

using rm::proto::Direction;
using rm::proto::Source;
using rm::proto::topics;

constexpr std::array<std::string_view, rm::proto::kTopicCount> kExpectedNames = {
    "KeyboardMouseControl",
    "CustomControl",
    "MapClickCmd",
    "AssemblyCommand",
    "RobotPerformanceSelectionCommand",
    "CommonCommand",
    "RuneActivateCommand",
    "DartCommand",
    "SentryCtrlCommand",
    "GameStatus",
    "GlobalUnitStatus",
    "GlobalLogisticsStatus",
    "GlobalSpecialMechanism",
    "Event",
    "RobotInjuryStat",
    "RobotRespawnStatus",
    "RobotStaticStatus",
    "RobotDynamicStatus",
    "RobotModuleStatus",
    "RobotPosition",
    "Buff",
    "PenaltyInfo",
    "RobotPathPlanInfo",
    "MapClickInfo",
    "RadarInfoToClient",
    "CustomByteBlock",
    "TechCoreMotionStateSync",
    "RobotPerformanceSelectionSync",
    "RuneStatusSync",
    "SentryStatusSync",
    "DartSelectTargetStatusSync",
    "SentryCtrlResult",
};

TEST(Topics, Has32Entries) { EXPECT_EQ(topics().size(), rm::proto::kTopicCount); }

TEST(Topics, IdsAreContiguous) {
    for (std::size_t i = 0; i < topics().size(); ++i) {
        EXPECT_EQ(topics()[i].id, i + 1) << "index " << i;
    }
}

TEST(Topics, NamesMatchProtocol) {
    for (std::size_t i = 0; i < topics().size(); ++i) {
        EXPECT_EQ(topics()[i].name, kExpectedNames[i]) << "index " << i;
    }
}

TEST(Topics, NamesAreUnique) {
    std::set<std::string> seen;
    for (const auto& def : topics()) {
        seen.emplace(def.name);
    }
    EXPECT_EQ(seen.size(), topics().size());
}

TEST(Topics, DirectionSplit) {
    std::size_t up = 0;
    std::size_t down = 0;
    for (const auto& def : topics()) {
        if (def.direction == Direction::Up) {
            ++up;
        } else {
            ++down;
        }
    }
    EXPECT_EQ(up, 9U);
    EXPECT_EQ(down, 23U);
}

TEST(Topics, QosIsZeroOrOne) {
    for (const auto& def : topics()) {
        EXPECT_TRUE(def.qos == 0 || def.qos == 1) << def.name;
    }
}

TEST(Topics, QosPerTopicClass) {
    EXPECT_EQ(rm::proto::find(std::uint32_t{1})->qos, 0);
    EXPECT_EQ(rm::proto::find(std::uint32_t{2})->qos, 0);
    for (std::uint32_t id = 3; id <= 9; ++id) {
        EXPECT_EQ(rm::proto::find(id)->qos, 1) << "id " << id;
    }
    EXPECT_EQ(rm::proto::find(std::uint32_t{10})->qos, 0);
    EXPECT_EQ(rm::proto::find(std::uint32_t{18})->qos, 0);
    EXPECT_EQ(rm::proto::find(std::uint32_t{26})->qos, 0);
    EXPECT_EQ(rm::proto::find(std::uint32_t{14})->qos, 1);
    EXPECT_EQ(rm::proto::find(std::uint32_t{32})->qos, 1);
}

TEST(Topics, OnlySevenCommandsAreSendableByUi) {
    std::size_t count = 0;
    for (const auto& def : topics()) {
        if (def.source != Source::Ui) {
            continue;
        }
        ++count;
        EXPECT_EQ(def.direction, Direction::Up) << def.name;
        EXPECT_EQ(def.qos, 1) << def.name;
        EXPECT_TRUE(def.id >= 3 && def.id <= 9) << def.name;
    }
    EXPECT_EQ(count, 7U);
}

TEST(Topics, OnlyCustomControlComesFromRmcs) {
    std::size_t count = 0;
    for (const auto& def : topics()) {
        if (def.source == Source::Rmcs) {
            ++count;
            EXPECT_EQ(def.id, 2U) << def.name;
        }
    }
    EXPECT_EQ(count, 1U);
}

TEST(Topics, LookupById) {
    const auto* first = rm::proto::find(std::uint32_t{1});
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->name, "KeyboardMouseControl");

    const auto* last = rm::proto::find(std::uint32_t{32});
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->name, "SentryCtrlResult");

    EXPECT_EQ(rm::proto::find(std::uint32_t{0}), nullptr);
    EXPECT_EQ(rm::proto::find(std::uint32_t{33}), nullptr);
}

TEST(Topics, LookupByName) {
    const auto* def = rm::proto::find(std::string_view{"CustomByteBlock"});
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->id, 26U);
    EXPECT_EQ(rm::proto::find(std::string_view{"custombyteblock"}), nullptr);
    EXPECT_EQ(rm::proto::find(std::string_view{""}), nullptr);
}

} // namespace
