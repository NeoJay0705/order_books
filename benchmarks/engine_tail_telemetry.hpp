#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "order_books/engine.hpp"

namespace order_books::benchmark {

enum class TailTelemetryPhase : std::uint8_t {
  disabled,
  measured,
  drain,
  stopped,
};

enum class TailTelemetryRecordType : std::uint8_t {
  sync,
  group_commands,
  state,
};

enum class TailTelemetryBoundary : std::uint8_t {
  none,
  drain_start,
  drain_end,
};

[[nodiscard]] std::chrono::steady_clock::time_point next_tail_sample_deadline(
    std::chrono::steady_clock::time_point previous_deadline,
    std::chrono::steady_clock::time_point observed_at) noexcept;

struct TailTelemetrySummary {
  std::uint64_t measured_sync_count{};
  std::optional<std::uint64_t> measured_sync_p50_us;
  std::optional<std::uint64_t> measured_sync_p99_us;
  std::optional<std::uint64_t> measured_sync_p999_us;
  std::optional<std::uint64_t> measured_sync_max_us;
  std::uint64_t measured_sync_total_us{};
  std::uint64_t measured_sync_over_25ms{};
  std::uint64_t measured_sync_over_100ms{};
  std::uint64_t measured_sync_over_250ms{};
  std::uint64_t measured_group_sample_count{};
  std::uint64_t measured_group_sample_commands{};
  std::optional<std::uint64_t> measured_queue_depth_max;
  std::optional<std::uint64_t> measured_publisher_lag_events_max;
  std::optional<std::uint64_t> measured_publisher_lag_bytes_max;
  std::optional<std::uint64_t> measured_publisher_lag_age_ns_max;
  std::uint64_t drain_state_sample_count{};
  std::optional<std::uint64_t> drain_publisher_lag_events_first;
  std::optional<std::uint64_t> drain_publisher_lag_bytes_first;
  std::optional<std::uint64_t> drain_publisher_lag_age_ns_first;
  std::optional<std::uint64_t> drain_publisher_lag_events_last;
  std::optional<std::uint64_t> drain_publisher_lag_bytes_last;
  std::optional<std::uint64_t> drain_publisher_lag_age_ns_last;
  std::uint64_t telemetry_dropped_samples{};
  bool sampler_error{};
  bool aggregate_overflow{};
};

class EngineTailTelemetry final : public order_books::MetricsSink {
 public:
  static constexpr std::size_t kMaxMetricSamples = 1'000'000U;
  static constexpr std::size_t kMaxStateSamples = 100'000U;
  static constexpr auto kStateSampleInterval = std::chrono::milliseconds(10);

  explicit EngineTailTelemetry(std::size_t expected_groups);
  ~EngineTailTelemetry() override;

  EngineTailTelemetry(const EngineTailTelemetry&) = delete;
  EngineTailTelemetry& operator=(const EngineTailTelemetry&) = delete;

  [[nodiscard]] bool reserve() noexcept;
  void observe(std::string_view name, std::uint64_t value) override;

  [[nodiscard]] bool begin_measured() noexcept;
  [[nodiscard]] bool begin_drain() noexcept;
  [[nodiscard]] bool record_drain_snapshot(
      const order_books::MetricsSnapshot& metrics,
      TailTelemetryBoundary boundary) noexcept;
  void stop_collection() noexcept;

  [[nodiscard]] bool start_sampler(order_books::Engine& engine,
                                   order_books::ShardId shard_id) noexcept;
  void stop_sampler() noexcept;

  [[nodiscard]] TailTelemetrySummary summary() const;
  [[nodiscard]] bool write_csv(const std::filesystem::path& path) const;

 private:
  struct Record {
    TailTelemetryRecordType type{TailTelemetryRecordType::state};
    TailTelemetryPhase phase{TailTelemetryPhase::disabled};
    std::uint64_t elapsed_us{};
    std::uint64_t value{};
    std::uint64_t queue_depth{};
    std::uint64_t publisher_lag_events{};
    std::uint64_t publisher_lag_bytes{};
    std::uint64_t publisher_lag_age_ns{};
    std::uint64_t order{};
    TailTelemetryBoundary boundary{TailTelemetryBoundary::none};
  };

  void sampler_loop(std::stop_token stop_token, order_books::Engine* engine,
                    order_books::ShardId shard_id) noexcept;
  [[nodiscard]] bool record_state(
      TailTelemetryPhase phase, const order_books::MetricsSnapshot& metrics,
      std::chrono::steady_clock::time_point observed_at,
      TailTelemetryBoundary boundary = TailTelemetryBoundary::none) noexcept;
  void record_sampler_error() noexcept;
  [[nodiscard]] std::uint64_t elapsed_us(
      std::chrono::steady_clock::time_point observed_at) const noexcept;
  [[nodiscard]] static std::string_view phase_name(TailTelemetryPhase phase) noexcept;

  std::atomic<TailTelemetryPhase> phase_{TailTelemetryPhase::disabled};
  std::chrono::steady_clock::time_point measured_epoch_{};
  mutable std::mutex mutex_;
  std::vector<Record> records_;
  bool setup_failed_{};
  std::size_t sync_sample_count_{};
  std::size_t group_command_sample_count_{};
  std::size_t state_sample_count_{};
  bool drain_start_recorded_{};
  bool drain_end_recorded_{};
  std::uint64_t next_order_{};
  std::uint64_t dropped_samples_{};
  bool aggregate_overflow_{};
  std::atomic<bool> sampler_error_{false};
  std::jthread sampler_;
  std::mutex sampler_wait_mutex_;
  std::condition_variable_any sampler_wait_condition_;
};

}  // namespace order_books::benchmark
