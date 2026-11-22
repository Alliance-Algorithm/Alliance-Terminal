#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ipc.pb.h"
#include "teleop_ipc/control_link.hpp"

namespace {

using teleop::link::ErrorCode;

template <typename Message>
std::string serialize(const Message& message) {
    std::string out;
    EXPECT_TRUE(message.SerializeToString(&out));
    return out;
}

std::span<const std::byte> as_bytes(const std::string& value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::string as_string(std::span<const std::byte> value) {
    return std::string{reinterpret_cast<const char*>(value.data()), value.size()};
}

template <typename Encode>
std::string encoded(Encode&& encode) {
    std::array<std::byte, 1024> out{};
    const auto size = encode(std::span<std::byte>{out});
    EXPECT_TRUE(size.has_value()) << teleop::link::error_code_name(size.error().code);
    if (!size.has_value()) {
        return {};
    }
    return as_string(std::span<const std::byte>{out}.first(*size));
}

TEST(ControlLink, HelloMatchesGeneratedProto) {
    const teleop::link::Hello msg{1, 5, "abc"};
    const auto bytes =
        encoded([&](std::span<std::byte> out) { return teleop::link::encode_hello(msg, out); });

    ::teleop::ipc::Hello generated;
    generated.set_proto_version(1);
    generated.set_capabilities(5);
    generated.set_client_version("abc");
    EXPECT_EQ(bytes, serialize(generated));
}

TEST(ControlLink, HelloDefaultsMatchGeneratedProto) {
    const teleop::link::Hello msg{};
    const auto bytes =
        encoded([&](std::span<std::byte> out) { return teleop::link::encode_hello(msg, out); });
    EXPECT_EQ(bytes, serialize(::teleop::ipc::Hello{}));
}

TEST(ControlLink, HelloAckMatchesGeneratedProto) {
    const teleop::link::HelloAck msg{1, 7, "2027-V1.0.0"};
    const auto bytes =
        encoded([&](std::span<std::byte> out) { return teleop::link::encode_hello_ack(msg, out); });

    ::teleop::ipc::HelloAck generated;
    generated.set_proto_version(1);
    generated.set_capabilities(7);
    generated.set_protocol_rev("2027-V1.0.0");
    EXPECT_EQ(bytes, serialize(generated));
}

TEST(ControlLink, UpDataMatchesGeneratedProto) {
    const std::array<std::byte, 2> payload = {std::byte{0x41}, std::byte{0x42}};
    const teleop::link::UpData msg{1, 1000, payload};
    const auto bytes =
        encoded([&](std::span<std::byte> out) { return teleop::link::encode_up_data(msg, out); });

    ::teleop::ipc::UpData generated;
    generated.set_channel(1);
    generated.set_t_src_ns(1000);
    generated.set_payload("AB", 2);
    EXPECT_EQ(bytes, serialize(generated));
}

TEST(ControlLink, UpDataDefaultsMatchGeneratedProto) {
    const teleop::link::UpData msg{1, 0, {}};
    const auto bytes =
        encoded([&](std::span<std::byte> out) { return teleop::link::encode_up_data(msg, out); });

    ::teleop::ipc::UpData generated;
    generated.set_channel(1);
    EXPECT_EQ(bytes, serialize(generated));
}

TEST(ControlLink, NegativeTimestampMatchesGeneratedProto) {
    const teleop::link::UpData msg{1, -1, {}};
    const auto bytes =
        encoded([&](std::span<std::byte> out) { return teleop::link::encode_up_data(msg, out); });

    ::teleop::ipc::UpData generated;
    generated.set_channel(1);
    generated.set_t_src_ns(-1);
    EXPECT_EQ(bytes, serialize(generated));
}

TEST(ControlLink, DownDataMatchesGeneratedProto) {
    const std::array<std::byte, 3> payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    const teleop::link::DownData msg{2, 2000, payload};
    const auto bytes =
        encoded([&](std::span<std::byte> out) { return teleop::link::encode_down_data(msg, out); });

    ::teleop::ipc::DownData generated;
    generated.set_channel(2);
    generated.set_t_recv_ns(2000);
    generated.set_payload("\x01\x02\x03", 3);
    EXPECT_EQ(bytes, serialize(generated));
}

TEST(ControlLink, HealthMatchesGeneratedProto) {
    const teleop::link::Health msg{1, 42};
    const auto bytes =
        encoded([&](std::span<std::byte> out) { return teleop::link::encode_health(msg, out); });

    ::teleop::ipc::Health generated;
    generated.set_state(1);
    generated.set_load_pct(42);
    EXPECT_EQ(bytes, serialize(generated));
}

TEST(ControlLink, DecodesGeneratedMessages) {
    ::teleop::ipc::UpData generated;
    generated.set_channel(1);
    generated.set_t_src_ns(123456);
    generated.set_payload("hello", 5);
    const auto serialized = serialize(generated);

    const auto decoded = teleop::link::decode_up_data(as_bytes(serialized));
    ASSERT_TRUE(decoded.has_value()) << teleop::link::error_code_name(decoded.error().code);
    EXPECT_EQ(decoded->channel, 1U);
    EXPECT_EQ(decoded->t_src_ns, 123456);
    EXPECT_EQ(as_string(decoded->payload), "hello");

    const auto* begin = reinterpret_cast<const std::byte*>(serialized.data());
    EXPECT_GE(decoded->payload.data(), begin);
    EXPECT_LE(decoded->payload.data() + decoded->payload.size(), begin + serialized.size());
}

TEST(ControlLink, PayloadSizesRoundTrip) {
    for (const std::size_t size :
         {std::size_t{0}, std::size_t{60}, std::size_t{300}, std::size_t{1400}}) {
        std::vector<std::byte> payload(size);
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<std::byte>((i * 7U) & 0xFFU);
        }
        const teleop::link::UpData msg{1, 100, std::span<const std::byte>{payload}};
        std::array<std::byte, 2048> out{};
        const auto encoded_size = teleop::link::encode_up_data(msg, out);
        ASSERT_TRUE(encoded_size.has_value())
            << teleop::link::error_code_name(encoded_size.error().code);

        const auto decoded =
            teleop::link::decode_up_data(std::span<const std::byte>{out}.first(*encoded_size));
        ASSERT_TRUE(decoded.has_value()) << teleop::link::error_code_name(decoded.error().code);
        EXPECT_EQ(decoded->payload.size(), size);
        EXPECT_TRUE(std::equal(decoded->payload.begin(), decoded->payload.end(), payload.begin()));
    }
}

TEST(ControlLink, DecodesEmptyBodyAsDefaults) {
    const auto hello = teleop::link::decode_hello({});
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->proto_version, 0U);
    EXPECT_EQ(hello->capabilities, 0U);
    EXPECT_TRUE(hello->client_version.empty());

    const auto health = teleop::link::decode_health({});
    ASSERT_TRUE(health.has_value());
    EXPECT_EQ(health->state, 0U);
    EXPECT_EQ(health->load_pct, 0U);
}

TEST(ControlLink, SkipsUnknownFields) {
    ::teleop::ipc::Hello generated;
    generated.set_proto_version(1);
    std::string serialized = serialize(generated);
    // field 99, varint 5：tag = (99 << 3) | 0 = 792 -> varint 0x98 0x06
    serialized.append("\x98\x06\x05", 3);

    const auto decoded = teleop::link::decode_hello(as_bytes(serialized));
    ASSERT_TRUE(decoded.has_value()) << teleop::link::error_code_name(decoded.error().code);
    EXPECT_EQ(decoded->proto_version, 1U);
}

TEST(ControlLink, RejectsTruncatedFields) {
    const std::array<std::byte, 1> tag_only = {std::byte{0x08}};
    const auto no_value = teleop::link::decode_hello(tag_only);
    ASSERT_FALSE(no_value.has_value());
    EXPECT_EQ(no_value.error().code, ErrorCode::Truncated);

    const std::array<std::byte, 3> short_bytes = {
        std::byte{0x1A}, std::byte{0x05}, std::byte{0x61}};
    const auto truncated = teleop::link::decode_hello(short_bytes);
    ASSERT_FALSE(truncated.has_value());
    EXPECT_EQ(truncated.error().code, ErrorCode::Truncated);
}

TEST(ControlLink, RejectsOverflowingVarint) {
    const std::array<std::byte, 12> bytes = {std::byte{0x08}, std::byte{0xFF}, std::byte{0xFF},
                                             std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                             std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                             std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}};
    const auto decoded = teleop::link::decode_hello(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::Protocol);
}

TEST(ControlLink, EncodeBufferTooSmall) {
    const teleop::link::Hello msg{1, 5, "abc"};
    std::array<std::byte, 2> out{};
    const auto result = teleop::link::encode_hello(msg, out);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::TooLarge);
}

} // namespace
