#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>

#include "core/clock.hpp"
#include "core/latest_value_register.hpp"
#include "teleop_ipc/control_link.hpp"
#include "teleop_ipc/seqpacket.hpp"
#include "teleop_ipc/session.hpp"

namespace teleop::ipc {

inline constexpr std::size_t kMaxPayloadBytes = 512;
inline constexpr std::size_t kDownQueueCapacity = 4;

using ConnectFn = std::function<link::Result<link::Socket>()>;

struct Config {
    std::string socket_path;
    std::string client_version{"ipc-probe/0.1.0"};
    std::uint32_t max_body_bytes{static_cast<std::uint32_t>(link::kControlMaxBodyBytes)};
    std::uint32_t down_hz{50};
    std::size_t rcvbuf_bytes{4096};
    std::size_t sndbuf_bytes{4096};
    std::int64_t handshake_timeout_ns{2'000'000'000};
    std::int64_t health_timeout_ns{3'000'000'000};
    std::int64_t backoff_min_ns{100'000'000};
    std::int64_t backoff_max_ns{2'000'000'000};
    std::uint32_t backoff_jitter_permille{200};
    std::int32_t allowed_peer_uid{-1};
    std::uint64_t session_id{0};
    core::NowFn now_ns{&core::now_ns};
    ConnectFn connect{};
};

struct UpFrame {
    std::array<std::byte, kMaxPayloadBytes> payload{};
    std::uint32_t size{0};
    std::uint64_t seq{0};
    std::int64_t t_src_ns{0};
    std::int64_t t_recv_ns{0};
};

struct Stats {
    enum class State : std::uint8_t {
        Disconnected,
        Handshaking,
        Active,
        Fatal,
    };

    State state{State::Disconnected};
    bool rmcs_alive{false};
    std::uint64_t connect_attempts{0};
    std::uint64_t connects{0};
    std::uint64_t connect_failures{0};
    std::uint64_t reconnects{0};
    std::uint64_t handshakes{0};
    std::uint64_t handshake_timeouts{0};
    std::uint64_t handshake_failures{0};
    std::uint64_t disconnects{0};
    std::uint64_t rx_up_frames{0};
    std::uint64_t rx_health_frames{0};
    std::uint64_t rx_unknown_frames{0};
    std::uint64_t rx_before_active{0};
    std::uint64_t rx_channel_mismatch{0};
    std::uint64_t rx_up_oversize{0};
    std::uint64_t rx_gaps{0};
    std::uint64_t rx_anomalies{0};
    std::uint64_t tx_down_frames{0};
    std::uint64_t tx_down_dropped{0};
    std::uint64_t tx_down_rejected{0};
    std::uint64_t tx_down_queue_overflow{0};
    std::uint64_t tx_down_cleared{0};
    std::uint64_t up_overwritten{0};
    std::uint64_t health_timeouts{0};
    std::size_t down_queue_depth{0};
    std::int64_t last_up_ns{0};
    std::int64_t last_health_ns{0};
    std::int64_t last_connect_attempt_ns{0};
    std::int64_t next_connect_ns{0};
    std::int32_t peer_uid{-1};
    std::uint32_t peer_proto_version{0};
    std::uint64_t peer_capabilities{0};
    std::uint32_t last_health_state{0};
    std::uint32_t last_health_load_pct{0};
    link::Error last_error{};
};

// 单线程拥有者：除 try_take_up / queue_down 外，所有方法都必须在同一个 IPC 线程调用。
// 没有对端是正常状态：连接失败只计数并退避，不抛异常、不退出。
class IpClient {
public:
    using State = Stats::State;

    explicit IpClient(Config config);
    ~IpClient() = default;

    IpClient(const IpClient&) = delete;
    IpClient& operator=(const IpClient&) = delete;
    IpClient(IpClient&&) = delete;
    IpClient& operator=(IpClient&&) = delete;

    // 非阻塞：连接/退避、握手、收、发、健康超时都在这里推进。
    void poll(std::int64_t now_ns);

    // 取最新上行；没有数据返回 false。可由消费线程调用。
    bool try_take_up(UpFrame& out);

    // 入下行有界队列；非 ACTIVE、队列满（丢最旧）、payload 过大都返回 false。可由生产线程调用。
    bool queue_down(std::span<const std::byte> payload, std::int64_t t_recv_ns);

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] bool rmcs_alive() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::int64_t next_wakeup_ns() const;

    [[nodiscard]] std::string_view protocol_rev() const noexcept;
    [[nodiscard]] std::uint32_t peer_proto_version() const noexcept;
    [[nodiscard]] std::uint64_t peer_capabilities() const noexcept;
    [[nodiscard]] std::int32_t peer_uid() const noexcept;

private:
    struct DownFrame {
        std::array<std::byte, kMaxPayloadBytes> payload{};
        std::uint32_t size{0};
        std::int64_t t_recv_ns{0};
    };

    void attempt_connect(std::int64_t now_ns);
    void send_pending_hello(std::int64_t now_ns);
    void drain_recv(std::int64_t now_ns);
    void handle_frame(const link::FrameView& frame, std::int64_t now_ns);
    void handle_hello_ack(std::span<const std::byte> body, std::int64_t now_ns);
    void handle_up_data(const link::FrameView& frame, std::int64_t now_ns);
    void handle_health(std::span<const std::byte> body, std::int64_t now_ns);
    void check_health(std::int64_t now_ns);
    void send_downlink(std::int64_t now_ns);
    void drop_session(std::int64_t now_ns, link::Error error);
    void fatal(link::Error error);
    void clear_down_queue();
    [[nodiscard]] std::int64_t next_backoff_ns();
    [[nodiscard]] std::uint64_t rng_next() noexcept;
    [[nodiscard]] std::int64_t down_period_ns() const noexcept;

    Config config_;
    core::NowFn now_fn_;
    std::atomic<State> state_{State::Disconnected};
    std::optional<link::UnixAddress> address_;
    std::optional<link::Session> session_;

    std::array<std::byte, link::kHeaderBytes + kMaxPayloadBytes> rx_buffer_{};
    std::array<std::byte, link::kHeaderBytes + 256> hello_frame_{};
    std::size_t hello_frame_size_{0};
    bool hello_pending_{false};
    std::array<std::byte, link::kHeaderBytes + kMaxPayloadBytes> tx_frame_{};

    std::int64_t handshake_deadline_ns_{0};
    std::int64_t active_since_ns_{0};
    std::int64_t next_connect_ns_{0};
    std::int64_t next_down_ns_{0};
    std::int64_t backoff_ns_{0};
    std::uint64_t rng_state_{0x9E3779B97F4A7C15ULL};
    bool health_timed_out_{false};
    bool rmcs_alive_{false};
    std::int64_t last_health_ns_{0};
    std::uint32_t last_health_state_{0};
    std::uint32_t last_health_load_pct_{0};

    Stats stats_{};
    core::LatestValueRegister<UpFrame> up_slot_;

    mutable std::mutex down_mutex_;
    std::array<DownFrame, kDownQueueCapacity> down_queue_{};
    std::size_t down_head_{0};
    std::size_t down_count_{0};
    std::atomic<std::uint64_t> down_rejected_atomic_{0};
    std::atomic<std::uint64_t> down_queue_overflow_atomic_{0};

    std::string protocol_rev_;
    std::uint32_t peer_proto_version_{0};
    std::uint64_t peer_capabilities_{0};
    std::int32_t peer_uid_{-1};
};

inline IpClient::IpClient(Config config)
    : config_{std::move(config)} {
    if (config_.now_ns == nullptr) {
        config_.now_ns = &core::now_ns;
    }
    now_fn_ = config_.now_ns;

    if (config_.max_body_bytes == 0) {
        config_.max_body_bytes = static_cast<std::uint32_t>(link::kControlMaxBodyBytes);
    }
    config_.max_body_bytes = std::min<std::uint32_t>(
        config_.max_body_bytes, static_cast<std::uint32_t>(kMaxPayloadBytes));
    if (config_.down_hz > 1000) {
        config_.down_hz = 1000;
    }
    if (config_.backoff_min_ns <= 0) {
        config_.backoff_min_ns = 1'000'000;
    }
    if (config_.backoff_max_ns < config_.backoff_min_ns) {
        config_.backoff_max_ns = config_.backoff_min_ns;
    }
    config_.backoff_jitter_permille =
        std::min<std::uint32_t>(config_.backoff_jitter_permille, 1000);
    if (config_.handshake_timeout_ns <= 0) {
        config_.handshake_timeout_ns = 2'000'000'000;
    }
    if (config_.health_timeout_ns <= 0) {
        config_.health_timeout_ns = 3'000'000'000;
    }

    backoff_ns_ = config_.backoff_min_ns;
    next_connect_ns_ = now_fn_();

    if (!config_.connect) {
        auto address = link::UnixAddress::path(config_.socket_path);
        if (!address) {
            state_.store(State::Fatal);
            stats_.last_error = address.error();
            return;
        }
        address_.emplace(*address);
    }
}

inline void IpClient::poll(std::int64_t now_ns) {
    if (state_.load(std::memory_order_relaxed) == State::Fatal) {
        return;
    }
    if (!session_) {
        if (now_ns >= next_connect_ns_) {
            attempt_connect(now_ns);
        }
        return;
    }

    switch (state_.load(std::memory_order_relaxed)) {
    case State::Handshaking:
        send_pending_hello(now_ns);
        if (!session_) {
            return;
        }
        drain_recv(now_ns);
        if (!session_) {
            return;
        }
        if (state_.load(std::memory_order_relaxed) == State::Handshaking
            && now_ns >= handshake_deadline_ns_) {
            stats_.handshake_timeouts++;
            stats_.handshake_failures++;
            drop_session(now_ns, link::make_error(link::ErrorCode::Protocol, "handshake timeout"));
        }
        break;
    case State::Active:
        drain_recv(now_ns);
        if (!session_) {
            return;
        }
        check_health(now_ns);
        send_downlink(now_ns);
        break;
    default:
        break;
    }
}

inline void IpClient::attempt_connect(std::int64_t now_ns) {
    stats_.connect_attempts++;
    stats_.last_connect_attempt_ns = now_ns;

    link::Result<link::Socket> result;
    if (config_.connect) {
        result = config_.connect();
    } else {
        result = link::Socket::connect(
            *address_, link::SocketOptions{config_.rcvbuf_bytes, config_.sndbuf_bytes});
    }
    if (!result) {
        stats_.connect_failures++;
        stats_.last_error = result.error();
        next_connect_ns_ = now_ns + next_backoff_ns();
        return;
    }

    auto socket = std::move(*result);
    auto cred = socket.peer_cred();
    if (!cred) {
        fatal(cred.error());
        return;
    }
    peer_uid_ = static_cast<std::int32_t>(cred->uid);
    stats_.peer_uid = peer_uid_;
    if (config_.allowed_peer_uid >= 0
        && static_cast<uid_t>(config_.allowed_peer_uid) != cred->uid) {
        fatal(link::make_error(link::ErrorCode::Protocol, "peercred mismatch"));
        return;
    }

    stats_.connects++;
    if (stats_.connects > 1) {
        stats_.reconnects++;
    }

    stats_.last_error = link::Error{};
    stats_.last_up_ns = 0;
    stats_.last_health_ns = 0;
    stats_.last_health_state = 0;
    stats_.last_health_load_pct = 0;

    link::SessionOptions options{};
    options.role = link::Role::Connector;
    options.session_id = config_.session_id;
    options.max_body_bytes = config_.max_body_bytes;
    options.now_ns = config_.now_ns;
    session_.emplace(std::move(socket), options);
    session_->begin_handshake();

    up_slot_.clear();
    clear_down_queue();
    hello_pending_ = false;
    health_timed_out_ = false;
    rmcs_alive_ = false;
    last_health_ns_ = 0;
    active_since_ns_ = 0;
    handshake_deadline_ns_ = now_ns + config_.handshake_timeout_ns;
    state_.store(State::Handshaking);
    stats_.state = State::Handshaking;

    std::array<std::byte, 256> body{};
    link::Hello hello{};
    hello.proto_version = link::kControlProtoVersion;
    hello.capabilities = link::kCapUpData | link::kCapDownData | link::kCapHealth;
    hello.client_version = config_.client_version;

    auto body_size = link::encode_hello(hello, body);
    if (!body_size) {
        fatal(body_size.error());
        return;
    }
    auto frame_size = session_->prepare(
        link::kMsgHello, std::span<const std::byte>{body.data(), *body_size}, hello_frame_);
    if (!frame_size) {
        fatal(frame_size.error());
        return;
    }
    hello_frame_size_ = *frame_size;
    hello_pending_ = true;
    send_pending_hello(now_ns);
}

inline void IpClient::send_pending_hello(std::int64_t now_ns) {
    if (!hello_pending_ || !session_) {
        return;
    }
    auto result = session_->send_prepared(
        std::span<const std::byte>{hello_frame_.data(), hello_frame_size_});
    if (result) {
        hello_pending_ = false;
        return;
    }
    if (result.error().code == link::ErrorCode::WouldBlock) {
        return;
    }
    drop_session(now_ns, result.error());
}

inline void IpClient::drain_recv(std::int64_t now_ns) {
    for (int i = 0; i < 64 && session_; ++i) {
        auto frame = session_->recv(std::span<std::byte>{rx_buffer_});
        if (!frame) {
            if (frame.error().code == link::ErrorCode::WouldBlock) {
                return;
            }
            drop_session(now_ns, frame.error());
            return;
        }
        stats_.rx_gaps = session_->stats().rx_gaps;
        stats_.rx_anomalies = session_->stats().rx_anomalies;
        handle_frame(*frame, now_ns);
    }
}

inline void IpClient::handle_frame(const link::FrameView& frame, std::int64_t now_ns) {
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == State::Handshaking) {
        if (frame.header.msg_type == link::kMsgHelloAck) {
            handle_hello_ack(frame.body, now_ns);
        } else {
            stats_.rx_before_active++;
        }
        return;
    }
    if (current != State::Active) {
        return;
    }

    switch (frame.header.msg_type) {
    case link::kMsgUpData:
        handle_up_data(frame, now_ns);
        break;
    case link::kMsgHealth:
        handle_health(frame.body, now_ns);
        break;
    default:
        stats_.rx_unknown_frames++;
        break;
    }
}

inline void IpClient::handle_hello_ack(std::span<const std::byte> body, std::int64_t now_ns) {
    auto ack = link::decode_hello_ack(body);
    if (!ack) {
        drop_session(now_ns, ack.error());
        return;
    }

    peer_proto_version_ = ack->proto_version;
    peer_capabilities_ = ack->capabilities;
    protocol_rev_.assign(ack->protocol_rev.begin(), ack->protocol_rev.end());
    stats_.peer_proto_version = peer_proto_version_;
    stats_.peer_capabilities = peer_capabilities_;

    if (ack->proto_version != link::kControlProtoVersion) {
        stats_.handshake_failures++;
        fatal(link::make_error(link::ErrorCode::Protocol, "hello version mismatch"));
        return;
    }
    if (!session_) {
        return;
    }

    session_->activate();
    state_.store(State::Active);
    stats_.state = State::Active;
    stats_.handshakes++;
    hello_pending_ = false;
    handshake_deadline_ns_ = 0;
    active_since_ns_ = now_ns;
    last_health_ns_ = 0;
    health_timed_out_ = false;
    rmcs_alive_ = false;
    next_down_ns_ = now_ns;
    backoff_ns_ = config_.backoff_min_ns;
    stats_.last_error = link::Error{};
}

inline void IpClient::handle_up_data(const link::FrameView& frame, std::int64_t now_ns) {
    auto up = link::decode_up_data(frame.body);
    if (!up) {
        drop_session(now_ns, up.error());
        return;
    }
    if (up->channel != link::kChannelCustomControl) {
        stats_.rx_channel_mismatch++;
        return;
    }
    if (up->payload.size() > kMaxPayloadBytes) {
        stats_.rx_up_oversize++;
        return;
    }

    UpFrame out{};
    out.size = static_cast<std::uint32_t>(up->payload.size());
    std::copy(up->payload.begin(), up->payload.end(), out.payload.begin());
    out.seq = frame.header.seq;
    out.t_src_ns = up->t_src_ns;
    out.t_recv_ns = now_ns;
    up_slot_.set(std::move(out));

    stats_.rx_up_frames++;
    stats_.last_up_ns = now_ns;
}

inline void IpClient::handle_health(std::span<const std::byte> body, std::int64_t now_ns) {
    auto health = link::decode_health(body);
    if (!health) {
        drop_session(now_ns, health.error());
        return;
    }

    last_health_ns_ = now_ns;
    last_health_state_ = health->state;
    last_health_load_pct_ = health->load_pct;
    stats_.last_health_ns = now_ns;
    stats_.last_health_state = last_health_state_;
    stats_.last_health_load_pct = last_health_load_pct_;
    stats_.rx_health_frames++;
    health_timed_out_ = false;
    rmcs_alive_ = true;
}

inline void IpClient::check_health(std::int64_t now_ns) {
    if (health_timed_out_) {
        return;
    }
    const auto base = last_health_ns_ != 0 ? last_health_ns_ : active_since_ns_;
    if (base == 0) {
        return;
    }
    if (now_ns - base > config_.health_timeout_ns) {
        health_timed_out_ = true;
        stats_.health_timeouts++;
        rmcs_alive_ = false;
    }
}

inline std::int64_t IpClient::down_period_ns() const noexcept {
    if (config_.down_hz == 0) {
        return 0;
    }
    return std::max<std::int64_t>(1, 1'000'000'000 / static_cast<std::int64_t>(config_.down_hz));
}

inline void IpClient::send_downlink(std::int64_t now_ns) {
    if (!session_ || state_.load(std::memory_order_relaxed) != State::Active
        || config_.down_hz == 0) {
        return;
    }
    if (now_ns < next_down_ns_) {
        return;
    }

    DownFrame frame;
    {
        std::lock_guard lock{down_mutex_};
        if (down_count_ == 0) {
            return;
        }
        frame = down_queue_[down_head_];
        down_head_ = (down_head_ + 1) % kDownQueueCapacity;
        --down_count_;
    }
    next_down_ns_ = now_ns + down_period_ns();

    link::DownData down{};
    down.channel = link::kChannelCustomByteBlock;
    down.t_recv_ns = frame.t_recv_ns;
    down.payload = std::span<const std::byte>{frame.payload.data(), frame.size};

    std::array<std::byte, kMaxPayloadBytes> body{};
    auto body_size = link::encode_down_data(down, body);
    if (!body_size) {
        stats_.tx_down_rejected++;
        return;
    }
    auto prepared = session_->prepare(
        link::kMsgDownData, std::span<const std::byte>{body.data(), *body_size}, tx_frame_);
    if (!prepared) {
        if (prepared.error().code == link::ErrorCode::Closed
            || prepared.error().code == link::ErrorCode::Protocol) {
            drop_session(now_ns, prepared.error());
        } else {
            stats_.tx_down_rejected++;
        }
        return;
    }
    auto sent = session_->send_prepared(
        std::span<const std::byte>{tx_frame_.data(), *prepared});
    if (!sent) {
        if (sent.error().code == link::ErrorCode::WouldBlock) {
            stats_.tx_down_dropped++;
        } else {
            drop_session(now_ns, sent.error());
        }
        return;
    }
    stats_.tx_down_frames++;
}

inline bool IpClient::queue_down(std::span<const std::byte> payload, std::int64_t t_recv_ns) {
    if (state_.load(std::memory_order_relaxed) != State::Active || config_.down_hz == 0
        || payload.size() > kMaxPayloadBytes) {
        down_rejected_atomic_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard lock{down_mutex_};
    if (down_count_ == kDownQueueCapacity) {
        down_head_ = (down_head_ + 1) % kDownQueueCapacity;
        --down_count_;
        down_queue_overflow_atomic_.fetch_add(1, std::memory_order_relaxed);
    }
    const auto tail = (down_head_ + down_count_) % kDownQueueCapacity;
    auto& slot = down_queue_[tail];
    slot.size = static_cast<std::uint32_t>(payload.size());
    std::copy(payload.begin(), payload.end(), slot.payload.begin());
    slot.t_recv_ns = t_recv_ns;
    ++down_count_;
    return true;
}

inline bool IpClient::try_take_up(UpFrame& out) {
    return up_slot_.try_take(out);
}

inline IpClient::State IpClient::state() const noexcept {
    return state_.load(std::memory_order_relaxed);
}

inline bool IpClient::rmcs_alive() const noexcept {
    return rmcs_alive_;
}

inline int IpClient::fd() const noexcept {
    return session_ ? session_->fd() : -1;
}

inline std::int64_t IpClient::next_wakeup_ns() const {
    const auto now = now_fn_();
    if (state_.load(std::memory_order_relaxed) == State::Fatal) {
        return now;
    }
    if (!session_) {
        return next_connect_ns_;
    }
    if (state_.load(std::memory_order_relaxed) == State::Handshaking) {
        return handshake_deadline_ns_;
    }

    auto wake = now + 1'000'000'000;
    if (!health_timed_out_) {
        const auto base = last_health_ns_ != 0 ? last_health_ns_ : active_since_ns_;
        if (base != 0) {
            wake = std::min(wake, base + config_.health_timeout_ns + 1);
        }
    }
    {
        std::lock_guard lock{down_mutex_};
        if (down_count_ > 0) {
            wake = std::min(wake, next_down_ns_);
        }
    }
    return std::max(wake, now);
}

inline Stats IpClient::stats() const {
    Stats out = stats_;
    out.state = state_.load(std::memory_order_relaxed);
    out.rmcs_alive = rmcs_alive_;
    out.up_overwritten = up_slot_.overwritten();
    out.tx_down_rejected += down_rejected_atomic_.load(std::memory_order_relaxed);
    out.tx_down_queue_overflow +=
        down_queue_overflow_atomic_.load(std::memory_order_relaxed);
    out.next_connect_ns = next_connect_ns_;
    if (session_) {
        out.rx_gaps = session_->stats().rx_gaps;
        out.rx_anomalies = session_->stats().rx_anomalies;
    }
    {
        std::lock_guard lock{down_mutex_};
        out.down_queue_depth = down_count_;
    }
    return out;
}

inline std::string_view IpClient::protocol_rev() const noexcept {
    return protocol_rev_;
}

inline std::uint32_t IpClient::peer_proto_version() const noexcept {
    return peer_proto_version_;
}

inline std::uint64_t IpClient::peer_capabilities() const noexcept {
    return peer_capabilities_;
}

inline std::int32_t IpClient::peer_uid() const noexcept {
    return peer_uid_;
}

inline void IpClient::drop_session(std::int64_t now_ns, link::Error error) {
    if (session_) {
        session_->close();
        session_.reset();
    }
    state_.store(State::Disconnected);
    stats_.state = State::Disconnected;
    stats_.disconnects++;
    stats_.last_error = error;
    rmcs_alive_ = false;
    health_timed_out_ = false;
    hello_pending_ = false;
    last_health_ns_ = 0;
    active_since_ns_ = 0;
    up_slot_.clear();
    clear_down_queue();
    next_connect_ns_ = now_ns + next_backoff_ns();
}

inline void IpClient::fatal(link::Error error) {
    if (session_) {
        session_->fail();
        session_.reset();
    }
    state_.store(State::Fatal);
    stats_.state = State::Fatal;
    stats_.last_error = error;
    rmcs_alive_ = false;
    hello_pending_ = false;
    up_slot_.clear();
    clear_down_queue();
}

inline void IpClient::clear_down_queue() {
    std::lock_guard lock{down_mutex_};
    if (down_count_ > 0) {
        stats_.tx_down_cleared += down_count_;
    }
    down_head_ = 0;
    down_count_ = 0;
}

inline std::int64_t IpClient::next_backoff_ns() {
    const auto base = backoff_ns_;
    std::int64_t delay = base;
    const auto permille =
        std::min<std::uint32_t>(config_.backoff_jitter_permille, 1000);
    if (permille > 0) {
        const auto span = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(base) * permille) / 1000);
        if (span > 0) {
            const auto random = static_cast<std::int64_t>(
                rng_next() % static_cast<std::uint64_t>(2 * span + 1));
            delay = base + random - span;
        }
    }
    backoff_ns_ = std::min(config_.backoff_max_ns, base * 2);
    return delay > 0 ? delay : 0;
}

inline std::uint64_t IpClient::rng_next() noexcept {
    auto x = rng_state_;
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    rng_state_ = x;
    return x;
}

} // namespace teleop::ipc
