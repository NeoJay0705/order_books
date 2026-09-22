#include "wal_latency_sampler.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using order_books::benchmark_wal::LatencyAggregate;
using order_books::benchmark_wal::checked_add;
using order_books::benchmark_wal::latency_sample_stride;
using order_books::benchmark_wal::observe_latency;

bool check_stride_boundaries() {
  return latency_sample_stride(0) == 1 && latency_sample_stride(1) == 1 &&
         latency_sample_stride(1'000'000) == 1 && latency_sample_stride(1'000'001) == 2;
}

bool check_bounded_sampling_and_full_aggregate() {
  constexpr std::uint64_t input_count = 1'000'005;
  const auto stride = latency_sample_stride(input_count);
  std::vector<std::uint64_t> samples;
  LatencyAggregate aggregate;
  std::uint64_t expected_total = 0;
  for (std::uint64_t index = 0; index < input_count; ++index) {
    const auto value = index == input_count - 1U ? 2'000'000U : index % 97U;
    if (!observe_latency(samples, aggregate, value, index, stride) ||
        !checked_add(expected_total, value)) {
      return false;
    }
  }
  return samples.size() <= 1'000'000U && aggregate.observed == input_count &&
         aggregate.total_ns == expected_total && aggregate.max_ns == 2'000'000U;
}

bool check_overflow_is_rejected() {
  LatencyAggregate aggregate;
  aggregate.total_ns = std::numeric_limits<std::uint64_t>::max();
  std::vector<std::uint64_t> samples;
  return !observe_latency(samples, aggregate, 1U, 0U, 1U) &&
         aggregate.total_ns == std::numeric_limits<std::uint64_t>::max() &&
         aggregate.observed == 0U;
}

}  // namespace

int main() {
  return check_stride_boundaries() && check_bounded_sampling_and_full_aggregate() &&
                 check_overflow_is_rejected()
             ? 0
             : 1;
}
