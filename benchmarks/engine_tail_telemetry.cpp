#include "engine_tail_telemetry.hpp"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <inttypes.h>
#include <limits>
#include <unistd.h>
#include <utility>

namespace order_books::benchmark {
namespace {

using order_books::MetricsSnapshot;

constexpr std::string_view kSyncMetric = "wal_sync_latency_us";
constexpr std::string_view kGroupCommandsMetric = "wal_group_commands";

bool checked_add(std::uint64_t& target, const std::uint64_t value) noexcept {
  if (target > std::numeric_limits<std::uint64_t>::max() - value) {
    return false;
  }
  target += value;
  return true;
}

std::optional<std::uint64_t> nearest_rank(const std::vector<std::uint64_t>& sorted,
                                          const std::size_t numerator,
                                          const std::size_t denominator) {
  if (sorted.empty()) {
    return std::nullopt;
  }
  const auto rank = (sorted.size() * numerator + denominator - 1U) / denominator;
  const auto index = std::min(sorted.size() - 1U, rank == 0U ? 0U : rank - 1U);
  return sorted[index];
}

}  // namespace

std::chrono::steady_clock::time_point next_tail_sample_deadline(
    const std::chrono::steady_clock::time_point previous_deadline,
    const std::chrono::steady_clock::time_point observed_at) noexcept {
  auto next_deadline = previous_deadline + EngineTailTelemetry::kStateSampleInterval;
  if (next_deadline <= observed_at) {
    next_deadline = observed_at + EngineTailTelemetry::kStateSampleInterval;
  }
  return next_deadline;
}

EngineTailTelemetry::EngineTailTelemetry(const std::size_t expected_groups) {
  try {
    const auto metric_count = std::min(expected_groups, kMaxMetricSamples);
    if (metric_count >
        (std::numeric_limits<std::size_t>::max() - kMaxStateSamples) / 2U) {
      setup_failed_ = true;
      return;
    }
    records_.reserve((metric_count * 2U) + kMaxStateSamples);
  } catch (...) {
    setup_failed_ = true;
  }
}

EngineTailTelemetry::~EngineTailTelemetry() {
  stop_collection();
  stop_sampler();
}

bool EngineTailTelemetry::reserve() noexcept {
  // The constructor performs the bounded reserve.  Keeping this explicit
  // setup gate lets the benchmark report allocation failure before opening
  // the Engine, rather than from the writer hot path.
  try {
    if (setup_failed_) {
      return false;
    }
    if (records_.capacity() == 0U) {
      return false;
    }
  } catch (...) {
    return false;
  }
  return true;
}

void EngineTailTelemetry::observe(const std::string_view name,
                                  const std::uint64_t value) {
  if (name != kSyncMetric && name != kGroupCommandsMetric) {
    return;
  }
  const auto current_phase = phase_.load(std::memory_order_acquire);
  if (current_phase != TailTelemetryPhase::measured &&
      current_phase != TailTelemetryPhase::drain) {
    return;
  }
  try {
    const auto observed_at = std::chrono::steady_clock::now();
    const auto type = name == kSyncMetric ? TailTelemetryRecordType::sync
                                          : TailTelemetryRecordType::group_commands;
    std::lock_guard lock(mutex_);
    if (phase_.load(std::memory_order_acquire) != current_phase) {
      return;
    }
    if (next_order_ == std::numeric_limits<std::uint64_t>::max()) {
      aggregate_overflow_ = true;
      return;
    }
    auto& sample_count = type == TailTelemetryRecordType::sync
                             ? sync_sample_count_
                             : group_command_sample_count_;
    if (sample_count >= kMaxMetricSamples) {
      if (dropped_samples_ != std::numeric_limits<std::uint64_t>::max()) {
        ++dropped_samples_;
      }
      return;
    }
    records_.push_back(Record{type, current_phase, elapsed_us(observed_at), value, 0U,
                              0U, 0U, 0U, next_order_++});
    ++sample_count;
  } catch (...) {
    try {
      std::lock_guard lock(mutex_);
      if (dropped_samples_ != std::numeric_limits<std::uint64_t>::max()) {
        ++dropped_samples_;
      }
    } catch (...) {
    }
  }
}

bool EngineTailTelemetry::begin_measured() noexcept {
  if (phase_.load(std::memory_order_acquire) != TailTelemetryPhase::disabled) {
    return false;
  }
  measured_epoch_ = std::chrono::steady_clock::now();
  phase_.store(TailTelemetryPhase::measured, std::memory_order_release);
  return true;
}

bool EngineTailTelemetry::begin_drain() noexcept {
  auto expected = TailTelemetryPhase::measured;
  return phase_.compare_exchange_strong(expected, TailTelemetryPhase::drain,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

bool EngineTailTelemetry::record_drain_snapshot(
    const order_books::MetricsSnapshot& metrics,
    const TailTelemetryBoundary boundary) noexcept {
  if (boundary != TailTelemetryBoundary::drain_start &&
      boundary != TailTelemetryBoundary::drain_end) {
    return false;
  }
  return record_state(TailTelemetryPhase::drain, metrics,
                      std::chrono::steady_clock::now(), boundary);
}

void EngineTailTelemetry::stop_collection() noexcept {
  phase_.store(TailTelemetryPhase::stopped, std::memory_order_release);
}

bool EngineTailTelemetry::start_sampler(order_books::Engine& engine,
                                        const order_books::ShardId shard_id) noexcept {
  if (sampler_.joinable()) {
    return false;
  }
  try {
    sampler_ = std::jthread([this, &engine, shard_id](std::stop_token stop_token) {
      sampler_loop(stop_token, &engine, shard_id);
    });
  } catch (...) {
    record_sampler_error();
    return false;
  }
  return true;
}

void EngineTailTelemetry::stop_sampler() noexcept {
  if (!sampler_.joinable()) {
    return;
  }
  sampler_.request_stop();
  sampler_wait_condition_.notify_all();
  sampler_.join();
}

void EngineTailTelemetry::sampler_loop(
    const std::stop_token stop_token, order_books::Engine* const engine,
    const order_books::ShardId shard_id) noexcept {
  try {
    auto next_sample = std::chrono::steady_clock::now() + kStateSampleInterval;
    while (!stop_token.stop_requested()) {
      {
        std::unique_lock lock(sampler_wait_mutex_);
        sampler_wait_condition_.wait_until(lock, stop_token, next_sample, [] {
          return false;
        });
      }
      if (stop_token.stop_requested()) {
        return;
      }
      const auto before_call_phase = phase_.load(std::memory_order_acquire);
      if (before_call_phase == TailTelemetryPhase::measured ||
          before_call_phase == TailTelemetryPhase::drain) {
        const auto metrics = engine->metrics(shard_id);
        if (std::holds_alternative<order_books::Error>(metrics)) {
          record_sampler_error();
          return;
        }
        const auto observed_phase = phase_.load(std::memory_order_acquire);
        if (observed_phase == TailTelemetryPhase::measured ||
            observed_phase == TailTelemetryPhase::drain) {
          (void)record_state(observed_phase, std::get<MetricsSnapshot>(metrics),
                             std::chrono::steady_clock::now());
        }
      }
      next_sample = next_tail_sample_deadline(next_sample,
                                              std::chrono::steady_clock::now());
    }
  } catch (...) {
    record_sampler_error();
  }
}

bool EngineTailTelemetry::record_state(
    const TailTelemetryPhase phase, const MetricsSnapshot& metrics,
    const std::chrono::steady_clock::time_point observed_at,
    const TailTelemetryBoundary boundary) noexcept {
  try {
    std::lock_guard lock(mutex_);
    if (phase_.load(std::memory_order_acquire) != phase) {
      return false;
    }
    if (phase == TailTelemetryPhase::drain) {
      if (boundary == TailTelemetryBoundary::none && !drain_start_recorded_) {
        // Do not let a periodic sample win the race with the explicit drain-start
        // snapshot.  The CSV intentionally has no boundary column, so this keeps
        // its first drain row deterministic without counting a sample as dropped.
        return true;
      }
      if (boundary == TailTelemetryBoundary::drain_start &&
          (drain_start_recorded_ || drain_end_recorded_)) {
        return false;
      }
      if (boundary == TailTelemetryBoundary::drain_end &&
          (!drain_start_recorded_ || drain_end_recorded_)) {
        return false;
      }
      if (drain_end_recorded_) {
        return false;
      }
    }
    if (state_sample_count_ >= kMaxStateSamples) {
      if (dropped_samples_ != std::numeric_limits<std::uint64_t>::max()) {
        ++dropped_samples_;
      }
      return false;
    }
    if (next_order_ == std::numeric_limits<std::uint64_t>::max()) {
      aggregate_overflow_ = true;
      return false;
    }
    records_.push_back(Record{
        TailTelemetryRecordType::state,
        phase,
        elapsed_us(observed_at),
        0U,
        metrics.queue_depth,
        metrics.event_publish_lag_events,
        metrics.event_publish_lag_bytes,
        metrics.event_publish_lag_age_ns,
        next_order_++,
        boundary,
    });
    ++state_sample_count_;
    if (boundary == TailTelemetryBoundary::drain_start) {
      drain_start_recorded_ = true;
    } else if (boundary == TailTelemetryBoundary::drain_end) {
      drain_end_recorded_ = true;
    }
    return true;
  } catch (...) {
    try {
      std::lock_guard lock(mutex_);
      if (dropped_samples_ != std::numeric_limits<std::uint64_t>::max()) {
        ++dropped_samples_;
      }
    } catch (...) {
    }
    return false;
  }
}

void EngineTailTelemetry::record_sampler_error() noexcept {
  sampler_error_.store(true, std::memory_order_release);
}

std::uint64_t EngineTailTelemetry::elapsed_us(
    const std::chrono::steady_clock::time_point observed_at) const noexcept {
  if (observed_at <= measured_epoch_) {
    return 0U;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      observed_at - measured_epoch_);
  return static_cast<std::uint64_t>(elapsed.count());
}

std::string_view EngineTailTelemetry::phase_name(
    const TailTelemetryPhase phase) noexcept {
  switch (phase) {
    case TailTelemetryPhase::disabled:
      return "disabled";
    case TailTelemetryPhase::measured:
      return "measured";
    case TailTelemetryPhase::drain:
      return "drain";
    case TailTelemetryPhase::stopped:
      return "stopped";
  }
  return "unknown";
}

TailTelemetrySummary EngineTailTelemetry::summary() const {
  std::vector<Record> records;
  std::uint64_t dropped_samples = 0U;
  bool aggregate_overflow = false;
  {
    std::lock_guard lock(mutex_);
    records = records_;
    dropped_samples = dropped_samples_;
    aggregate_overflow = aggregate_overflow_;
  }

  TailTelemetrySummary result;
  std::vector<std::uint64_t> sync_values;
  sync_values.reserve(records.size());
  std::optional<std::uint64_t> measured_queue_depth_max;
  std::optional<std::uint64_t> measured_lag_events_max;
  std::optional<std::uint64_t> measured_lag_bytes_max;
  std::optional<std::uint64_t> measured_lag_age_max;
  for (const auto& record : records) {
    if (record.type == TailTelemetryRecordType::sync &&
        record.phase == TailTelemetryPhase::measured) {
      sync_values.push_back(record.value);
      if (!checked_add(result.measured_sync_total_us, record.value)) {
        result.aggregate_overflow = true;
      }
      if (record.value > 25'000U) {
        ++result.measured_sync_over_25ms;
      }
      if (record.value > 100'000U) {
        ++result.measured_sync_over_100ms;
      }
      if (record.value > 250'000U) {
        ++result.measured_sync_over_250ms;
      }
    } else if (record.type == TailTelemetryRecordType::group_commands &&
               record.phase == TailTelemetryPhase::measured) {
      ++result.measured_group_sample_count;
      if (!checked_add(result.measured_group_sample_commands, record.value)) {
        result.aggregate_overflow = true;
      }
    } else if (record.type == TailTelemetryRecordType::state &&
               record.phase == TailTelemetryPhase::measured) {
      measured_queue_depth_max = measured_queue_depth_max.has_value()
                                     ? std::max(*measured_queue_depth_max, record.queue_depth)
                                     : std::optional<std::uint64_t>(record.queue_depth);
      measured_lag_events_max = measured_lag_events_max.has_value()
                                    ? std::max(*measured_lag_events_max,
                                               record.publisher_lag_events)
                                    : std::optional<std::uint64_t>(record.publisher_lag_events);
      measured_lag_bytes_max = measured_lag_bytes_max.has_value()
                                   ? std::max(*measured_lag_bytes_max, record.publisher_lag_bytes)
                                   : std::optional<std::uint64_t>(record.publisher_lag_bytes);
      measured_lag_age_max = measured_lag_age_max.has_value()
                                 ? std::max(*measured_lag_age_max,
                                            record.publisher_lag_age_ns)
                                 : std::optional<std::uint64_t>(record.publisher_lag_age_ns);
    } else if (record.type == TailTelemetryRecordType::state &&
               record.phase == TailTelemetryPhase::drain) {
      ++result.drain_state_sample_count;
      if (record.boundary == TailTelemetryBoundary::drain_start) {
        result.drain_publisher_lag_events_first = record.publisher_lag_events;
        result.drain_publisher_lag_bytes_first = record.publisher_lag_bytes;
        result.drain_publisher_lag_age_ns_first = record.publisher_lag_age_ns;
      } else if (record.boundary == TailTelemetryBoundary::drain_end) {
        result.drain_publisher_lag_events_last = record.publisher_lag_events;
        result.drain_publisher_lag_bytes_last = record.publisher_lag_bytes;
        result.drain_publisher_lag_age_ns_last = record.publisher_lag_age_ns;
      }
    }
  }
  std::sort(sync_values.begin(), sync_values.end());
  result.measured_sync_count = sync_values.size();
  result.measured_sync_p50_us = nearest_rank(sync_values, 1U, 2U);
  result.measured_sync_p99_us = nearest_rank(sync_values, 99U, 100U);
  result.measured_sync_p999_us = nearest_rank(sync_values, 999U, 1000U);
  if (!sync_values.empty()) {
    result.measured_sync_max_us = sync_values.back();
  }

  result.measured_queue_depth_max = measured_queue_depth_max;
  result.measured_publisher_lag_events_max = measured_lag_events_max;
  result.measured_publisher_lag_bytes_max = measured_lag_bytes_max;
  result.measured_publisher_lag_age_ns_max = measured_lag_age_max;
  result.telemetry_dropped_samples = dropped_samples;
  result.sampler_error = sampler_error_.load(std::memory_order_acquire);
  result.aggregate_overflow = result.aggregate_overflow || aggregate_overflow;
  return result;
}

bool EngineTailTelemetry::write_csv(const std::filesystem::path& path) const {
  if (phase_.load(std::memory_order_acquire) != TailTelemetryPhase::stopped ||
      sampler_.joinable()) {
    return false;
  }
  std::vector<Record> rows;
  {
    std::lock_guard lock(mutex_);
    rows = records_;
  }
  std::stable_sort(rows.begin(), rows.end(), [](const Record& lhs, const Record& rhs) {
    if (lhs.elapsed_us != rhs.elapsed_us) {
      return lhs.elapsed_us < rhs.elapsed_us;
    }
    return lhs.order < rhs.order;
  });

  const auto file_descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                      0644);
  if (file_descriptor < 0) {
    return false;
  }

  auto* output = ::fdopen(file_descriptor, "w");
  if (output == nullptr) {
    (void)::close(file_descriptor);
    return false;
  }

  bool success = std::fputs(
                     "record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,"
                     "publisher_lag_bytes,publisher_lag_age_ns\n",
                     output) >= 0;
  for (const auto& row : rows) {
    const auto type_name = row.type == TailTelemetryRecordType::sync
                               ? "sync"
                               : row.type == TailTelemetryRecordType::group_commands
                                     ? "group_commands"
                                     : "state";
    const auto phase = phase_name(row.phase);
    const auto phase_length = static_cast<int>(phase.size());
    if (row.type == TailTelemetryRecordType::state) {
      if (std::fprintf(output,
                       "%s,%.*s,%" PRIu64 ",,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                       ",%" PRIu64 "\n",
                       type_name, phase_length, phase.data(), row.elapsed_us,
                       row.queue_depth, row.publisher_lag_events, row.publisher_lag_bytes,
                       row.publisher_lag_age_ns) < 0) {
        success = false;
      }
    } else if (std::fprintf(output, "%s,%.*s,%" PRIu64 ",%" PRIu64 ",,,,\n", type_name,
                           phase_length, phase.data(), row.elapsed_us, row.value) < 0) {
      success = false;
    }
  }
  if (std::fflush(output) != 0) {
    success = false;
  }
  if (std::fclose(output) != 0) {
    success = false;
  }
  return success;
}

}  // namespace order_books::benchmark
