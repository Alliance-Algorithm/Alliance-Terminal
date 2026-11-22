#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <poll.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
    bool latency = false;
    bool rtt = false;
    std::string record_path;
    std::string record_meta_path;
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
        "  --latency              collect UP_DATA one-way latency\n"
        "  --rtt                  send echo tokens and measure RTT (test bridge)\n"
        "  --record PATH          write per-sample latency CSV (implies --latency)\n"
        "  --record-meta PATH     write run metadata JSON (default: PATH.meta.json)\n"
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

struct LatencySummary {
    std::size_t count{0};
    std::size_t negative{0};
    std::int64_t min{0};
    std::int64_t p50{0};
    std::int64_t p95{0};
    std::int64_t p99{0};
    std::int64_t max{0};
};

LatencySummary summarize_latency(std::vector<std::int64_t>& samples) {
    LatencySummary summary;
    summary.count = samples.size();
    if (samples.empty()) {
        return summary;
    }

    std::sort(samples.begin(), samples.end());
    for (const auto value : samples) {
        if (value < 0) {
            ++summary.negative;
        }
    }

    const auto at = [&samples](double percentile) {
        const auto index = static_cast<std::size_t>((samples.size() - 1) * percentile);
        return samples[index];
    };

    summary.min = samples.front();
    summary.p50 = at(0.50);
    summary.p95 = at(0.95);
    summary.p99 = at(0.99);
    summary.max = samples.back();
    return summary;
}

void log_latency(const char* name, std::vector<std::int64_t>& samples) {
    const auto summary = summarize_latency(samples);
    if (summary.count == 0) {
        log_line("INFO", "IPC", "%s count=0", name);
        return;
    }

    const auto us = [](std::int64_t ns) { return static_cast<double>(ns) / 1000.0; };
    log_line(
        "INFO", "IPC",
        "%s count=%zu negative=%zu min=%.1fus p50=%.1fus p95=%.1fus p99=%.1fus "
        "max=%.1fus",
        name, summary.count, summary.negative, us(summary.min), us(summary.p50), us(summary.p95),
        us(summary.p99), us(summary.max));
}

std::string utc_now_string() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char value : text) {
        switch (value) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(value); break;
        }
    }
    return out;
}

void write_run_metadata(
    const Options& options, const std::string& run_id, const std::string& started_utc,
    std::int64_t start_ns, const ipc::Stats& stats, const LatencySummary& latency,
    std::uint64_t record_samples, std::uint64_t warmup_samples, std::string_view protocol_rev) {
    std::ofstream out{options.record_meta_path};
    if (!out) {
        log_line("ERROR", "IPC", "cannot open metadata file: %s", options.record_meta_path.c_str());
        return;
    }

    const auto error = format_error(stats.last_error);
    out << "{\n";
    out << "  \"run_id\": \"" << json_escape(run_id) << "\",\n";
    out << "  \"started_utc\": \"" << json_escape(started_utc) << "\",\n";
    out << "  \"start_mono_ns\": " << start_ns << ",\n";
    out << "  \"duration_s\": " << options.duration_s << ",\n";
    out << "  \"socket\": \"" << json_escape(options.socket_path) << "\",\n";
    out << "  \"record_csv\": \"" << json_escape(options.record_path) << "\",\n";
    out << "  \"down_hz\": " << options.down_hz << ",\n";
    out << "  \"peer_uid\": " << static_cast<int>(stats.peer_uid) << ",\n";
    out << "  \"client_version\": \"" << json_escape(options.client_version) << "\",\n";
    out << "  \"protocol_rev\": \"" << json_escape(protocol_rev) << "\",\n";
    out << "  \"clock\": \"CLOCK_MONOTONIC\",\n";
    out << "  \"state\": \"" << state_name(stats.state) << "\",\n";
    out << "  \"up_total\": " << stats.rx_up_frames << ",\n";
    out << "  \"down_total\": " << stats.tx_down_frames << ",\n";
    out << "  \"record_samples\": " << record_samples << ",\n";
    out << "  \"warmup_samples\": " << warmup_samples << ",\n";
    out << "  \"latency\": {\n";
    out << "    \"count\": " << latency.count << ",\n";
    out << "    \"negative\": " << latency.negative << ",\n";
    out << "    \"min_ns\": " << latency.min << ",\n";
    out << "    \"p50_ns\": " << latency.p50 << ",\n";
    out << "    \"p95_ns\": " << latency.p95 << ",\n";
    out << "    \"p99_ns\": " << latency.p99 << ",\n";
    out << "    \"max_ns\": " << latency.max << "\n";
    out << "  },\n";
    out << "  \"last_error\": \"" << json_escape(error) << "\"\n";
    out << "}\n";
}

std::array<std::byte, 62> make_token_payload(std::uint64_t token) {
    std::array<std::byte, 62> payload{};
    payload[0] = std::byte{0x0A};
    payload[1] = std::byte{0x3C};
    for (std::size_t i = 0; i < 8; ++i) {
        payload[2 + i] = static_cast<std::byte>((token >> ((7 - i) * 8U)) & 0xFFU);
    }
    return payload;
}

bool parse_token(const ipc::UpFrame& up, std::uint64_t& token) {
    if (up.size < 10 || up.payload[0] != std::byte{0x0A} || up.payload[1] != std::byte{0x3C}) {
        return false;
    }
    token = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        token = (token << 8U) | std::to_integer<std::uint64_t>(up.payload[2 + i]);
    }
    return true;
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
        if (arg == "--latency") {
            options.latency = true;
            continue;
        }
        if (arg == "--rtt") {
            options.rtt = true;
            continue;
        }
        if (arg == "--record") {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                log_line("ERROR", "IPC", "--record requires a path");
                return 3;
            }
            options.record_path = argv[++i];
            options.latency = true;
            continue;
        }
        if (arg == "--record-meta") {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                log_line("ERROR", "IPC", "--record-meta requires a path");
                return 3;
            }
            options.record_meta_path = argv[++i];
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
    if (options.rtt && options.down_hz == 0) {
        log_line("ERROR", "IPC", "--rtt requires --down-hz > 0");
        return 3;
    }
    if (!options.record_meta_path.empty() && options.record_path.empty()) {
        log_line("ERROR", "IPC", "--record-meta requires --record");
        return 3;
    }
    if (!options.record_path.empty() && options.record_meta_path.empty()) {
        options.record_meta_path = options.record_path + ".meta.json";
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
        "INFO", "IPC",
        "probe start socket=%s duration=%.0fs down_hz=%u peer_uid=%d latency=%d rtt=%d "
        "record=%s",
        options.socket_path.c_str(), options.duration_s, options.down_hz, options.peer_uid,
        options.latency ? 1 : 0, options.rtt ? 1 : 0,
        options.record_path.empty() ? "-" : options.record_path.c_str());

    const std::string run_id = utc_now_string();
    const std::string started_utc = run_id;
    std::ofstream record_csv;
    std::uint64_t record_samples = 0;
    std::uint64_t warmup_samples = 0;
    if (!options.record_path.empty()) {
        record_csv.open(options.record_path, std::ios::out | std::ios::trunc);
        if (!record_csv) {
            log_line("ERROR", "IPC", "cannot open record CSV: %s", options.record_path.c_str());
            return 3;
        }
        record_csv
            << "sample,seq,t_src_ns,t_recv_ns,latency_ns,payload_size,state,peer_uid,warmup\n";
    }

    // 默认下行样例使用 60 B 数据段，Echo 模式下上行也是 60 B。
    std::array<std::byte, 62> kDownSample{};
    kDownSample[0] = std::byte{0x0A};
    kDownSample[1] = std::byte{0x3C};
    for (std::size_t i = 0; i < 60; ++i) {
        kDownSample[2 + i] = static_cast<std::byte>(i & 0xFFU);
    }

    std::int64_t now = start;
    std::int64_t next_stats = start + 1'000'000'000;
    std::int64_t next_flush = start + 1'000'000'000;
    std::int64_t next_down = start;
    const std::int64_t measurement_start = start + 1'000'000'000;
    std::uint64_t up_taken = 0;
    std::uint64_t last_up_taken = 0;
    std::uint64_t last_down_frames = 0;
    std::uint64_t rtt_token = 0;
    std::uint64_t rtt_sent_count = 0;
    std::vector<std::int64_t> up_latency;
    std::vector<std::int64_t> rtt_samples;
    std::unordered_map<std::uint64_t, std::int64_t> rtt_sent;
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
            const auto latency = up.t_recv_ns - up.t_src_ns;
            const bool warmup = up.t_recv_ns < measurement_start;

            if (record_csv.is_open()) {
                record_csv << record_samples << ',' << up.seq << ',' << up.t_src_ns << ','
                           << up.t_recv_ns << ',' << latency << ',' << up.size << ','
                           << state_name(stats.state) << ',' << stats.peer_uid << ','
                           << (warmup ? 1 : 0) << '\n';
                ++record_samples;
                if (warmup) {
                    ++warmup_samples;
                }
            }

            if (options.latency && !warmup) {
                up_latency.push_back(latency);
            }
            if (options.rtt) {
                std::uint64_t token = 0;
                if (parse_token(up, token)) {
                    const auto sent = rtt_sent.find(token);
                    if (sent != rtt_sent.end()) {
                        rtt_samples.push_back(up.t_recv_ns - sent->second);
                        rtt_sent.erase(sent);
                    }
                }
            }
        }

        if (record_csv.is_open() && now >= next_flush) {
            record_csv.flush();
            next_flush = now + 1'000'000'000;
        }

        if (options.down_hz > 0 && client.state() == ipc::IpClient::State::Active
            && now >= next_down) {
            if (options.rtt) {
                const auto token = ++rtt_token;
                const auto payload = make_token_payload(token);
                if (client.queue_down(payload, now)) {
                    rtt_sent.emplace(token, now);
                    ++rtt_sent_count;
                }
            } else {
                (void)client.queue_down(kDownSample, now);
            }
            next_down = now + down_period_ns;
        }

        if (options.rtt && now >= next_stats) {
            for (auto it = rtt_sent.begin(); it != rtt_sent.end();) {
                if (now - it->second > 5'000'000'000) {
                    it = rtt_sent.erase(it);
                } else {
                    ++it;
                }
            }
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

    if (options.latency) {
        log_latency("up_latency", up_latency);
    }
    if (options.rtt) {
        log_latency("rtt", rtt_samples);
        const auto received = static_cast<std::uint64_t>(rtt_samples.size());
        const auto lost = rtt_sent_count > received ? rtt_sent_count - received : 0;
        log_line(
            "INFO", "IPC", "rtt_sent=%llu rtt_recv=%llu rtt_lost=%llu",
            static_cast<unsigned long long>(rtt_sent_count),
            static_cast<unsigned long long>(received), static_cast<unsigned long long>(lost));
    }

    if (record_csv.is_open()) {
        record_csv.flush();
        record_csv.close();
    }
    if (!options.record_meta_path.empty()) {
        const auto latency = summarize_latency(up_latency);
        const std::string protocol{client.protocol_rev()};
        write_run_metadata(
            options, run_id, started_utc, start, stats, latency, record_samples, warmup_samples,
            protocol);
    }
    return 0;
}
