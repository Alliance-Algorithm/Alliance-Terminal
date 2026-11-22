#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "teleop_ipc/seqpacket.hpp"

namespace {

using teleop::link::ErrorCode;
using teleop::link::Listener;
using teleop::link::Socket;
using teleop::link::SocketOptions;
using teleop::link::UnixAddress;

std::string unique_name() {
    static std::atomic<int> counter{0};
    return "teleop-ipc-test-" + std::to_string(::getpid()) + "-"
         + std::to_string(counter.fetch_add(1));
}

bool equal_bytes(std::span<const std::byte> lhs, std::span<const std::byte> rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

std::vector<std::byte> pattern(std::size_t size, std::uint8_t seed = 0xA5) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((seed + i) & 0xFFU);
    }
    return out;
}

TEST(Seqpacket, AbstractAddressEndToEnd) {
    const auto name = unique_name();
    auto address = UnixAddress::abstract(name);
    ASSERT_TRUE(address.has_value()) << teleop::link::error_code_name(address.error().code);

    auto listener = Listener::open(*address);
    ASSERT_TRUE(listener.has_value()) << teleop::link::error_code_name(listener.error().code);

    auto client = Socket::connect(*address);
    ASSERT_TRUE(client.has_value()) << teleop::link::error_code_name(client.error().code);

    auto server = listener->accept();
    ASSERT_TRUE(server.has_value()) << teleop::link::error_code_name(server.error().code);

    const auto outbound = pattern(128);
    ASSERT_TRUE(client->send(outbound).has_value());

    std::vector<std::byte> inbound(256);
    const auto received = server->recv(inbound);
    ASSERT_TRUE(received.has_value()) << teleop::link::error_code_name(received.error().code);
    EXPECT_EQ(received->length, outbound.size());
    EXPECT_FALSE(received->truncated);
    EXPECT_TRUE(equal_bytes(
        std::span<const std::byte>(inbound).first(received->length),
        std::span<const std::byte>(outbound)));
}

TEST(Seqpacket, PathAddressEndToEnd) {
    const auto name = unique_name();
    const auto path = std::filesystem::temp_directory_path() / (name + ".sock");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    auto address = UnixAddress::path(path.string());
    ASSERT_TRUE(address.has_value());

    auto listener = Listener::open(*address);
    ASSERT_TRUE(listener.has_value()) << teleop::link::error_code_name(listener.error().code);

    auto client = Socket::connect(*address);
    ASSERT_TRUE(client.has_value()) << teleop::link::error_code_name(client.error().code);

    auto server = listener->accept();
    ASSERT_TRUE(server.has_value());

    const auto outbound = pattern(64);
    ASSERT_TRUE(server->send(outbound).has_value());
    std::vector<std::byte> inbound(128);
    const auto received = client->recv(inbound);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->length, outbound.size());

    client->close();
    server->close();
    listener->close();
    std::filesystem::remove(path, ignored);
}

TEST(Seqpacket, SocketPairSendReceive) {
    auto pair = Socket::pair();
    ASSERT_TRUE(pair.has_value()) << teleop::link::error_code_name(pair.error().code);

    auto first = std::move(pair->first);
    auto second = std::move(pair->second);

    const auto outbound = pattern(512);
    ASSERT_TRUE(first.send(outbound).has_value());

    std::vector<std::byte> inbound(1024);
    const auto received = second.recv(inbound);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->length, outbound.size());
    EXPECT_TRUE(equal_bytes(
        std::span<const std::byte>(inbound).first(received->length),
        std::span<const std::byte>(outbound)));
}

TEST(Seqpacket, EmptySocketWouldBlock) {
    auto pair = Socket::pair();
    ASSERT_TRUE(pair.has_value());

    std::vector<std::byte> buffer(128);
    const auto received = pair->second.recv(buffer);
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().code, ErrorCode::WouldBlock);
}

TEST(Seqpacket, AcceptWithoutPeerWouldBlock) {
    const auto name = unique_name();
    auto address = UnixAddress::abstract(name);
    ASSERT_TRUE(address.has_value());
    auto listener = Listener::open(*address);
    ASSERT_TRUE(listener.has_value());

    const auto accepted = listener->accept();
    ASSERT_FALSE(accepted.has_value());
    EXPECT_EQ(accepted.error().code, ErrorCode::WouldBlock);
}

TEST(Seqpacket, PeerCloseIsReported) {
    auto pair = Socket::pair();
    ASSERT_TRUE(pair.has_value());
    auto first = std::move(pair->first);
    auto second = std::move(pair->second);
    first.close();

    std::vector<std::byte> buffer(128);
    const auto received = second.recv(buffer);
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().code, ErrorCode::Closed);
}

TEST(Seqpacket, DetectsTruncatedRecord) {
    auto pair = Socket::pair();
    ASSERT_TRUE(pair.has_value());
    auto sender = std::move(pair->first);
    auto receiver = std::move(pair->second);

    const auto outbound = pattern(100);
    ASSERT_TRUE(sender.send(outbound).has_value());

    std::vector<std::byte> small(40);
    const auto received = receiver.recv(small);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->length, outbound.size());
    EXPECT_TRUE(received->truncated);
}

TEST(Seqpacket, DetectsOversizeRecord) {
    auto pair = Socket::pair();
    ASSERT_TRUE(pair.has_value());
    auto sender = std::move(pair->first);
    auto receiver = std::move(pair->second);

    const auto outbound = pattern(70000);
    ASSERT_TRUE(sender.send(outbound).has_value());

    std::vector<std::byte> buffer(65536);
    const auto received = receiver.recv(buffer);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->length, outbound.size());
    EXPECT_TRUE(received->truncated);
}

TEST(Seqpacket, SendWouldBlockWhenPeerDoesNotRead) {
    auto pair = Socket::pair(SocketOptions{.rcvbuf_bytes = 2048, .sndbuf_bytes = 2048});
    ASSERT_TRUE(pair.has_value());
    auto sender = std::move(pair->first);
    auto receiver = std::move(pair->second);
    (void)receiver;

    const auto chunk = pattern(512);
    bool would_block = false;
    for (int i = 0; i < 4096; ++i) {
        const auto result = sender.send(chunk);
        if (!result) {
            EXPECT_EQ(result.error().code, ErrorCode::WouldBlock);
            would_block = true;
            break;
        }
    }
    EXPECT_TRUE(would_block);
}

TEST(Seqpacket, PeerCredentialsMatchCurrentUser) {
    auto pair = Socket::pair();
    ASSERT_TRUE(pair.has_value());
    const auto cred = pair->first.peer_cred();
    ASSERT_TRUE(cred.has_value()) << teleop::link::error_code_name(cred.error().code);
    EXPECT_EQ(cred->uid, ::geteuid());
    EXPECT_GT(cred->pid, 0);
}

TEST(Seqpacket, RejectsBadAddresses) {
    EXPECT_FALSE(UnixAddress::path("").has_value());
    EXPECT_FALSE(UnixAddress::abstract("").has_value());
    EXPECT_FALSE(UnixAddress::path(std::string(200, 'a')).has_value());
    EXPECT_FALSE(UnixAddress::abstract(std::string(200, 'a')).has_value());
}

TEST(Seqpacket, ConnectToMissingPathFails) {
    const auto path = std::filesystem::temp_directory_path() / (unique_name() + ".sock");
    auto address = UnixAddress::path(path.string());
    ASSERT_TRUE(address.has_value());
    const auto client = Socket::connect(*address);
    EXPECT_FALSE(client.has_value());
}

} // namespace
