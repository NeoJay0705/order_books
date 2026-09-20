#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace order_books::benchmark {

struct ThreadResourceError {
  std::string code;
  std::string detail;
};

struct ThreadResourceSample {
  std::uint64_t tid{};
  std::string role;
  std::uint64_t cpu_runtime_ns{};
  std::uint64_t runqueue_wait_ns{};
  std::uint64_t sched_timeslices{};
  std::uint64_t voluntary_context_switches{};
  std::uint64_t involuntary_context_switches{};
  std::optional<std::uint64_t> cpu_migrations;
};

struct ThreadResourceSnapshot {
  std::vector<ThreadResourceSample> samples;
};

struct ThreadResourceDelta {
  ThreadResourceSample sample;
};

using ThreadResourceSnapshotResult =
    std::variant<ThreadResourceSnapshot, ThreadResourceError>;
using ThreadResourceDeltaResult =
    std::variant<std::vector<ThreadResourceDelta>, ThreadResourceError>;

[[nodiscard]] ThreadResourceSnapshotResult parse_thread_resource_sample(
    std::uint64_t tid, std::string role, std::string_view status,
    std::string_view schedstat, std::string_view sched);

[[nodiscard]] ThreadResourceSnapshotResult capture_thread_resources(
    std::span<const std::string> expected_roles);

[[nodiscard]] ThreadResourceDeltaResult subtract_thread_resources(
    const ThreadResourceSnapshot& before,
    const ThreadResourceSnapshot& after);

[[nodiscard]] std::optional<std::uint64_t> per_million(
    std::uint64_t value, std::uint64_t commands) noexcept;

}  // namespace order_books::benchmark
