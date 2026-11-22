#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "teleop_ipc/control_link.hpp"
#include "teleop_ipc/frame.hpp"
#include "teleop_ipc/seqpacket.hpp"
#include "teleop_ipc/session.hpp"

namespace {

using teleop::link::ErrorCode;
using teleop::link::Role;
using teleop::link::Session;
using teleop::link::SessionOptions;
using teleop::link::SessionState;
using teleop::link::Socket;

constexpr std::int64_t kFakeNow = 1'234'567'890;

std::int64_t fake_now() noexcept { return kFakeNow; }

struct Pair {
    Session connector;
    Session acceptor;
};

Pair make_pair(std::uint32_t max_body = 512) {
    auto sockets = Socket::pair();
    if (!sockets) {
        std::abort();
    }
    SessionOptions connector_options{};
    connector_options.role = Role::Connector;
    connector_options.session_id = 0x1111;
    connector_options.max_body_bytes = max_body;
    connector_options.now_ns = &fake_now;

    SessionOptions acceptor_options{};
    acceptor_options.role = Role::Acceptor;
    acceptor_options.session_id = 0x2222;
    acceptor_options.max_body_bytes = max_body;
    acceptor_options.now_ns = &fake_now;

    return Pair{
        Session{std::move(sockets->first), connector_options},
        Session{std::move(sockets->second), acceptor_options},
    };
}

void send_message(Session& session, std::uint8_t type, std::span<const std::byte> body) {
    std::array<std::byte, 1024> frame{};
    const auto prepared = session.prepare(type, body, frame);
    ASSERT_TRUE(prepared.has_value()) << teleop::link::error_code_name(prepared.error().code);
    const auto sent = session.send_prepared(std::span<const std::byte>{frame}.first(*prepared));
    ASSERT_TRUE(sent.has_value()) << teleop::link::error_code_name(sent.error().code);
}

TEST(Session, HandshakeAndSessionIds) {
    auto pair = make_pair();
    pair.connector.begin_handshake();
    pair.acceptor.begin_handshake();

    send_message(pair.connector, teleop::link::kMsgHello, {});

    std::array<std::byte, 2048> receive{};
    const auto hello = pair.acceptor.recv(receive);
    ASSERT_TRUE(hello.has_value()) << teleop::link::error_code_name(hello.error().code);
    EXPECT_EQ(hello->header.msg_type, teleop::link::kMsgHello);
    EXPECT_EQ(hello->header.session_id, 0x1111U);
    EXPECT_EQ(hello->header.seq, 1U);
    EXPECT_EQ(hello->header.t_mono_ns, kFakeNow);
    EXPECT_EQ(pair.acceptor.peer_session_id(), 0x1111U);

    send_message(pair.acceptor, teleop::link::kMsgHelloAck, {});
    const auto ack = pair.connector.recv(receive);
    ASSERT_TRUE(ack.has_value()) << teleop::link::error_code_name(ack.error().code);
    EXPECT_EQ(ack->header.msg_type, teleop::link::kMsgHelloAck);
    EXPECT_EQ(ack->header.session_id, 0x2222U);
    EXPECT_EQ(ack->header.seq, 1U);
    EXPECT_EQ(pair.connector.peer_session_id(), 0x2222U);

    pair.acceptor.activate();
    pair.connector.activate();
    EXPECT_EQ(pair.connector.state(), SessionState::Active);
    EXPECT_EQ(pair.acceptor.state(), SessionState::Active);
    EXPECT_EQ(pair.connector.stats().tx_frames, 1U);
    EXPECT_EQ(pair.connector.stats().rx_frames, 1U);
}

TEST(Session, SequenceGapIsCounted) {
    auto pair = make_pair();
    pair.connector.begin_handshake();
    pair.acceptor.begin_handshake();
    send_message(pair.connector, teleop::link::kMsgHello, {});

    std::array<std::byte, 2048> receive{};
    ASSERT_TRUE(pair.acceptor.recv(receive).has_value());

    std::array<std::byte, 1024> scratch{};
    const auto skipped = pair.connector.prepare(teleop::link::kMsgUpData, {}, scratch);
    ASSERT_TRUE(skipped.has_value());
    (void)skipped;

    send_message(pair.connector, teleop::link::kMsgUpData, {});
    const auto third = pair.acceptor.recv(receive);
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third->header.seq, 3U);
    EXPECT_EQ(pair.acceptor.stats().rx_gaps, 1U);
}

TEST(Session, SessionIdChangeIsRejected) {
    auto pair = make_pair();
    pair.connector.begin_handshake();
    pair.acceptor.begin_handshake();
    send_message(pair.connector, teleop::link::kMsgHello, {});

    std::array<std::byte, 2048> receive{};
    ASSERT_TRUE(pair.acceptor.recv(receive).has_value());

    teleop::link::FrameHeader header{};
    header.msg_type = teleop::link::kMsgUpData;
    header.session_id = 0x9999;
    header.seq = 2;
    header.t_mono_ns = kFakeNow;
    std::array<std::byte, teleop::link::kHeaderBytes> frame{};
    ASSERT_TRUE(teleop::link::encode_frame(header, {}, frame).has_value());
    ASSERT_TRUE(pair.connector.socket().send(frame).has_value());

    const auto changed = pair.acceptor.recv(receive);
    ASSERT_FALSE(changed.has_value());
    EXPECT_EQ(changed.error().code, ErrorCode::Protocol);
    EXPECT_EQ(pair.acceptor.stats().peer_session_changes, 1U);
    EXPECT_EQ(pair.acceptor.state(), SessionState::Failed);
}

TEST(Session, ZeroSessionIdIsRejected) {
    auto pair = make_pair();
    pair.acceptor.begin_handshake();

    teleop::link::FrameHeader header{};
    header.msg_type = teleop::link::kMsgHello;
    header.session_id = 0;
    header.seq = 1;
    std::array<std::byte, teleop::link::kHeaderBytes> frame{};
    ASSERT_TRUE(teleop::link::encode_frame(header, {}, frame).has_value());
    ASSERT_TRUE(pair.connector.socket().send(frame).has_value());

    std::array<std::byte, 2048> receive{};
    const auto result = pair.acceptor.recv(receive);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Protocol);
}

TEST(Session, BodyLengthMismatchIsRejected) {
    auto pair = make_pair();
    pair.acceptor.begin_handshake();

    teleop::link::FrameHeader header{};
    header.msg_type = teleop::link::kMsgHello;
    header.session_id = 0x1111;
    header.seq = 1;
    header.body_len = 100;
    const auto header_bytes = teleop::link::encode_header(header);

    std::array<std::byte, 40> record{};
    std::copy(header_bytes.begin(), header_bytes.end(), record.begin());
    ASSERT_TRUE(pair.connector.socket().send(record).has_value());

    std::array<std::byte, 2048> receive{};
    const auto result = pair.acceptor.recv(receive);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Protocol);
}

TEST(Session, ShortRecordIsRejected) {
    auto pair = make_pair();
    pair.acceptor.begin_handshake();

    std::array<std::byte, 20> record{};
    ASSERT_TRUE(pair.connector.socket().send(record).has_value());

    std::array<std::byte, 2048> receive{};
    const auto result = pair.acceptor.recv(receive);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Truncated);
}

TEST(Session, EmptyReceiveWouldBlock) {
    auto pair = make_pair();
    pair.acceptor.begin_handshake();

    std::array<std::byte, 2048> receive{};
    const auto result = pair.acceptor.recv(receive);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::WouldBlock);
}

TEST(Session, PrepareRejectsOversizedBody) {
    auto pair = make_pair(8);
    const std::vector<std::byte> body(9, std::byte{0x5A});
    std::array<std::byte, 128> out{};
    const auto result = pair.connector.prepare(teleop::link::kMsgUpData, body, out);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::TooLarge);
}

TEST(Session, AckRetryReusesSequence) {
    auto pair = make_pair();
    pair.connector.begin_handshake();
    pair.acceptor.begin_handshake();

    std::array<std::byte, 1024> frame{};
    const auto prepared = pair.connector.prepare(teleop::link::kMsgHello, {}, frame);
    ASSERT_TRUE(prepared.has_value());
    const auto view = std::span<const std::byte>{frame}.first(*prepared);

    ASSERT_TRUE(pair.connector.send_prepared(view).has_value());
    ASSERT_TRUE(pair.connector.send_prepared(view).has_value());

    std::array<std::byte, 2048> receive{};
    const auto first = pair.acceptor.recv(receive);
    ASSERT_TRUE(first.has_value());
    const auto second = pair.acceptor.recv(receive);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->header.seq, second->header.seq);
    EXPECT_EQ(pair.acceptor.stats().rx_anomalies, 1U);
}

TEST(Session, UnknownMessageTypeIsPassedThrough) {
    auto pair = make_pair();
    pair.connector.begin_handshake();
    pair.acceptor.begin_handshake();

    send_message(pair.connector, 200, {});
    std::array<std::byte, 2048> receive{};
    const auto result = pair.acceptor.recv(receive);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.msg_type, 200);
}

TEST(Session, CloseStopsPreparing) {
    auto pair = make_pair();
    pair.connector.begin_handshake();
    pair.connector.close();
    EXPECT_EQ(pair.connector.state(), SessionState::Closed);

    std::array<std::byte, 128> out{};
    const auto result = pair.connector.prepare(teleop::link::kMsgHello, {}, out);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Closed);
}

TEST(Session, GeneratedSessionIdIsNonZero) {
    auto sockets = Socket::pair();
    ASSERT_TRUE(sockets.has_value());
    SessionOptions options{};
    options.session_id = 0;
    Session session{std::move(sockets->first), options};
    EXPECT_NE(session.session_id(), 0U);
}

} // namespace
