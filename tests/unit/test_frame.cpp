#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "teleop_ipc/frame.hpp"

namespace {

using teleop::link::ErrorCode;
using teleop::link::FrameHeader;

std::vector<std::uint8_t> as_u8(const std::array<std::byte, teleop::link::kHeaderBytes>& bytes) {
    std::vector<std::uint8_t> out;
    out.reserve(bytes.size());
    for (const auto byte : bytes) {
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return out;
}

FrameHeader sample_header() {
    FrameHeader header{};
    header.proto_version = 1;
    header.msg_type = 3;
    header.flags = 0x1234;
    header.session_id = 0x0102030405060708ULL;
    header.seq = 2;
    header.t_mono_ns = 0x0102030405060708LL;
    header.body_len = 4;
    return header;
}

TEST(Frame, HeaderGoldenBytesAreBigEndian) {
    const auto encoded = teleop::link::encode_header(sample_header());
    const std::vector<std::uint8_t> expected = {
        0x54, 0x4C, 0x01, 0x03, 0x12, 0x34, 0x00, 0x04, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    };
    EXPECT_EQ(as_u8(encoded), expected);
}

TEST(Frame, HeaderRoundTrip) {
    const auto encoded = teleop::link::encode_header(sample_header());
    const auto decoded = teleop::link::decode_header(encoded);
    ASSERT_TRUE(decoded.has_value()) << teleop::link::error_code_name(decoded.error().code);
    EXPECT_EQ(decoded->proto_version, 1);
    EXPECT_EQ(decoded->msg_type, 3);
    EXPECT_EQ(decoded->flags, 0x1234);
    EXPECT_EQ(decoded->body_len, 4);
    EXPECT_EQ(decoded->session_id, 0x0102030405060708ULL);
    EXPECT_EQ(decoded->seq, 2U);
    EXPECT_EQ(decoded->t_mono_ns, 0x0102030405060708LL);
}

TEST(Frame, EncodeFrameAppendsBody) {
    FrameHeader header = sample_header();
    const std::array<std::byte, 4> body = {
        std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};
    std::array<std::byte, teleop::link::kHeaderBytes + body.size()> out{};

    const auto size = teleop::link::encode_frame(header, body, out);
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, out.size());

    const std::span<const std::byte, teleop::link::kHeaderBytes> header_bytes{
        out.data(), teleop::link::kHeaderBytes};
    const auto decoded = teleop::link::decode_header(header_bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->body_len, body.size());
    EXPECT_EQ(out[teleop::link::kHeaderBytes], std::byte{0xAA});
    EXPECT_EQ(out[teleop::link::kHeaderBytes + 3], std::byte{0xDD});
}

TEST(Frame, ValidateLength) {
    FrameHeader header{};
    header.body_len = 4;

    EXPECT_TRUE(teleop::link::validate_frame_length(header, 36).has_value());

    const auto short_record = teleop::link::validate_frame_length(header, 20);
    ASSERT_FALSE(short_record.has_value());
    EXPECT_EQ(short_record.error().code, ErrorCode::Truncated);

    const auto mismatch = teleop::link::validate_frame_length(header, 35);
    ASSERT_FALSE(mismatch.has_value());
    EXPECT_EQ(mismatch.error().code, ErrorCode::Protocol);

    const auto extra = teleop::link::validate_frame_length(header, 37);
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error().code, ErrorCode::Protocol);
}

TEST(Frame, RejectsBadMagic) {
    auto bytes = teleop::link::encode_header(sample_header());
    bytes[0] = std::byte{0x00};
    const auto decoded = teleop::link::decode_header(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::Protocol);
}

TEST(Frame, RejectsBadFrameVersion) {
    auto bytes = teleop::link::encode_header(sample_header());
    bytes[2] = std::byte{0x02};
    const auto decoded = teleop::link::decode_header(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::Protocol);
}

TEST(Frame, RejectsBodyOverLimit) {
    auto bytes = teleop::link::encode_header(sample_header());
    bytes[6] = std::byte{0xFF};
    bytes[7] = std::byte{0xFF}; // 65535 > 65504
    const auto decoded = teleop::link::decode_header(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::TooLarge);
}

TEST(Frame, IgnoresUnknownFlagBits) {
    auto header = sample_header();
    header.flags = 0xFFFF;
    const auto encoded = teleop::link::encode_header(header);
    const auto decoded = teleop::link::decode_header(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->flags, 0xFFFF);
}

TEST(Frame, MaxBodyFits) {
    FrameHeader header{};
    header.body_len = static_cast<std::uint16_t>(teleop::link::kMaxBodyBytes);
    const std::vector<std::byte> body(teleop::link::kMaxBodyBytes, std::byte{0x5A});
    std::vector<std::byte> out(teleop::link::kMaxFrameBytes);

    const auto size = teleop::link::encode_frame(header, body, out);
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, teleop::link::kMaxFrameBytes);
    EXPECT_TRUE(teleop::link::validate_frame_length(header, *size).has_value());
}

TEST(Frame, BodyOverLimitRejected) {
    FrameHeader header{};
    const std::vector<std::byte> body(teleop::link::kMaxBodyBytes + 1, std::byte{0x5A});
    std::vector<std::byte> out(teleop::link::kMaxFrameBytes + 1);

    const auto size = teleop::link::encode_frame(header, body, out);
    ASSERT_FALSE(size.has_value());
    EXPECT_EQ(size.error().code, ErrorCode::TooLarge);
}

TEST(Frame, OutputBufferTooSmall) {
    FrameHeader header{};
    const std::array<std::byte, 4> body = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::array<std::byte, 10> out{};

    const auto size = teleop::link::encode_frame(header, body, out);
    ASSERT_FALSE(size.has_value());
    EXPECT_EQ(size.error().code, ErrorCode::TooLarge);
}

} // namespace
