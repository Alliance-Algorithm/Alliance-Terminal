#pragma once

#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>

namespace rm::proto {

enum class Direction : std::uint8_t { Up, Down };

// 本地的生产者：None = 不产生（未实现或纯下行），Rmcs = RMCS 发，Ui = 前端发
enum class Source : std::uint8_t { None, Rmcs, Ui };

struct TopicDef {
    std::uint32_t    id        = 0;
    std::string_view name;
    Direction        direction = Direction::Down;
    std::uint8_t     qos       = 0;
    Source           source    = Source::None;
};

inline constexpr std::uint32_t kTopicCount = 32;

namespace detail {

#define X(id, name, dir, qos, src, type)                                       \
    TopicDef {                                                                 \
        static_cast<std::uint32_t>(id), name, dir, static_cast<std::uint8_t>(qos), src \
    },
inline constexpr TopicDef kTopics[] = {
#include "topics.inc"
};
#undef X

constexpr bool ids_are_contiguous() {
    for (std::uint32_t i = 0; i < kTopicCount; ++i) {
        if (kTopics[i].id != i + 1) {
            return false;
        }
    }
    return true;
}

static_assert(std::size(kTopics) == kTopicCount, "topics.inc 必须正好 32 条");
static_assert(ids_are_contiguous(), "topic 编号必须从 1 连续递增，顺序与 topics.inc 一致");

}  // namespace detail

// 32 条 topic，按 id 升序（1 上行…32 下行）
inline std::span<const TopicDef> topics() {
    return detail::kTopics;
}

inline const TopicDef* find(std::uint32_t id) {
    for (const auto& def : detail::kTopics) {
        if (def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

inline const TopicDef* find(std::string_view name) {
    for (const auto& def : detail::kTopics) {
        if (def.name == name) {
            return &def;
        }
    }
    return nullptr;
}

inline bool is_up(const TopicDef& def) {
    return def.direction == Direction::Up;
}
inline bool is_down(const TopicDef& def) {
    return def.direction == Direction::Down;
}
inline bool sendable_by_ui(const TopicDef& def) {
    return def.source == Source::Ui;
}
inline bool from_rmcs(const TopicDef& def) {
    return def.source == Source::Rmcs;
}

}  // namespace rm::proto
