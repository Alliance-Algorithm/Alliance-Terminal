#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "rm/terminal.pb.h"
#include "rm_proto/topics.hpp"

namespace rm::proto {

namespace detail {

// 吃掉 topics.inc 每行末尾的逗号，不参与任何编码
struct Sentinel {};

} // namespace detail

// 32 条官方消息的类型安全联合体，顺序与 topics.inc 一致（下标 = topic_id - 1）
#define X(id, name, dir, qos, src, type) type,
using Payload = std::variant<
#include "topics.inc"
    detail::Sentinel>;
#undef X

static_assert(
    std::variant_size_v<Payload> == kTopicCount + 1, "topics.inc 的条数与 Payload 不一致");

struct Encoded {
    std::uint32_t topic_id = 0;
    std::vector<std::uint8_t> bytes;
};

namespace detail {

template <typename Msg>
inline std::optional<Payload> decode_as(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        return Payload{Msg{}};
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    Msg msg;
    if (!msg.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return std::nullopt;
    }
    return Payload{std::move(msg)};
}

template <typename Msg>
constexpr std::uint32_t topic_id_of() {
#define X(id, name, dir, qos, src, type)       \
    if constexpr (std::is_same_v<Msg, type>) { \
        return static_cast<std::uint32_t>(id); \
    } else
#include "topics.inc"
#undef X
    { return 0U; }
}

} // namespace detail

// 解析失败（截断、非法 wire format）返回 nullopt；未知字段按 protobuf 语义忽略
inline std::optional<Payload> decode(std::uint32_t topic_id, std::span<const std::uint8_t> bytes) {
    switch (topic_id) {
#define X(id, name, dir, qos, src, type) \
    case id: return detail::decode_as<type>(bytes);
#include "topics.inc"
#undef X
    default: return std::nullopt;
    }
}

inline std::optional<Payload> decode(const TopicDef& topic, std::span<const std::uint8_t> bytes) {
    return decode(topic.id, bytes);
}

inline Encoded encode(const Payload& payload) {
    return std::visit(
        [](const auto& msg) -> Encoded {
            using Msg = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<Msg, detail::Sentinel>) {
                return Encoded{};
            } else {
                std::string out;
                if (!msg.SerializeToString(&out)) {
                    return Encoded{};
                }
                return Encoded{
                    detail::topic_id_of<Msg>(),
                    std::vector<std::uint8_t>(out.begin(), out.end()),
                };
            }
        },
        payload);
}

template <typename Msg>
inline Encoded encode(const Msg& msg) {
    return encode(Payload{msg});
}

template <typename Msg>
inline constexpr std::uint32_t kTopicIdOf = detail::topic_id_of<Msg>();

} // namespace rm::proto
