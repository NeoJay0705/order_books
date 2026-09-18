#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace order_books {

struct HistogramSnapshot {
  std::uint64_t count{};
  std::uint64_t p50_microseconds{};
  std::uint64_t p99_microseconds{};
  std::uint64_t p999_microseconds{};
  std::uint64_t max_microseconds{};
};

struct MetricsSnapshot {
  std::uint64_t commands{};
  std::uint64_t trades{};
  std::uint64_t duplicate_commands{};
  std::uint64_t sequence_gaps{};
  std::uint64_t stale_epochs{};
  std::uint64_t identity_conflicts{};
  std::uint64_t active_orders{};
  std::uint64_t active_price_levels{};
  std::uint64_t active_instruments{};
  std::uint64_t queue_depth{};
  std::uint64_t wal_size_bytes{};
  std::uint64_t wal_group_commits{};
  std::uint64_t wal_group_commands{};
  std::uint64_t replayed_records{};
  std::uint64_t event_publish_lag_events{};
  std::uint64_t event_publish_lag_bytes{};
  std::uint64_t event_publish_lag_age_ns{};
  HistogramSnapshot queue_latency;
  HistogramSnapshot end_to_end_latency;
  HistogramSnapshot wal_commit_latency;
  HistogramSnapshot execution_latency;
  HistogramSnapshot publish_latency;
};

class MetricsSink {
 public:
  virtual ~MetricsSink() = default;
  virtual void observe(std::string_view name, std::uint64_t value) = 0;
};

class NullMetricsSink final : public MetricsSink {
 public:
  void observe(std::string_view, std::uint64_t) override {}
};

}  // namespace order_books
