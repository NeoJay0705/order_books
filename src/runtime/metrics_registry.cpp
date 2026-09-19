#include "runtime/metrics_registry.hpp"

#include <algorithm>
#include <limits>

namespace order_books::runtime {

namespace {

constexpr std::array<std::uint64_t, 22U> kLatencyUpperBounds{
    1U,       2U,       5U,       10U,      25U,      50U,      100U,     250U,
    500U,     1'000U,   2'500U,   5'000U,   10'000U,  25'000U,  50'000U,  100'000U,
    250'000U, 500'000U, 1'000'000U, 5'000'000U, 10'000'000U, std::numeric_limits<std::uint64_t>::max()};

constexpr std::array<std::string_view, 35U> kMetricNames{
    "active_instruments",
    "active_orders",
    "active_price_levels",
    "commands",
    "completion_callback_error",
    "duplicate_commands",
    "end_to_end_latency_us",
    "event_publish_lag_age_ns",
    "event_publish_lag_bytes",
    "event_publish_lag_events",
    "event_publish_retry",
    "identity_conflicts",
    "publisher_cursor_error",
    "publisher_lag_critical",
    "publisher_lag_warning",
    "publisher_replay_error",
    "publisher_snapshot_error",
    "publisher_state_error",
    "publish_latency_us",
    "queue_depth",
    "queue_latency_us",
    "replayed_records",
    "wal_parallel_prepare_groups",
    "wal_parallel_prepare_min_commands",
    "wal_prepare_lanes",
    "wal_prepare_tasks",
    "sequence_gaps",
    "stale_epochs",
    "trades",
    "wal_commit_latency_us",
    "wal_group_commits",
    "wal_group_commands",
    "wal_size_bytes",
    "wal_sync_latency_us",
    "execution_latency_us",
};

bool is_known_metric(const std::string_view name) {
  return std::find(kMetricNames.begin(), kMetricNames.end(), name) != kMetricNames.end();
}

bool is_histogram(const std::string_view name) {
  return name == "queue_latency_us" || name == "end_to_end_latency_us" ||
         name == "wal_commit_latency_us" ||
         name == "wal_sync_latency_us" ||
         name == "execution_latency_us" || name == "publish_latency_us";
}

}  // namespace

void MetricsRegistry::Histogram::observe(const std::uint64_t value) noexcept {
  const auto iterator = std::lower_bound(kLatencyUpperBounds.begin(), kLatencyUpperBounds.end(),
                                         value);
  const auto index = static_cast<std::size_t>(iterator - kLatencyUpperBounds.begin());
  if (buckets[index] != std::numeric_limits<std::uint64_t>::max()) {
    ++buckets[index];
  }
  if (count != std::numeric_limits<std::uint64_t>::max()) {
    ++count;
  }
  max = std::max(max, value);
}

HistogramSnapshot MetricsRegistry::Histogram::snapshot() const noexcept {
  HistogramSnapshot result;
  result.count = count;
  result.max_microseconds = max;
  if (count == 0) {
    return result;
  }
  const auto quantile = [this](const std::uint64_t numerator,
                               const std::uint64_t denominator) {
    const auto quotient = count / denominator;
    const auto remainder = count % denominator;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto scaled_quotient = quotient > maximum / numerator
                                     ? maximum
                                     : quotient * numerator;
    const auto scaled_remainder = remainder > (maximum - (denominator - 1U)) / numerator
                                       ? maximum
                                       : (remainder * numerator + denominator - 1U) /
                                             denominator;
    const auto target = scaled_quotient > maximum - scaled_remainder
                            ? maximum
                            : scaled_quotient + scaled_remainder;
    std::uint64_t accumulated = 0;
    for (std::size_t index = 0; index < buckets.size(); ++index) {
      accumulated = accumulated > maximum - buckets[index]
                        ? maximum
                        : accumulated + buckets[index];
      if (accumulated >= target) {
        return kLatencyUpperBounds[index];
      }
    }
    return max;
  };
  result.p50_microseconds = quantile(1U, 2U);
  result.p99_microseconds = quantile(99U, 100U);
  result.p999_microseconds = quantile(999U, 1000U);
  return result;
}

void MetricsRegistry::observe(const std::string_view name, const std::uint64_t value) {
  if (!is_known_metric(name)) {
    return;
  }
  try {
    {
      std::lock_guard lock(mutex_);
      if (is_histogram(name)) {
        histograms_[std::string(name)].observe(value);
      } else {
        auto& counter = counters_[std::string(name)];
        if (name == "active_orders" || name == "active_price_levels" ||
            name == "active_instruments" || name == "queue_depth" ||
            name == "wal_size_bytes" || name == "event_publish_lag_events" ||
            name == "event_publish_lag_bytes" || name == "event_publish_lag_age_ns" ||
            name == "wal_prepare_lanes" ||
            name == "wal_parallel_prepare_min_commands") {
          counter = value;
        } else {
          counter = counter > std::numeric_limits<std::uint64_t>::max() - value
                        ? std::numeric_limits<std::uint64_t>::max()
                        : counter + value;
        }
      }
    }
  } catch (...) {
    return;
  }
  // Monitoring failures are deliberately outside the command path.  The
  // configured sink is expected to be non-throwing; a defensive boundary keeps
  // an exporter implementation from failing a matching shard.
  try {
    downstream_.observe(name, value);
  } catch (...) {
  }
}

MetricsSnapshot MetricsRegistry::snapshot() const {
  std::lock_guard lock(mutex_);
  const auto read_counter = [this](const std::string_view name) {
    const auto iterator = counters_.find(std::string(name));
    return iterator == counters_.end() ? std::uint64_t{0} : iterator->second;
  };
  const auto read_histogram = [this](const std::string_view name) {
    const auto iterator = histograms_.find(std::string(name));
    return iterator == histograms_.end() ? HistogramSnapshot{} : iterator->second.snapshot();
  };
  MetricsSnapshot result;
  result.commands = read_counter("commands");
  result.trades = read_counter("trades");
  result.duplicate_commands = read_counter("duplicate_commands");
  result.sequence_gaps = read_counter("sequence_gaps");
  result.stale_epochs = read_counter("stale_epochs");
  result.identity_conflicts = read_counter("identity_conflicts");
  result.active_orders = read_counter("active_orders");
  result.active_price_levels = read_counter("active_price_levels");
  result.active_instruments = read_counter("active_instruments");
  result.queue_depth = read_counter("queue_depth");
  result.wal_size_bytes = read_counter("wal_size_bytes");
  result.wal_prepare_lanes = read_counter("wal_prepare_lanes");
  result.wal_parallel_prepare_min_commands =
      read_counter("wal_parallel_prepare_min_commands");
  result.wal_parallel_prepare_groups = read_counter("wal_parallel_prepare_groups");
  result.wal_prepare_tasks = read_counter("wal_prepare_tasks");
  result.wal_group_commits = read_counter("wal_group_commits");
  result.wal_group_commands = read_counter("wal_group_commands");
  result.replayed_records = read_counter("replayed_records");
  result.event_publish_lag_events = read_counter("event_publish_lag_events");
  result.event_publish_lag_bytes = read_counter("event_publish_lag_bytes");
  result.event_publish_lag_age_ns = read_counter("event_publish_lag_age_ns");
  result.queue_latency = read_histogram("queue_latency_us");
  result.end_to_end_latency = read_histogram("end_to_end_latency_us");
  result.wal_commit_latency = read_histogram("wal_commit_latency_us");
  result.wal_sync_latency = read_histogram("wal_sync_latency_us");
  result.execution_latency = read_histogram("execution_latency_us");
  result.publish_latency = read_histogram("publish_latency_us");
  return result;
}

}  // namespace order_books::runtime
