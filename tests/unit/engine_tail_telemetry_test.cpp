#include "engine_tail_telemetry.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

namespace order_books::benchmark {
namespace {

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
  EXPECT_EQ(result.telemetry_dropped_samples, 0U);
  EXPECT_FALSE(result.sampler_error);
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
