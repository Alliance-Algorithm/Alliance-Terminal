#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <string_view>
#include <utility>

#include "core/clock.hpp"
#include "core/error.hpp"
#include "ipc/ipc_client.hpp"

namespace {

namespace core = teleop::core;
namespace ipc = teleop::ipc;
namespace link = teleop::link;

std::int64_t g_log_start_ns = 0;

void log_line(const char* level, const char* module, const char* format, ...) {
    const auto now = core::now_ns();
    const auto elapsed_s = now >= g_log_start_ns ? (now - g_log_start_ns) / 1'000'000'000 : 0;

    std::printf("[%04lld][%s][%s] ", static_cast<long long>(elapsed_s), level, module);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::putchar('\n');
    std::fflush(stdout);
}

struct Options {
    std::string socket_path;
    double duration_s = 10.0;
    unsigned down_hz = 50;
    bool stats = true;
    int peer_uid = -1;
    std::string client_version = "ipc-probe/0.1.0";
};

void print_usage(const char* argv0) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "  --socket PATH          control socket (default: "
        "$HOME/.local/state/teleop/run/rmcs/control.sock)\n"
        "  --duration SEC         run time in seconds (default: 10)\n"
        "  --down-hz HZ           DOWN_DATA rate, 0 disables (default: 50)\n"
        "  --stats                print per-second status (default: on)\n"
        "  --peer-uid UID         expected peer uid, -1 disables (default: -1)\n"
        "  --client-version STR   Hello.client_version (default: ipc-probe/0.1.0)\n"
        "  --help                 show this help\n"
        "\n"
        "Without a running terminal bridge the probe keeps backing off and exits cleanly.\n",
        argv0);
}

std::string default_socket_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return std::string{home} + "/.local/state/teleop/run/rmcs/control.sock";
}

bool parse_double(const char* text, double& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool parse_unsigned(const char* text, unsigned& out) {
    if (text == nullptr || *text == '\0' || *text == '-') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT_MAX) {
        return false;
    }
    out = static_cast<unsigned>(value);
    return true;
}

bool parse_int(const char* text, int& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < INT_MIN || value > INT_MAX) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

std::string format_error(const link::Error& error) {
    if (error.code == link::ErrorCode::None && error.context.empty() && error.sys_errno == 0) {
        return "none";
    }
    std::string out{link::error_code_name(error.code)};
    if (!error.context.empty()) {
        out += ':';
        out.append(error.context.data(), error.context.size());
    }
    if (error.sys_errno != 0) {
        out += " errno=";
        out += std::to_string(error.sys_errno);
        out += " (";
        const char* text = std::strerror(error.sys_errno);
        out += text != nullptr ? text : "?";
        out += ')';
    }
    return out;
}

const char* state_name(ipc::IpClient::State state) {
    switch (state) {
    case ipc::IpClient::State::Disconnected: return "disconnected";
    case ipc::IpClient::State::Handshaking: return "handshaking";
    case ipc::IpClient::State::Active: return "active";
    case ipc::IpClient::State::Fatal: return "fatal";
    }
    return "unknown";
}

std::uint64_t dropped_count(const ipc::Stats& stats) {
    return stats.tx_down_dropped + stats.tx_down_rejected + stats.tx_down_queue_overflow
         + stats.up_overwritten;
}

void log_status(
    const ipc::Stats& stats, std::uint64_t up_taken, std::uint64_t last_up_taken,
    std::uint64_t last_down_frames) {
    const auto up_delta = up_taken - last_up_taken;
    const auto down_delta = stats.tx_down_frames - last_down_frames;

    log_line(
        "INFO", "IPC", "state=%s alive=%d rx_up=%llu tx_down=%llu drop=%llu gap=%llu peer_uid=%d",
        state_name(stats.state), stats.rmcs_alive ? 1 : 0,
        static_cast<unsigned long long>(up_delta), static_cast<unsigned long long>(down_delta),
        static_cast<unsigned long long>(dropped_count(stats)),
        static_cast<unsigned long long>(stats.rx_gaps), static_cast<int>(stats.peer_uid));
}

} // namespace

int main(int argc, char** argv) {
    g_log_start_ns = core::now_ns();

    Options options;
    options.socket_path = default_socket_path();

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--stats") {
            options.stats = true;
            continue;
        }
        if (arg == "--socket") {
            if (i + 1 >= argc) {
                log_line("ERROR", "IPC", "--socket requires a value");
                return 3;
            }
            options.socket_path = argv[++i];
            continue;
        }
        if (arg == "--duration") {
            if (i + 1 >= argc || !parse_double(argv[i + 1], options.duration_s)
                || options.duration_s <= 0.0 || options.duration_s > 3600.0) {
                log_line("ERROR", "IPC", "invalid --duration");
                return 3;
            }
            ++i;
            continue;
        }
        if (arg == "--down-hz") {
            if (i + 1 >= argc || !parse_unsigned(argv[i + 1], options.down_hz)
                || options.down_hz > 1000) {
                log_line("ERROR", "IPC", "invalid --down-hz (0..1000)");
                return 3;
            }
            ++i;
            continue;
        }
        if (arg == "--peer-uid") {
            if (i + 1 >= argc || !parse_int(argv[i + 1], options.peer_uid)
                || options.peer_uid < -1) {
                log_line("ERROR", "IPC", "invalid --peer-uid (>= -1)");
                return 3;
            }
            ++i;
            continue;
        }
        if (arg == "--client-version") {
            if (i + 1 >= argc) {
                log_line("ERROR", "IPC", "--client-version requires a value");
                return 3;
            }
            options.client_version = argv[++i];
            continue;
        }
        log_line(
            "ERROR", "IPC", "unknown argument '%.*s'", static_cast<int>(arg.size()), arg.data());
        return 3;
    }

    if (options.socket_path.empty()) {
        log_line("ERROR", "IPC", "socket path is empty and HOME is not set");
        return 3;
    }
    if (options.client_version.empty() || options.client_version.size() > 200) {
        log_line("ERROR", "IPC", "client version must be 1..200 bytes");
        return 3;
    }

    ipc::Config config;
    config.socket_path = options.socket_path;
    config.client_version = options.client_version;
    config.down_hz = options.down_hz;
    config.allowed_peer_uid = options.peer_uid;

    ipc::IpClient client{std::move(config)};
    if (client.state() == ipc::IpClient::State::Fatal) {
        const auto stats = client.stats();
        log_line("ERROR", "IPC", "fatal reason=%s", format_error(stats.last_error).c_str());
        return 2;
    }

    const std::int64_t start = core::now_ns();
    const auto duration_ns = static_cast<std::int64_t>(options.duration_s * 1e9);
    const std::int64_t down_period_ns =
        options.down_hz == 0 ? 0 : 1'000'000'000 / static_cast<std::int64_t>(options.down_hz);

    log_line(
        "INFO", "IPC", "probe start socket=%s duration=%.0fs down_hz=%u peer_uid=%d",
        options.socket_path.c_str(), options.duration_s, options.down_hz, options.peer_uid);

    constexpr std::array<std::byte, 6> kDownSample{std::byte{0x0A}, std::byte{0x04},
                                                   std::byte{0xDE}, std::byte{0xAD},
                                                   std::byte{0xBE}, std::byte{0xEF}};

    std::int64_t now = start;
    std::int64_t next_stats = start + 1'000'000'000;
    std::int64_t next_down = start;
    std::uint64_t up_taken = 0;
    std::uint64_t last_up_taken = 0;
    std::uint64_t last_down_frames = 0;
    auto last_state = client.state();
    auto last_alive = client.stats().rmcs_alive;

    while (now - start < duration_ns) {
        client.poll(now);
        const auto stats = client.stats();

        if (stats.state == ipc::IpClient::State::Fatal) {
            log_line("ERROR", "IPC", "fatal reason=%s", format_error(stats.last_error).c_str());
            return 2;
        }

        if (stats.state != last_state) {
            if (stats.state == ipc::IpClient::State::Active) {
                const std::string protocol{client.protocol_rev()};
                log_line(
                    "INFO", "UDS", "active peer_uid=%d protocol=%s",
                    static_cast<int>(stats.peer_uid), protocol.c_str());
            } else if (stats.state == ipc::IpClient::State::Handshaking) {
                log_line("INFO", "UDS", "handshaking");
            } else if (stats.state == ipc::IpClient::State::Disconnected) {
                if (last_state != ipc::IpClient::State::Disconnected) {
                    log_line(
                        "WARN", "UDS", "disconnected reason=%s",
                        format_error(stats.last_error).c_str());
                }
            }
            last_state = stats.state;
        }

        if (stats.rmcs_alive != last_alive) {
            log_line(
                stats.rmcs_alive ? "INFO" : "WARN", "IPC", "rmcs_alive=%d",
                stats.rmcs_alive ? 1 : 0);
            last_alive = stats.rmcs_alive;
        }

        ipc::UpFrame up;
        while (client.try_take_up(up)) {
            ++up_taken;
        }

        if (options.down_hz > 0 && client.state() == ipc::IpClient::State::Active
            && now >= next_down) {
            (void)client.queue_down(kDownSample, now);
            next_down = now + down_period_ns;
        }

        if (options.stats && now >= next_stats) {
            log_status(stats, up_taken, last_up_taken, last_down_frames);
            last_up_taken = up_taken;
            last_down_frames = stats.tx_down_frames;
            next_stats = now + 1'000'000'000;
        }

        auto next_event = client.next_wakeup_ns();
        if (options.stats) {
            next_event = std::min(next_event, next_stats);
        }
        if (options.down_hz > 0 && client.state() == ipc::IpClient::State::Active) {
            next_event = std::min(next_event, next_down);
        }
        next_event = std::min(next_event, start + duration_ns);

        auto wait_ns = next_event - now;
        if (wait_ns < 0) {
            wait_ns = 0;
        }
        auto wait_ms = static_cast<int>(wait_ns / 1'000'000);
        if (wait_ms > 10) {
            wait_ms = 10;
        }

        const int fd = client.fd();
        if (fd >= 0) {
            pollfd pfd{fd, POLLIN, 0};
            (void)::poll(&pfd, 1, wait_ms);
        } else {
            (void)::poll(nullptr, 0, wait_ms);
        }
        now = core::now_ns();
    }

    const auto stats = client.stats();
    const auto level = stats.last_error.code == link::ErrorCode::None ? "INFO" : "WARN";
    log_line(
        level, "IPC",
        "probe stop state=%s up_total=%llu down_total=%llu drop=%llu gap=%llu peer_uid=%d "
        "last_error=%s",
        state_name(stats.state), static_cast<unsigned long long>(up_taken),
        static_cast<unsigned long long>(stats.tx_down_frames),
        static_cast<unsigned long long>(dropped_count(stats)),
        static_cast<unsigned long long>(stats.rx_gaps), static_cast<int>(stats.peer_uid),
        format_error(stats.last_error).c_str());
    return 0;
}
