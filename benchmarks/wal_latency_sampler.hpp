#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace order_books::benchmark_wal {

constexpr std::uint64_t kMaxLatencySamples = 1'000'000U;

struct LatencyAggregate {
  std::uint64_t total_ns{};
  std::uint64_t max_ns{};
  std::uint64_t observed{};
};

inline bool checked_add(std::uint64_t& target, const std::uint64_t value) {
  if (target > std::numeric_limits<std::uint64_t>::max() - value) {
    return false;
  }
  target += value;
  return true;
}

inline std::uint64_t latency_sample_stride(const std::uint64_t group_count) {
  if (group_count == 0U) {
    return 1U;
  }
  const auto quotient = group_count / kMaxLatencySamples;
  const auto remainder = group_count % kMaxLatencySamples;
  return quotient + (remainder == 0U ? 0U : 1U);
}

inline bool observe_latency(std::vector<std::uint64_t>& samples,
                           LatencyAggregate& aggregate,
                           const std::uint64_t value,
                           const std::uint64_t sample_index,
                           const std::uint64_t sample_stride) {
  if (sample_stride == 0U) {
    return false;
  }
  const auto will_sample = sample_index % sample_stride == 0U;
  if (will_sample && samples.size() >= kMaxLatencySamples) {
    return false;
  }
  auto total_ns = aggregate.total_ns;
  auto observed = aggregate.observed;
  if (!checked_add(total_ns, value) || !checked_add(observed, 1U)) {
    return false;
  }
  aggregate.total_ns = total_ns;
  aggregate.observed = observed;
  aggregate.max_ns = std::max(aggregate.max_ns, value);
  if (will_sample) {
    samples.push_back(value);
  }
  return true;
}

}  // namespace order_books::benchmark_wal
