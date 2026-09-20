#include "engine_tail_telemetry.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

namespace order_books::benchmark {
namespace {

std::uint64_t realtime_epoch_now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}

void expect_anchor_within_bracket(
    const std::optional<std::uint64_t>& realtime_epoch,
    const std::optional<std::uint64_t>& uncertainty,
    const std::uint64_t before, const std::uint64_t after) {
  if (!realtime_epoch.has_value() || !uncertainty.has_value()) {
    ADD_FAILURE() << "clock anchor fields are missing";
    return;
  }
  EXPECT_GE(*realtime_epoch, before);
  EXPECT_LE(*realtime_epoch, after);
  EXPECT_LE(*uncertainty, after - before);
}

TEST(EngineTailTelemetryTest, KeepsTheDesignedStateSamplingInterval) {
  EXPECT_EQ(EngineTailTelemetry::kStateSampleInterval,
            std::chrono::milliseconds(10));
}

TEST(EngineTailTelemetryTest, AdvancesDeadlineWithoutCatchingUpMissedTicks) {
  using Clock = std::chrono::steady_clock;
  const auto previous = Clock::time_point{} + std::chrono::milliseconds(100);

  EXPECT_EQ(next_tail_sample_deadline(previous,
                                      Clock::time_point{} + std::chrono::milliseconds(105)),
            Clock::time_point{} + std::chrono::milliseconds(110));
  const auto late = next_tail_sample_deadline(
      previous, Clock::time_point{} + std::chrono::milliseconds(125));
  EXPECT_EQ(late, Clock::time_point{} + std::chrono::milliseconds(135));
  EXPECT_GT(late, Clock::time_point{} + std::chrono::milliseconds(125));
}

TEST(EngineTailTelemetryTest, ChecksClockAnchorsAndMapsElapsedSamples) {
  const TailClockAnchor start{1'000'000'000U, 0U, 100U};
  const TailClockAnchor end{1'020'000'100U, 20'000'000U, 100U};
  EXPECT_TRUE(tail_clock_anchors_consistent(start, end));
  EXPECT_TRUE(tail_clock_anchors_consistent(
      TailClockAnchor{1'000'000'000U, 0U, 0U},
      TailClockAnchor{1'020'000'000U, 20'000'000U, 0U}));
  const auto interval =
      map_tail_sample_to_epoch_interval_ns(start, 2'500U, 500U);
  ASSERT_TRUE(interval.has_value());
  if (!interval.has_value()) {
    return;
  }
  EXPECT_EQ(interval->start_epoch_ns, 1'002'000'000U);
  EXPECT_EQ(interval->end_epoch_ns, 1'002'500'000U);

  const auto crossing =
      map_tail_sample_to_epoch_interval_ns(start, 500U, 1'000U);
  ASSERT_TRUE(crossing.has_value());
  EXPECT_EQ(crossing->start_epoch_ns, 999'500'000U);
  EXPECT_EQ(crossing->end_epoch_ns, 1'000'500'000U);

  const TailClockAnchor stepped{1'020'002'000U, 20'000'000U, 100U};
  EXPECT_FALSE(tail_clock_anchors_consistent(start, stepped));
  EXPECT_FALSE(map_tail_sample_to_epoch_interval_ns(
      TailClockAnchor{0U, 0U, 0U}, 1U, 2U));
  EXPECT_FALSE(map_tail_sample_to_epoch_interval_ns(
      start, (std::numeric_limits<std::uint64_t>::max() / 1'000U) + 1U, 0U));
  EXPECT_FALSE(map_tail_sample_to_epoch_interval_ns(
      TailClockAnchor{std::numeric_limits<std::uint64_t>::max() - 999U, 0U, 0U},
      2U, 0U));
}

TEST(EngineTailTelemetryTest, AllowsEachClockPhaseTransitionOnlyOnce) {
  EngineTailTelemetry telemetry(1);
  ASSERT_TRUE(telemetry.reserve());
  ASSERT_TRUE(telemetry.begin_measured());
  const auto measured = telemetry.summary();
  ASSERT_TRUE(measured.tail_clock_start_realtime_epoch_ns.has_value());
  ASSERT_TRUE(measured.tail_clock_start_uncertainty_ns.has_value());
  EXPECT_FALSE(telemetry.begin_measured());
  const auto duplicate_measured = telemetry.summary();
  EXPECT_EQ(duplicate_measured.tail_clock_start_realtime_epoch_ns,
            measured.tail_clock_start_realtime_epoch_ns);
  EXPECT_EQ(duplicate_measured.tail_clock_start_uncertainty_ns,
            measured.tail_clock_start_uncertainty_ns);
  ASSERT_TRUE(telemetry.begin_drain());
  const auto drained = telemetry.summary();
  ASSERT_TRUE(drained.tail_clock_end_realtime_epoch_ns.has_value());
  EXPECT_FALSE(telemetry.begin_drain());
  const auto duplicate_drain = telemetry.summary();
  EXPECT_EQ(duplicate_drain.tail_clock_end_realtime_epoch_ns,
            drained.tail_clock_end_realtime_epoch_ns);
  EXPECT_EQ(duplicate_drain.tail_clock_end_steady_elapsed_ns,
            drained.tail_clock_end_steady_elapsed_ns);
  EXPECT_EQ(duplicate_drain.tail_clock_end_uncertainty_ns,
            drained.tail_clock_end_uncertainty_ns);
}

TEST(EngineTailTelemetryTest, RejectsDrainBeforeMeasuredWithoutAnchors) {
  EngineTailTelemetry telemetry(1);
  ASSERT_TRUE(telemetry.reserve());
  EXPECT_FALSE(telemetry.begin_drain());

  const auto result = telemetry.summary();
  EXPECT_FALSE(result.tail_clock_start_realtime_epoch_ns.has_value());
  EXPECT_FALSE(result.tail_clock_start_uncertainty_ns.has_value());
  EXPECT_FALSE(result.tail_clock_end_realtime_epoch_ns.has_value());
  EXPECT_FALSE(result.tail_clock_end_steady_elapsed_ns.has_value());
  EXPECT_FALSE(result.tail_clock_end_uncertainty_ns.has_value());
}

TEST(EngineTailTelemetryTest, CapturesClockAnchorsWithinExternalBrackets) {
  EngineTailTelemetry telemetry(1);
  ASSERT_TRUE(telemetry.reserve());
  const auto start_before = realtime_epoch_now_ns();
  ASSERT_TRUE(telemetry.begin_measured());
  const auto start_after = realtime_epoch_now_ns();
  ASSERT_LE(start_before, start_after);

  const auto measured = telemetry.summary();
  expect_anchor_within_bracket(measured.tail_clock_start_realtime_epoch_ns,
                               measured.tail_clock_start_uncertainty_ns,
                               start_before, start_after);

  const auto end_before = realtime_epoch_now_ns();
  ASSERT_TRUE(telemetry.begin_drain());
  const auto end_after = realtime_epoch_now_ns();
  ASSERT_LE(end_before, end_after);

  const auto drained = telemetry.summary();
  expect_anchor_within_bracket(drained.tail_clock_end_realtime_epoch_ns,
                               drained.tail_clock_end_uncertainty_ns, end_before,
                               end_after);
}

std::filesystem::path unique_artifact_path() {
  return std::filesystem::temp_directory_path() /
         ("order_books_engine_tail_telemetry_test-" +
          std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".csv");
}

TEST(EngineTailTelemetryTest, CollectsMeasuredSamplesAndSeparatesDrain) {
  EngineTailTelemetry telemetry(4);
  ASSERT_TRUE(telemetry.reserve());
  ASSERT_TRUE(telemetry.begin_measured());
  telemetry.observe("wal_sync_latency_us", 100);
  telemetry.observe("wal_sync_latency_us", 200);
  telemetry.observe("wal_group_commands", 2);
  telemetry.observe("wal_group_commands", 2);
  telemetry.observe("unrelated_metric", 999);
  ASSERT_TRUE(telemetry.begin_drain());
  telemetry.observe("wal_sync_latency_us", 900);
  telemetry.observe("wal_group_commands", 4);
  telemetry.stop_collection();

  const auto result = telemetry.summary();
  EXPECT_EQ(result.measured_sync_count, 2U);
  EXPECT_EQ(result.measured_sync_p50_us, 100U);
  EXPECT_EQ(result.measured_sync_p99_us, 200U);
  EXPECT_EQ(result.measured_sync_p999_us, 200U);
  EXPECT_EQ(result.measured_sync_max_us, 200U);
  EXPECT_EQ(result.measured_group_sample_count, 2U);
  EXPECT_EQ(result.measured_group_sample_commands, 4U);
  ASSERT_TRUE(result.tail_clock_start_realtime_epoch_ns.has_value());
  ASSERT_TRUE(result.tail_clock_start_uncertainty_ns.has_value());
  ASSERT_TRUE(result.tail_clock_end_realtime_epoch_ns.has_value());
  ASSERT_TRUE(result.tail_clock_end_steady_elapsed_ns.has_value());
  ASSERT_TRUE(result.tail_clock_end_uncertainty_ns.has_value());
  EXPECT_TRUE(tail_clock_anchors_consistent(
      TailClockAnchor{*result.tail_clock_start_realtime_epoch_ns, 0U,
                      *result.tail_clock_start_uncertainty_ns},
      TailClockAnchor{*result.tail_clock_end_realtime_epoch_ns,
                      *result.tail_clock_end_steady_elapsed_ns,
                      *result.tail_clock_end_uncertainty_ns}));
  EXPECT_EQ(result.telemetry_dropped_samples, 0U);
  EXPECT_FALSE(result.sampler_error);
}

TEST(EngineTailTelemetryTest, DropsSamplesAtTheReservedRecordCapacity) {
  EngineTailTelemetry telemetry(0);
  ASSERT_TRUE(telemetry.reserve());
  ASSERT_TRUE(telemetry.begin_measured());

  for (std::size_t index = 0; index < EngineTailTelemetry::kMaxStateSamples; ++index) {
    telemetry.observe("wal_sync_latency_us", 1);
  }
  telemetry.observe("wal_sync_latency_us", 1);
  telemetry.stop_collection();

  const auto result = telemetry.summary();
  EXPECT_EQ(result.measured_sync_count, EngineTailTelemetry::kMaxStateSamples);
  EXPECT_EQ(result.telemetry_dropped_samples, 1U);
}

TEST(EngineTailTelemetryTest, RecordsDrainBoundarySnapshots) {
  EngineTailTelemetry telemetry(2);
  ASSERT_TRUE(telemetry.reserve());
  ASSERT_TRUE(telemetry.begin_measured());
  ASSERT_TRUE(telemetry.begin_drain());

  order_books::MetricsSnapshot first;
  first.queue_depth = 3;
  first.event_publish_lag_events = 5;
  first.event_publish_lag_bytes = 7;
  first.event_publish_lag_age_ns = 11;
  order_books::MetricsSnapshot last;
  last.queue_depth = 13;
  last.event_publish_lag_events = 17;
  last.event_publish_lag_bytes = 19;
  last.event_publish_lag_age_ns = 23;
  ASSERT_FALSE(telemetry.record_drain_snapshot(
      last, TailTelemetryBoundary::drain_end));
  ASSERT_TRUE(telemetry.record_drain_snapshot(first,
                                              TailTelemetryBoundary::drain_start));
  ASSERT_FALSE(telemetry.record_drain_snapshot(
      first, TailTelemetryBoundary::drain_start));
  ASSERT_TRUE(telemetry.record_drain_snapshot(last,
                                              TailTelemetryBoundary::drain_end));
  ASSERT_FALSE(telemetry.record_drain_snapshot(
      last, TailTelemetryBoundary::drain_end));
  telemetry.stop_collection();

  const auto result = telemetry.summary();
  EXPECT_EQ(result.drain_state_sample_count, 2U);
  EXPECT_EQ(result.drain_publisher_lag_events_first, 5U);
  EXPECT_EQ(result.drain_publisher_lag_bytes_first, 7U);
  EXPECT_EQ(result.drain_publisher_lag_age_ns_first, 11U);
  EXPECT_EQ(result.drain_publisher_lag_events_last, 17U);
  EXPECT_EQ(result.drain_publisher_lag_bytes_last, 19U);
  EXPECT_EQ(result.drain_publisher_lag_age_ns_last, 23U);
}

TEST(EngineTailTelemetryTest, WritesSchemaAndDoesNotOverwriteArtifact) {
  EngineTailTelemetry telemetry(1);
  ASSERT_TRUE(telemetry.reserve());
  ASSERT_TRUE(telemetry.begin_measured());
  telemetry.observe("wal_sync_latency_us", 100);
  telemetry.observe("wal_group_commands", 1);
  telemetry.stop_collection();

  const auto path = unique_artifact_path();
  ASSERT_TRUE(telemetry.write_csv(path));
  std::ifstream input(path);
  ASSERT_TRUE(input.is_open());
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find(
                "record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,"),
            std::string::npos);
  EXPECT_NE(contents.find("sync,measured,"), std::string::npos);
  EXPECT_NE(contents.find("group_commands,measured,"), std::string::npos);

  ASSERT_FALSE(telemetry.write_csv(path));
  std::ifstream unchanged(path);
  ASSERT_TRUE(unchanged.is_open());
  std::string unchanged_contents((std::istreambuf_iterator<char>(unchanged)),
                                 std::istreambuf_iterator<char>());
  EXPECT_EQ(unchanged_contents, contents);

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

}  // namespace
}  // namespace order_books::benchmark
