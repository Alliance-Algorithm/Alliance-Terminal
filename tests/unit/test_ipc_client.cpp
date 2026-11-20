#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>

#include <gtest/gtest.h>

#include "ipc/ipc_client.hpp"
#include "teleop_ipc/control_link.hpp"
#include "teleop_ipc/seqpacket.hpp"
#include "teleop_ipc/session.hpp"

namespace {

namespace ipc = teleop::ipc;
namespace link = teleop::link;

std::int64_t g_now = 1'000'000'000;

std::int64_t fake_now() noexcept {
    return g_now;
}

constexpr std::size_t kBodyBytes = static_cast<std::size_t>(link::kControlMaxBodyBytes);
using Body = std::array<std::byte, kBodyBytes>;
using Frame = std::array<std::byte, link::kHeaderBytes + kBodyBytes>;

struct FakePeer {
    std::optional<link::Socket> socket;
    std::optional<link::Session> session;
    Frame rx{};

    bool accept() {
        if (!socket) {
            return false;
        }
        link::SessionOptions options{};
        options.role = link::Role::Acceptor;
        options.session_id = 0x1234;
        options.max_body_bytes = static_cast<std::uint32_t>(kBodyBytes);
        options.now_ns = &fake_now;
        session.emplace(std::move(*socket), options);
        socket.reset();
        session->begin_handshake();
        return true;
    }

    link::Result<link::FrameView> recv() {
        return session->recv(rx);
    }

    bool send(std::uint8_t type, std::span<const std::byte> body) {
        Frame frame{};
        auto size = session->prepare(type, body, frame);
        if (!size) {
            return false;
        }
        auto result =
            session->send_prepared(std::span<const std::byte>{frame.data(), *size});
        return result.has_value();
    }

    bool send_raw(std::span<const std::byte> bytes) {
        return session->socket().send(bytes).has_value();
    }
};

ipc::Config make_config(FakePeer& peer, std::size_t sndbuf = 4096, std::size_t rcvbuf = 4096) {
    ipc::Config config;
    config.socket_path = "test-socket";
    config.client_version = "test-client";
    config.down_hz = 50;
    config.rcvbuf_bytes = rcvbuf;
    config.sndbuf_bytes = sndbuf;
    config.handshake_timeout_ns = 2'000'000'000;
    config.health_timeout_ns = 3'000'000'000;
    config.backoff_min_ns = 100'000'000;
    config.backoff_max_ns = 2'000'000'000;
    config.backoff_jitter_permille = 0;
    config.allowed_peer_uid = -1;
    config.now_ns = &fake_now;
    config.connect = [&peer, sndbuf, rcvbuf]() -> link::Result<link::Socket> {
        auto pair = link::Socket::pair(link::SocketOptions{rcvbuf, sndbuf});
        if (!pair) {
            return tl::make_unexpected(pair.error());
        }
        peer.session.reset();
        peer.socket.emplace(std::move(pair->first));
        return std::move(pair->second);
    };
    return config;
}

void handshake(
    ipc::IpClient& client, FakePeer& peer, std::int64_t now,
    std::uint32_t version = link::kControlProtoVersion) {
    client.poll(now);
    ASSERT_TRUE(peer.accept());

    auto hello_frame = peer.recv();
    ASSERT_TRUE(hello_frame);
    EXPECT_EQ(hello_frame->header.msg_type, link::kMsgHello);

    auto hello = link::decode_hello(hello_frame->body);
    ASSERT_TRUE(hello);
    EXPECT_EQ(hello->proto_version, link::kControlProtoVersion);
    EXPECT_EQ(
        hello->capabilities,
        link::kCapUpData | link::kCapDownData | link::kCapHealth);
    EXPECT_EQ(std::string(hello->client_version), "test-client");

    link::HelloAck ack{};
    ack.proto_version = version;
    ack.capabilities = link::kCapUpData | link::kCapDownData | link::kCapHealth;
    ack.protocol_rev = "2027-V1.0.0";
    Body body{};
    auto size = link::encode_hello_ack(ack, body);
    ASSERT_TRUE(size);
    ASSERT_TRUE(peer.send(
        link::kMsgHelloAck, std::span<const std::byte>{body.data(), *size}));
    if (version == link::kControlProtoVersion) {
        peer.session->activate();
    }
    client.poll(now);
}

bool send_up(FakePeer& peer, std::span<const std::byte> payload, std::int64_t t_src_ns) {
    link::UpData up{};
    up.channel = link::kChannelCustomControl;
    up.t_src_ns = t_src_ns;
    up.payload = payload;
    Body body{};
    auto size = link::encode_up_data(up, body);
    if (!size) {
        return false;
    }
    return peer.send(link::kMsgUpData, std::span<const std::byte>{body.data(), *size});
}

bool send_health(FakePeer& peer, std::uint32_t state, std::uint32_t load_pct) {
    link::Health health{};
    health.state = state;
    health.load_pct = load_pct;
    Body body{};
    auto size = link::encode_health(health, body);
    if (!size) {
        return false;
    }
    return peer.send(link::kMsgHealth, std::span<const std::byte>{body.data(), *size});
}

TEST(IpcClient, HandshakeActivates) {
    g_now = 1'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};

    handshake(client, peer, g_now);

    EXPECT_EQ(client.state(), ipc::IpClient::State::Active);
    const auto stats = client.stats();
    EXPECT_EQ(stats.handshakes, 1ULL);
    EXPECT_EQ(stats.connect_attempts, 1ULL);
    EXPECT_EQ(stats.connects, 1ULL);
    EXPECT_EQ(stats.handshake_failures, 0ULL);
    EXPECT_EQ(client.peer_proto_version(), link::kControlProtoVersion);
    EXPECT_EQ(std::string(client.protocol_rev()), "2027-V1.0.0");
    EXPECT_GE(stats.peer_uid, 0);
}

TEST(IpcClient, NoPeerBacksOffAndStaysUsable) {
    g_now = 0;
    ipc::Config config;
    config.socket_path = "test-socket";
    config.now_ns = &fake_now;
    config.backoff_min_ns = 100'000'000;
    config.backoff_max_ns = 2'000'000'000;
    config.backoff_jitter_permille = 0;
    config.connect = []() -> link::Result<link::Socket> {
        return tl::make_unexpected(link::make_error(link::ErrorCode::Io, "connect", ENOENT));
    };

    ipc::IpClient client{std::move(config)};
    ipc::UpFrame up;
    EXPECT_FALSE(client.try_take_up(up));
    EXPECT_FALSE(client.queue_down(std::span<const std::byte>{}, 0));

    for (int i = 0; i < 1000; ++i) {
        g_now += 10'000'000;
        client.poll(g_now);
    }

    const auto stats = client.stats();
    EXPECT_EQ(client.state(), ipc::IpClient::State::Disconnected);
    EXPECT_GT(stats.connect_attempts, 1ULL);
    EXPECT_GT(stats.connect_failures, 0ULL);
    EXPECT_EQ(stats.connects, 0ULL);
    EXPECT_EQ(stats.tx_down_rejected, 1ULL);
    EXPECT_FALSE(client.try_take_up(up));
}

TEST(IpcClient, HandshakeTimeoutRetries) {
    g_now = 2'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};

    client.poll(g_now);
    ASSERT_TRUE(peer.accept());
    ASSERT_TRUE(peer.recv());

    g_now += 2'000'000'001;
    client.poll(g_now);

    auto stats = client.stats();
    EXPECT_EQ(client.state(), ipc::IpClient::State::Disconnected);
    EXPECT_EQ(stats.handshake_timeouts, 1ULL);
    EXPECT_EQ(stats.handshake_failures, 1ULL);
    EXPECT_EQ(stats.disconnects, 1ULL);

    const auto attempts = stats.connect_attempts;
    g_now += 99'000'000;
    client.poll(g_now);
    EXPECT_EQ(client.stats().connect_attempts, attempts);

    g_now += 1'000'000;
    client.poll(g_now);
    EXPECT_EQ(client.stats().connect_attempts, attempts + 1);
    ASSERT_TRUE(peer.accept());
}

TEST(IpcClient, VersionMismatchIsFatal) {
    g_now = 3'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};

    handshake(client, peer, g_now, 2);

    auto stats = client.stats();
    EXPECT_EQ(client.state(), ipc::IpClient::State::Fatal);
    EXPECT_EQ(stats.handshake_failures, 1ULL);
    EXPECT_EQ(stats.last_error.code, link::ErrorCode::Protocol);
    EXPECT_EQ(stats.handshakes, 0ULL);

    const auto attempts = stats.connect_attempts;
    g_now += 10'000'000'000;
    client.poll(g_now);
    EXPECT_EQ(client.stats().connect_attempts, attempts);
}

TEST(IpcClient, UpDataSingleSlotKeepsLatest) {
    g_now = 4'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};
    handshake(client, peer, g_now);

    const std::array<std::byte, 4> first{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    const std::array<std::byte, 4> second{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
    const std::array<std::byte, 4> third{
        std::byte{0x00}, std::byte{0xFF}, std::byte{0x0A}, std::byte{0x00}};

    ASSERT_TRUE(send_up(peer, first, 1));
    ASSERT_TRUE(send_up(peer, second, 2));
    ASSERT_TRUE(send_up(peer, third, 3));
    client.poll(g_now);

    ipc::UpFrame up;
    ASSERT_TRUE(client.try_take_up(up));
    EXPECT_EQ(up.size, third.size());
    EXPECT_EQ(up.t_src_ns, 3);
    EXPECT_EQ(up.t_recv_ns, g_now);
    EXPECT_TRUE(std::equal(third.begin(), third.end(), up.payload.begin()));

    const auto stats = client.stats();
    EXPECT_EQ(stats.rx_up_frames, 3ULL);
    EXPECT_EQ(stats.up_overwritten, 2ULL);
    EXPECT_FALSE(client.try_take_up(up));
}

TEST(IpcClient, DownDataIsRateLimitedAndPayloadIsVerbatim) {
    g_now = 5'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};
    handshake(client, peer, g_now);

    const std::array<std::byte, 6> payload{
        std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
        std::byte{0x00}, std::byte{0xFF}};

    ASSERT_TRUE(client.queue_down(payload, 12345));
    client.poll(g_now);

    auto frame = peer.recv();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->header.msg_type, link::kMsgDownData);
    auto down = link::decode_down_data(frame->body);
    ASSERT_TRUE(down);
    EXPECT_EQ(down->channel, link::kChannelCustomByteBlock);
    EXPECT_EQ(down->t_recv_ns, 12345);
    EXPECT_EQ(down->payload.size(), payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), down->payload.begin()));
    EXPECT_EQ(client.stats().tx_down_frames, 1ULL);

    ASSERT_TRUE(client.queue_down(payload, 22222));
    client.poll(g_now);
    auto limited = peer.recv();
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().code, link::ErrorCode::WouldBlock);

    g_now += 20'000'000;
    client.poll(g_now);
    auto second = peer.recv();
    ASSERT_TRUE(second);
    auto down2 = link::decode_down_data(second->body);
    ASSERT_TRUE(down2);
    EXPECT_EQ(down2->t_recv_ns, 22222);
    EXPECT_EQ(client.stats().tx_down_frames, 2ULL);
}

TEST(IpcClient, HealthTimeoutMarksOfflineButKeepsSession) {
    g_now = 6'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};
    handshake(client, peer, g_now);

    ASSERT_TRUE(send_health(peer, 0b111, 0));
    client.poll(g_now);
    auto stats = client.stats();
    EXPECT_TRUE(stats.rmcs_alive);
    EXPECT_EQ(stats.rx_health_frames, 1ULL);
    EXPECT_EQ(stats.last_health_state, 0b111U);

    g_now += 3'000'000'001;
    client.poll(g_now);
    stats = client.stats();
    EXPECT_FALSE(stats.rmcs_alive);
    EXPECT_EQ(stats.health_timeouts, 1ULL);
    EXPECT_EQ(client.state(), ipc::IpClient::State::Active);
}

TEST(IpcClient, UnknownMessageIsIgnored) {
    g_now = 7'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};
    handshake(client, peer, g_now);

    ASSERT_TRUE(peer.send(100, std::span<const std::byte>{}));
    client.poll(g_now);

    EXPECT_EQ(client.state(), ipc::IpClient::State::Active);
    EXPECT_EQ(client.stats().rx_unknown_frames, 1ULL);
}

TEST(IpcClient, MalformedFrameDropsSession) {
    g_now = 8'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};
    handshake(client, peer, g_now);

    std::array<std::byte, link::kHeaderBytes> garbage{};
    garbage[0] = std::byte{0x00};
    ASSERT_TRUE(peer.send_raw(garbage));
    client.poll(g_now);

    EXPECT_EQ(client.state(), ipc::IpClient::State::Disconnected);
    EXPECT_EQ(client.stats().last_error.code, link::ErrorCode::Protocol);
    EXPECT_EQ(client.stats().disconnects, 1ULL);
}

TEST(IpcClient, PeerCloseSchedulesReconnect) {
    g_now = 9'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer)};
    handshake(client, peer, g_now);

    peer.session->close();
    client.poll(g_now);

    auto stats = client.stats();
    EXPECT_EQ(client.state(), ipc::IpClient::State::Disconnected);
    EXPECT_EQ(stats.disconnects, 1ULL);

    const auto attempts = stats.connect_attempts;
    g_now += 99'000'000;
    client.poll(g_now);
    EXPECT_EQ(client.stats().connect_attempts, attempts);

    g_now += 1'000'000;
    client.poll(g_now);
    EXPECT_EQ(client.stats().connect_attempts, attempts + 1);
}

TEST(IpcClient, DownEagainDropsAndCounts) {
    g_now = 10'000'000'000;
    FakePeer peer;
    ipc::IpClient client{make_config(peer, 1024, 1024)};
    handshake(client, peer, g_now);

    const std::array<std::byte, 300> payload = [] {
        std::array<std::byte, 300> bytes{};
        bytes.fill(std::byte{0x5A});
        return bytes;
    }();

    for (int i = 0; i < 200; ++i) {
        g_now += 20'000'000;
        EXPECT_TRUE(client.queue_down(payload, g_now));
        client.poll(g_now);
    }

    const auto stats = client.stats();
    EXPECT_GT(stats.tx_down_frames, 0ULL);
    EXPECT_GT(stats.tx_down_dropped, 0ULL);
}

TEST(IpcClient, PeercredMismatchIsFatal) {
    g_now = 11'000'000'000;
    FakePeer peer;
    auto config = make_config(peer);
    config.allowed_peer_uid = static_cast<std::int32_t>(::geteuid()) + 1;
    ipc::IpClient client{std::move(config)};

    client.poll(g_now);

    const auto stats = client.stats();
    EXPECT_EQ(client.state(), ipc::IpClient::State::Fatal);
    EXPECT_EQ(stats.connect_attempts, 1ULL);
    EXPECT_EQ(stats.last_error.code, link::ErrorCode::Protocol);
}

TEST(IpcClient, InvalidSocketPathIsFatal) {
    g_now = 12'000'000'000;
    ipc::Config config;
    config.socket_path = std::string(200, 'x');
    config.now_ns = &fake_now;

    ipc::IpClient client{std::move(config)};

    EXPECT_EQ(client.state(), ipc::IpClient::State::Fatal);
    EXPECT_EQ(client.stats().last_error.code, link::ErrorCode::InvalidArgument);
}

} // namespace
