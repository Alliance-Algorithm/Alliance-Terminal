#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <variant>
#include <vector>

#include "rm/terminal.pb.h"
#include "rm_proto/codec.hpp"

namespace {

using rm::proto::Encoded;
using rm::proto::topics;

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    out.reserve(values.size());
    for (int value : values) {
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
}

TEST(Codec, CustomControlGoldenBytes) {
    rm::terminal::CustomControl msg;
    msg.set_data("\xAA\xBB", 2);

    const Encoded encoded = rm::proto::encode(msg);
    EXPECT_EQ(encoded.topic_id, 2U);
    EXPECT_EQ(encoded.bytes, bytes({0x0A, 0x02, 0xAA, 0xBB}));
}

TEST(Codec, RuneActivateGoldenBytes) {
    rm::terminal::RuneActivateCommand msg;
    msg.set_activate(1);

    const Encoded encoded = rm::proto::encode(msg);
    EXPECT_EQ(encoded.topic_id, 7U);
    EXPECT_EQ(encoded.bytes, bytes({0x08, 0x01}));
}

TEST(Codec, DartCommandGoldenBytes) {
    rm::terminal::DartCommand msg;
    msg.set_target_id(1);
    msg.set_open(true);
    msg.set_launch_confirm(true);

    const Encoded encoded = rm::proto::encode(msg);
    EXPECT_EQ(encoded.topic_id, 8U);
    EXPECT_EQ(encoded.bytes, bytes({0x08, 0x01, 0x10, 0x01, 0x18, 0x01}));
}

TEST(Codec, MapClickUsesTag3AndTag6) {
    rm::terminal::MapClickCmd msg;
    msg.set_mark_type(2);
    msg.set_mark_mode(1);

    const Encoded encoded = rm::proto::encode(msg);
    EXPECT_EQ(encoded.topic_id, 3U);
    EXPECT_EQ(encoded.bytes, bytes({0x18, 0x02, 0x30, 0x01}));
}

TEST(Codec, EveryTopicDecodesEmptyAndReEncodes) {
    for (const auto& def : topics()) {
        const auto decoded = rm::proto::decode(def.id, {});
        ASSERT_TRUE(decoded.has_value()) << def.name;
        EXPECT_EQ(decoded->index(), def.id - 1) << def.name;

        const Encoded re = rm::proto::encode(*decoded);
        EXPECT_EQ(re.topic_id, def.id) << def.name;
        EXPECT_TRUE(re.bytes.empty()) << def.name;
    }
}

TEST(Codec, RoundTripGameStatus) {
    rm::terminal::GameStatus msg;
    msg.set_current_round(3);
    msg.set_total_rounds(5);
    msg.set_red_score(120);
    msg.set_blue_score(98);
    msg.set_current_stage(4);
    msg.set_stage_countdown_sec(245);
    msg.set_is_paused(false);
    msg.set_game_result(255);

    const auto decoded = rm::proto::decode(10, rm::proto::encode(msg).bytes);
    ASSERT_TRUE(decoded.has_value());

    const auto* back = std::get_if<rm::terminal::GameStatus>(&*decoded);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->SerializeAsString(), msg.SerializeAsString());
}

TEST(Codec, RoundTripNestedRadarInfo) {
    rm::terminal::RadarInfoToClient msg;
    for (int i = 0; i < 8; ++i) {
        auto* item = msg.add_radar_single_robot_info();
        item->set_target_pos_x(static_cast<std::uint32_t>(i * 100));
        item->set_target_pos_y(static_cast<std::uint32_t>(i * 200));
        item->set_is_high_light(static_cast<std::uint32_t>(i % 3));
    }

    const auto decoded = rm::proto::decode(25, rm::proto::encode(msg).bytes);
    ASSERT_TRUE(decoded.has_value());

    const auto* back = std::get_if<rm::terminal::RadarInfoToClient>(&*decoded);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->radar_single_robot_info_size(), 8);
    EXPECT_EQ(back->radar_single_robot_info(7).target_pos_x(), 700U);
}

TEST(Codec, RoundTripCustomByteBlock300Bytes) {
    rm::terminal::CustomByteBlock msg;
    msg.set_data(std::string(300, '\x5A'));

    const auto decoded = rm::proto::decode(26, rm::proto::encode(msg).bytes);
    ASSERT_TRUE(decoded.has_value());

    const auto* back = std::get_if<rm::terminal::CustomByteBlock>(&*decoded);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->data().size(), 300U);
}

TEST(Codec, DecodeRejectsGarbage) {
    EXPECT_FALSE(rm::proto::decode(10, bytes({0xFF})).has_value());
}

TEST(Codec, DecodeRejectsUnknownTopic) {
    EXPECT_FALSE(rm::proto::decode(0, {}).has_value());
    EXPECT_FALSE(rm::proto::decode(33, {}).has_value());
}

TEST(Codec, DecodeIgnoresUnknownFields) {
    // CustomControl.data = AA BB，后面跟一个未知字段 99（varint 5）
    const auto raw = bytes({0x0A, 0x02, 0xAA, 0xBB, 0x98, 0x06, 0x05});

    const auto decoded = rm::proto::decode(2, raw);
    ASSERT_TRUE(decoded.has_value());

    const auto* msg = std::get_if<rm::terminal::CustomControl>(&*decoded);
    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->data(), std::string("\xAA\xBB", 2));
}

TEST(Codec, LookupByIdMatchesEncode) {
    rm::terminal::SentryCtrlCommand msg;
    msg.set_command_id(12);

    const Encoded encoded = rm::proto::encode(msg);
    const auto* def = rm::proto::find(encoded.topic_id);
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->name, "SentryCtrlCommand");

    const auto decoded = rm::proto::decode(*def, encoded.bytes);
    ASSERT_TRUE(decoded.has_value());
    const auto* back = std::get_if<rm::terminal::SentryCtrlCommand>(&*decoded);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->command_id(), 12U);
}

} // namespace
